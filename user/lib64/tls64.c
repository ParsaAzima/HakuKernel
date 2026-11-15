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
 *
 * The template is recorded once at startup and haku64_tls_new() makes one
 * TEB plus one block per thread from it, so a thread created by
 * haku64_thread_create() gets its own copy of every __declspec(thread)
 * variable instead of sharing the startup thread's.
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

/* The image's PT_TLS, recorded by haku64_tls_init() because the auxiliary
 * vector is only available on the startup stack. A zero memsz means the
 * image has no thread variables, which is possible but not the norm: the
 * linker always emits the two marker bytes. */
static struct {
    const void *vaddr;
    size_t filesz;
    size_t memsz;
} tls_image;

/* Offsets inside the TEB the compiler reads. +0x30 and +0x58 are the
 * Windows definitions; nothing else in this program interprets the page. */
#define HAKU64_TEB_SELF   0x30
#define HAKU64_TEB_SLOTS  0x58
/* The slots array starts at +0x100 so it cannot overlap the fields above.
 * A static executable has one module, so only slot 0 is ever read. */
#define HAKU64_TEB_SLOT0  0x100

/* The block is preceded by one size word so haku64_tls_free() can unmap it
 * without keeping a side table. The pointer handed to the compiler is the
 * first byte *after* that word, so every template offset stays relative to
 * the linker's view. */
#define HAKU64_TLS_HEADER 16u

static unsigned long long auxv_value(const unsigned long long *auxv,
                                     unsigned long long key) {
    if (!auxv) return 0;
    for (unsigned long long i = 0; auxv[i] != 0; i += 2)
        if (auxv[i] == key) return auxv[i + 1];
    return 0;
}

/* Build one thread's TEB and TLS block from the recorded template. Returns
 * the TEB address (the value a GS base wants), or NULL with errno set. The
 * block is a copy of the template with the virgin part zeroed, which is
 * exactly what a new thread must see. */
void *haku64_tls_new(void) {
    size_t block_size = (tls_image.memsz + 15u) & ~(size_t)15u;
    unsigned char *allocation = NULL;
    unsigned char *block = NULL;
    unsigned char *teb;
    unsigned long long *slots;

    if (block_size) {
        allocation = (unsigned char *)mmap(NULL, block_size + HAKU64_TLS_HEADER,
                                           HAKU64_PROT_READ |
                                               HAKU64_PROT_WRITE,
                                           HAKU64_MAP_PRIVATE |
                                               HAKU64_MAP_ANONYMOUS,
                                           -1, 0);
        if (allocation == (void *)-1) return NULL;
        block = allocation + HAKU64_TLS_HEADER;
        if (tls_image.filesz)
            memcpy(block, tls_image.vaddr, tls_image.filesz);
        /* mmap already zeroed [filesz, memsz), which is the template's BSS. */
        *(size_t *)allocation = block_size;
    }

    /* One page is plenty: the only fields that matter are the Self pointer
     * at +0x30 and the TlsSlots pointer at +0x58, with the slots array at
     * +0x100. */
    teb = (unsigned char *)mmap(NULL, 4096,
                                HAKU64_PROT_READ | HAKU64_PROT_WRITE,
                                HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS,
                                -1, 0);
    if (teb == (void *)-1) {
        if (allocation) munmap(allocation, block_size + HAKU64_TLS_HEADER);
        return NULL;
    }
    slots = (unsigned long long *)(teb + HAKU64_TEB_SLOT0);
    *(unsigned long long *)(teb + HAKU64_TEB_SELF) = (unsigned long long)teb;
    *(unsigned long long *)(teb + HAKU64_TEB_SLOTS) =
        (unsigned long long)slots;
    slots[0] = (unsigned long long)block;
    return teb;
}

/* Release a TEB and its block. Only safe once no task can touch them, which
 * is why the thread API does not call it: a thread's resources belong to it
 * until the process exits. haku64_thread_create() uses it on the failure
 * path, where the child was never created. */
void haku64_tls_free(void *teb) {
    unsigned char *page = (unsigned char *)teb;
    unsigned long long *slots;
    unsigned char *block;
    size_t block_size;

    if (!page) return;
    slots = *(unsigned long long **)(page + HAKU64_TEB_SLOTS);
    if (slots && slots[0]) {
        block = (unsigned char *)slots[0] - HAKU64_TLS_HEADER;
        block_size = *(size_t *)block;
        munmap(block, block_size + HAKU64_TLS_HEADER);
    }
    munmap(page, 4096);
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
    void *teb;

    tls_image.vaddr = NULL;
    tls_image.filesz = 0;
    tls_image.memsz = 0;
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
    tls_image.vaddr = (const void *)tls->vaddr;
    tls_image.filesz = (size_t)tls->filesz;
    tls_image.memsz = (size_t)tls->memsz;

    teb = haku64_tls_new();
    if (!teb) return 1;
    if (arch_prctl(HAKU64_ARCH_SET_GS, (unsigned long long)teb) != 0) {
        haku64_tls_free(teb);
        return 1;
    }
    return 0;
}
