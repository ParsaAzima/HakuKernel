/* Haku x86-64 hook for the shared ACPI parser.
 *
 * The parser follows firmware pointers that can name a table anywhere in
 * physical memory, including regions the physmap deliberately leaves out.
 * It therefore asks this hook for a window; two pages at K64_ACPI_WINDOW
 * (PML4 slot 384) are enough for any structure the parser reads, and the
 * window is rebuilt by paging64_map_page, which takes its intermediate
 * tables from the boot allocator.
 */
#include "acpi.h"
#include "haku64.h"

void *acpi_arch_map_table(u32 phys) {
    u64 base = (u64)phys & K64_PAGE_MASK;
    u32 page;

    for (page = 0; page < K64_ACPI_WINDOW_PAGES; page++) {
        u64 offset = (u64)page * K64_PAGE_SIZE;
        if (!paging64_map_page(K64_ACPI_WINDOW + offset, base + offset, 1))
            return 0;
    }
    return (void *)K64_ACPI_WINDOW;
}
