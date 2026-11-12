/* Haku x86-64 SMP bring-up.
 *
 * Stage B mirrors the 32-bit bring-up on the long-mode path: find the CPUs
 * through ACPI, enable the local APIC, copy the trampoline to 0x8000 and
 * start every other processor with INIT-SIPI-SIPI. Each AP walks the
 * trampoline through real mode, protected mode with PAE, and long mode, then
 * calls smp64_ap_entry() and parks. The bootstrap processor remains the only
 * CPU that runs the scheduler, exactly as on the 32-bit build: per-CPU
 * scheduler state and a per-CPU TSS come first (stage C).
 *
 * Unlike the 32-bit trampoline there is no CR3 to build. The AP reuses the
 * BSP's PML4, which already identity maps the low 1 GiB and the whole
 * physical map, so the trampoline only has to load EFER and CR0.
 */
#include "acpi.h"
#include "apic.h"
#include "haku64.h"
#include "sync.h"

#define SMP64_TRAMPOLINE_BASE 0x8000ull
#define SMP64_TRAMPOLINE_VECTOR 0x08u
/* Parameter block at the top of the trampoline page; must match
 * ap_trampoline64.asm. Four 64-bit slots, above the real-mode stack. */
#define SMP64_PARAM_BASE 0x8F00ull

#define SMP64_START_ATTEMPTS 3u
#define SMP64_ONLINE_SPINS 200000u
/* Increments each CPU contributes to the cross-CPU lock test; see the 32-bit
 * smp.c for why the counter update itself is deliberately not atomic. */
#define SMP64_LOCK_ITERATIONS 50000u

typedef struct {
    u32 apic_id;
    u64 stack_phys;
    u64 stack_top;
    volatile u32 online;
    volatile u32 done;
    volatile u32 gdt_ok;
} smp64_cpu_t;

static smp64_cpu_t cpus[K64_SMP_MAX_CPUS];
static u32 cpu_count;

static mutex_t lock_test = MUTEX_INIT;
static volatile u32 lock_counter;

/* From ap_trampoline64.asm. The blob is copied, not executed in place. */
extern void ap_trampoline64_start(void);
extern void ap_trampoline64_end(void);
extern void smp_memory_barrier64(void);

/* Running on the AP's private stack with the trampoline's bootstrap GDT.
 * Nothing here may touch shared scheduler state; the BSP owns it. */
void smp64_ap_entry(u64 cpu) {
    u32 i;

    if (cpu < (u64)K64_SMP_MAX_CPUS) {
        /* Switch from the trampoline's bootstrap table to this CPU's own
         * GDT/TSS and the shared IDT before doing anything else. */
        if (gdt64_load_ap((u32)cpu)) {
            idt64_load_ap();
            cpus[cpu].gdt_ok = 1;
        }
        cpus[cpu].online = 1;
    }

    for (i = 0; i < SMP64_LOCK_ITERATIONS; i++) {
        mutex_lock(&lock_test);
        lock_counter++;
        mutex_unlock(&lock_test);
    }

    if (cpu < (u64)K64_SMP_MAX_CPUS) cpus[cpu].done = 1;

    for (;;) {
        cli64();
        cpu_halt64();
    }
}

static int start_ap(u32 index, u32 apic_id) {
    volatile u64 *params = (volatile u64 *)SMP64_PARAM_BASE;
    u64 blob_size = (u64)ap_trampoline64_end - (u64)ap_trampoline64_start;
    u64 stack_phys = frame64_alloc();
    u64 stack_top;
    u32 attempt;
    u32 spin;

    if (!stack_phys) return 0;
    cpus[index].stack_phys = stack_phys;
    stack_top = (u64)phys_to_virt64(stack_phys) + K64_PAGE_SIZE;
    cpus[index].stack_top = stack_top;
    /* Build the descriptor table here, while only the BSP uses the frame
     * allocator; the AP only loads it. */
    if (!gdt64_prepare_ap(index, stack_top)) {
        frame64_free(stack_phys);
        cpus[index].stack_phys = 0;
        return 0;
    }

    memcpy((void *)SMP64_TRAMPOLINE_BASE, (void *)ap_trampoline64_start,
           blob_size);
    params[0] = read_cr3_64() & K64_PAGE_MASK;  /* BSP's PML4 */
    params[1] = stack_top;
    params[2] = index;
    params[3] = (u64)smp64_ap_entry;

    for (attempt = 0; attempt < SMP64_START_ATTEMPTS; attempt++) {
        /* Stores to the trampoline page and the parameter block must be
         * globally visible before the INIT/SIPI sequence; mfence is the
         * x64 equivalent of the 32-bit build's barrier. */
        smp_memory_barrier64();
        apic_send_init(apic_id);
        apic_delay(2000);
        apic_send_sipi(apic_id, SMP64_TRAMPOLINE_VECTOR);
        apic_delay(2000);
        apic_send_sipi(apic_id, SMP64_TRAMPOLINE_VECTOR);
        for (spin = 0; spin < SMP64_ONLINE_SPINS; spin++) {
            if (cpus[index].online) return 1;
            apic_delay(10);
        }
    }
    return 0;
}

