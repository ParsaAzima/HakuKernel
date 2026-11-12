/* Haku ACPI table parsing: RSDP -> RSDT/XSDT -> MADT.
 *
 * Every read goes through acpi_arch_map_table(), which places the requested
 * physical page in a two-page scratch window. The window is therefore only
 * valid until the next call: each helper copies the fields it needs (or
 * finishes with the pointer) before stepping to the next page. Walks are
 * bounded by the tables' own lengths and by small hard caps so a corrupt
 * table cannot turn boot into an unbounded scan.
 */
#include "acpi.h"

#define ACPI_RSDP_SIG "RSD PTR "
#define ACPI_RSDT_SIG "RSDT"
#define ACPI_XSDT_SIG "XSDT"
#define ACPI_MADT_SIG "APIC"

#define ACPI_ROOT_MAX_TABLES 64u
#define ACPI_MADT_MAX_ENTRIES 256u
#define ACPI_TABLE_MAX_BYTES 0x100000u

static acpi_info_t info;

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int sig_equal(const uint8_t *p, const char *sig) {
    for (uint32_t i = 0; i < 4; i++) {
        if (p[i] != (uint8_t)sig[i]) return 0;
    }
    return 1;
}

static int checksum_ok(const uint8_t *p, uint32_t length) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

/* Pointer to `phys` inside the currently mapped window. The page offset is
 * preserved, so callers see the table bytes as written. */
static const uint8_t *map_at(uint32_t phys) {
    uint8_t *page = (uint8_t *)acpi_arch_map_table(phys);
    if (!page) return 0;
    return page + (phys & 0xFFFu);
}

static void add_cpu(uint32_t apic_id, int enabled) {
    if (info.cpu_count >= ACPI_MAX_CPUS) return;
    /* The xAPIC destination field is 8 bits wide. A processor whose ID does
     * not fit cannot be addressed by the IPI code this stage implements, so
     * leave it out instead of truncating it into a wrong CPU. */
    if (apic_id > 0xFFu) return;
    info.cpus[info.cpu_count].apic_id = (uint8_t)apic_id;
    info.cpus[info.cpu_count].enabled = (uint8_t)(enabled != 0);
    info.cpu_count++;
}

/* MADT: header (44 bytes) followed by variable-length entries. Type 0 is a
 * local APIC (the xAPIC id and the enable flag), type 5 overrides the MMIO
 * base, type 9 is the x2APIC form used by firmware that reports IDs above
 * 255. QEMU emits type 0 for the machines this kernel targets. */
static int parse_madt(uint32_t phys) {
    const uint8_t *hdr = map_at(phys);
    uint32_t length;
    uint32_t offset;
    uint32_t entries;

    if (!hdr || !sig_equal(hdr, ACPI_MADT_SIG)) return 0;
    length = read32(hdr + 4);
    if (length < 44 || length > ACPI_TABLE_MAX_BYTES) return 0;
    if (!checksum_ok(hdr, length)) return 0;

    info.have_madt = 1;
    /* Offset 36 is the 32-bit local-APIC MMIO address every xAPIC machine
     * uses; a type 5 entry may override it below. */
    if (read32(hdr + 36) != 0) info.lapic_phys = read32(hdr + 36);

    offset = 44;
    for (entries = 0; entries < ACPI_MADT_MAX_ENTRIES; entries++) {
        const uint8_t *entry;
        uint8_t type;
        uint8_t size;

        if (offset + 2 > length) break;
        entry = map_at(phys + offset);
        if (!entry) break;
        type = entry[0];
        size = entry[1];
        if (size < 2 || offset + size > length) break;

        if (type == 0 && size >= 8) {
            /* ACPI processor id (byte 2) is a firmware handle, not the APIC
             * id; the destination field is byte 3. */
            add_cpu(entry[3], (read32(entry + 4) & 1u) != 0);
        } else if (type == 9 && size >= 16) {
            add_cpu(read32(entry + 4), (read32(entry + 8) & 1u) != 0);
        } else if (type == 5 && size >= 12) {
            info.lapic_phys = read32(entry + 4);
            info.have_lapic_override = 1;
        }
        offset += size;
    }
    return 1;
}

