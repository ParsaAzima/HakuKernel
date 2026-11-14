/* Haku x86-64 thread-local storage.
 *
 * The compiler turns __declspec(thread) into the Windows x64 access pattern:
 *
 *     mov eax, [_tls_index]        ; 0 for the executable
 *     mov rcx, gs:[0x58]           ; TEB->TlsSlots
 *     mov rax, [rcx + rax*8]       ; the module's TLS block
 *     mov ... , [rax + offset]
 *
 * So a working TLS needs three things this file provides: the `_tls_index`
 * symbol the compiler references, a TEB whose +0x58 word points at a
 * TlsSlots array, and a copy of the linker's TLS template at a user GS
 * base. The template is discovered at runtime from the ELF program headers
 * (PT_TLS) that tools/pe2elf64.py emits from the PE TLS directory; nothing
 * here assumes a link-time address.
 *
 * `_tls_used` is the structure the MSVC linker recognizes as "this image
 * has a TLS directory". The two markers bracket the template:
 * `_haku_tls_start` in .tls sorts before every .tls$ contribution and
 * `_haku_tls_end` in .tls$ZZZ sorts after them, so [start, end) covers the
 * compiler's variables with no need to know their names. The marker bytes
 * themselves are harmless padding, and the compiler's offsets stay correct
 * because they are relative to the same template the linker built.
 */
#include <haku/abi64.h>

/* The compiler's TLS access reads this DWORD. A static executable has one
 * module, so index 0 is the only valid value. */
unsigned long _tls_index = 0;

#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
#pragma section(".rdata$T", read)

__declspec(allocate(".tls")) char _haku_tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _haku_tls_end = 0;

typedef struct {
    unsigned long long start;
    unsigned long long end;
    unsigned long long index;
    unsigned long long callbacks;
    unsigned int zero_fill;
    unsigned int characteristics;
} haku64_tls_directory_t;

__declspec(allocate(".rdata$T"))
const haku64_tls_directory_t _tls_used = {
    (unsigned long long)&_haku_tls_start,
    (unsigned long long)&_haku_tls_end,
    (unsigned long long)&_tls_index,
    0,
    0,
    0
};

typedef struct {
    unsigned int type;
    unsigned int flags;
    unsigned long long offset;
    unsigned long long vaddr;
    unsigned long long paddr;
    unsigned long long filesz;
    unsigned long long memsz;
    unsigned long long align;
} haku64_phdr_t;

static unsigned long long auxv_value(const unsigned long long *auxv,
                                     unsigned long long key) {
    if (!auxv) return 0;
    for (unsigned long long i = 0; auxv[i] != 0; i += 2)
        if (auxv[i] == key) return auxv[i + 1];
    return 0;
}

/* Called by crt64.asm before main(), with the auxiliary vector the kernel
 * built. Returns 0 when there is nothing to do or the setup succeeded, and
 * 1 when a mapping or arch_prctl failed; the startup stub turns that into a
 * non-zero exit instead of letting a TLS access fault later. */
int haku64_tls_init(const unsigned long long *auxv) {
    unsigned long long phdr = auxv_value(auxv, HAKU64_AT_PHDR);
    unsigned long long phnum = auxv_value(auxv, HAKU64_AT_PHNUM);
    unsigned long long phent = auxv_value(auxv, HAKU64_AT_PHENT);
    const unsigned char *table;
    const haku64_phdr_t *tls = NULL;
    unsigned char *block;
    unsigned char *teb;
    unsigned long long *slots;
    size_t block_size;

    if (!phdr || !phnum || phent != sizeof(haku64_phdr_t)) return 0;
    table = (const unsigned char *)phdr;
    for (unsigned long long i = 0; i < phnum; i++) {
        const haku64_phdr_t *entry =
            (const haku64_phdr_t *)(table + i * phent);

        if (entry->type == HAKU64_PT_TLS) {
            tls = entry;
            break;
        }
    }
    if (!tls || tls->memsz == 0 || tls->filesz > tls->memsz) return 0;

    block_size = (size_t)((tls->memsz + 15ull) & ~15ull);
    block = (unsigned char *)mmap(NULL, block_size,
                                  HAKU64_PROT_READ | HAKU64_PROT_WRITE,
                                  HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS,
                                  -1, 0);
    if (block == (void *)-1) return 1;
    if (tls->filesz)
        memcpy(block, (const void *)tls->vaddr, (size_t)tls->filesz);

    /* One page is plenty: the only field the compiler reads is the
     * TlsSlots pointer at +0x58. The Self pointer at +0x30 is filled in
     * because that is what makes a TEB a TEB, and the slots array lives at
     * +0x100 so the two never overlap. */
    teb = (unsigned char *)mmap(NULL, 4096,
                                HAKU64_PROT_READ | HAKU64_PROT_WRITE,
                                HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS,
                                -1, 0);
    if (teb == (void *)-1) return 1;
    slots = (unsigned long long *)(teb + 0x100);
    *(unsigned long long *)(teb + 0x30) = (unsigned long long)teb;
    *(unsigned long long *)(teb + 0x58) = (unsigned long long)slots;
    slots[0] = (unsigned long long)block;

    if (arch_prctl(HAKU64_ARCH_SET_GS, (unsigned long long)teb) != 0)
        return 1;
    return 0;
}