int smp64_init(void) {
    const acpi_info_t *info;
    u32 bsp_id;
    u32 i;
    u32 spin;
    u32 expected;

    memset(cpus, 0, sizeof(cpus));
    cpu_count = 0;
    lock_counter = 0;
    lock_test.locked = 0;

    if (!acpi_init() || !acpi_info()->have_madt) {
        k64_puts("HAKU64-SMP-SKIP: no ACPI MADT\r\n");
        return 0;
    }
    info = acpi_info();
    if (!apic_init()) {
        k64_puts("HAKU64-SMP-SKIP: local APIC did not map\r\n");
        return 0;
    }

    bsp_id = apic_id();
    cpus[0].apic_id = bsp_id;
    cpus[0].online = 1;
    cpu_count = 1;

    /* One pass over the MADT entries; the table is tiny and this runs once. */
    for (i = 0; i < info->cpu_count && cpu_count < K64_SMP_MAX_CPUS; i++) {
        u32 id = info->cpus[i].apic_id;
        if (!info->cpus[i].enabled || id == bsp_id) continue;
        if (start_ap(cpu_count, id)) {
            k64_puts("HAKU64-SMP-AP-OK cpu=");
            k64_dec(cpu_count);
            k64_puts(" apic=");
            k64_dec(id);
            k64_puts("\r\n");
            cpu_count++;
        } else {
            k64_puts("HAKU64-SMP-AP-FAIL apic=");
            k64_dec(id);
            k64_puts("\r\n");
        }
    }

    k64_puts("HAKU64-SMP-OK: cpus=");
    k64_dec(cpu_count);
    k64_puts(" lapic=");
    k64_hex(info->lapic_phys);
    k64_puts(" madt=");
    k64_dec(info->cpu_count);
    k64_puts(" parked=1\r\n");

    /* The BSP takes the same share as each AP while the APs run theirs, so
     * the counter update has to be protected by a lock that is atomic across
     * CPUs, not by a local interrupt mask. */
    for (i = 0; i < SMP64_LOCK_ITERATIONS; i++) {
        mutex_lock(&lock_test);
        lock_counter++;
        mutex_unlock(&lock_test);
    }
    for (i = 1; i < cpu_count; i++) {
        for (spin = 0; spin < SMP64_ONLINE_SPINS; spin++) {
            if (cpus[i].done) break;
            apic_delay(10);
        }
    }

    /* Each AP switched to its own GDT/TSS: prove the TSS carries that CPU's
     * stack, which is what a ring-3 entry would use. */
    for (i = 1; i < cpu_count; i++) {
        if (cpus[i].gdt_ok && gdt64_ap_rsp0(i) == cpus[i].stack_top) {
            k64_puts("HAKU64-SMP-AP-GDT-OK cpu=");
        } else {
            k64_puts("HAKU64-SMP-AP-GDT-FAIL cpu=");
        }
        k64_dec(i);
        k64_puts(" rsp0=");
        k64_hex(gdt64_ap_rsp0(i));
        k64_puts("\r\n");
    }

    expected = SMP64_LOCK_ITERATIONS * cpu_count;
    if (lock_counter == expected) {
        k64_puts("HAKU64-SMP-LOCK-OK counter=");
    } else {
        k64_puts("HAKU64-SMP-LOCK-FAIL counter=");
    }
    k64_dec(lock_counter);
    k64_puts(" expected=");
    k64_dec(expected);
    k64_puts("\r\n");
    return (int)cpu_count;
}

u32 smp64_cpu_count(void) {
    return cpu_count;
}
