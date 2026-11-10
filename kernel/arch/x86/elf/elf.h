/* Minimal ELF32 loader for static i386 ET_EXEC images. */
#ifndef HAKU_ELF_H
#define HAKU_ELF_H

#include "paging.h"

typedef struct {
    address_space_t address_space;
    uint32_t entry;
    uint32_t user_stack;
    /* Where the program headers ended up in the loaded image. A static libc
     * walks them from AT_PHDR to find its PT_TLS segment, so the value is
     * the runtime address, not the file offset. When the image does not
     * carry the table inside a segment, the loader maps a copy just below
     * the stack and points here instead. */
    uint32_t phdr_address;
    uint32_t phdr_count;
    uint32_t phdr_size;
} loaded_elf_t;

int elf_load_executable(const uint8_t *image, uint32_t image_size,
                        loaded_elf_t *loaded);

/* Reason the last elf_load_executable() call failed, for the boot log.
 * Never NULL; "no error" until a load is rejected. */
const char *elf_last_error(void);

#endif