/* Walk a root table's pointers. XSDT entries are 64-bit; a table above
 * 4 GiB cannot be reached through the 32-bit physical type, so it is
 * skipped rather than mis-addressed. */
static int parse_root(uint32_t root_phys, int use_xsdt) {
    const uint8_t *hdr = map_at(root_phys);
    uint32_t length;
    uint32_t entry_size;
    uint32_t count;

    if (!hdr) return 0;
    if (!sig_equal(hdr, use_xsdt ? ACPI_XSDT_SIG : ACPI_RSDT_SIG)) return 0;
    length = read32(hdr + 4);
    if (length < 36 || length > ACPI_TABLE_MAX_BYTES) return 0;
    if (!checksum_ok(hdr, length)) return 0;

    entry_size = use_xsdt ? 8u : 4u;
    count = (length - 36u) / entry_size;
    if (count > ACPI_ROOT_MAX_TABLES) count = ACPI_ROOT_MAX_TABLES;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = map_at(root_phys + 36u + i * entry_size);
        uint32_t table_phys;
        uint32_t table_high;

        if (!entry) return 0;
        table_phys = read32(entry);
        table_high = use_xsdt ? read32(entry + 4) : 0;
        if (table_high != 0 || table_phys == 0) continue;
        if (parse_madt(table_phys)) return 1;
    }
    return 0;
}

static int read_rsdp(uint32_t phys, uint32_t *rsdt, uint32_t *xsdt,
                     int *has_xsdt) {
    const uint8_t *p = map_at(phys);
    uint32_t revision;
    uint32_t length;

    if (!p || !sig_equal(p, ACPI_RSDP_SIG)) return 0;
    if (!checksum_ok(p, 20)) return 0;

    revision = p[15];
    *rsdt = read32(p + 16);
    *xsdt = 0;
    *has_xsdt = 0;
    if (revision >= 2) {
        length = read32(p + 20);
        if (length >= 36 && length <= 64 && checksum_ok(p, length)) {
            uint32_t low = read32(p + 24);
            uint32_t high = read32(p + 28);
            if (low != 0 && high == 0) {
                *xsdt = low;
                *has_xsdt = 1;
            }
        }
    }
    return 1;
}

/* RSDP lives either in the first KiB of the extended BIOS data area or in
 * the 0xE0000-0xFFFFF firmware window, on a 16-byte boundary. Both areas are
 * identity mapped, so the scan itself never faults. */
static int find_rsdp(uint32_t *rsdt, uint32_t *xsdt, int *has_xsdt) {
    uint32_t ebda = (uint32_t)(*(volatile uint16_t *)0x40E) << 4;

    if (ebda >= 0x80000u && ebda < 0xA0000u) {
        for (uint32_t p = ebda; p < ebda + 0x400u; p += 16u) {
            if (read_rsdp(p, rsdt, xsdt, has_xsdt)) return 1;
        }
    }
    for (uint32_t p = 0xE0000u; p < 0x100000u; p += 16u) {
        if (read_rsdp(p, rsdt, xsdt, has_xsdt)) return 1;
    }
    return 0;
}

int acpi_init(void) {
    uint32_t rsdt = 0;
    uint32_t xsdt = 0;
    int has_xsdt = 0;

    memset(&info, 0, sizeof(info));
    info.lapic_phys = ACPI_DEFAULT_LAPIC;

    if (!find_rsdp(&rsdt, &xsdt, &has_xsdt)) return 0;
    info.have_rsdp = 1;

    /* Prefer the 64-bit root table when the firmware provided one, but fall
     * back to the 32-bit table if it does not name a MADT. */
    if (has_xsdt && xsdt && parse_root(xsdt, 1)) return 1;
    if (rsdt && parse_root(rsdt, 0)) return 1;
    return 0;
}

const acpi_info_t *acpi_info(void) {
    return &info;
}
