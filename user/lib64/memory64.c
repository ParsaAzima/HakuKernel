/* Haku x86-64 libc: malloc/free on top of brk().
 *
 * Same first-fit list with splitting and coalescing as user/lib/memory.c.
 * The 64-bit version keeps the header pointer-sized and checks the size
 * arithmetic at 64 bits, so a hostile size cannot wrap into a small-looking
 * value. The heap itself is the kernel's brk() window
 * (K64_USER_BRK_BASE..K64_USER_MMAP_BASE), which grows in zeroed pages.
 */
#include <haku/abi64.h>

#define BLOCK_MAGIC 0x48414B55u
#define GROW_SIZE   16384ull
#define MIN_SPLIT   16ull

typedef struct haku64_block {
    size_t size;
    struct haku64_block *next;
    struct haku64_block *prev;
    unsigned int free;
    unsigned int magic;
} haku64_block_t;

static haku64_block_t *block_list;
static haku64_block_t *block_tail;

/* Keep every block 16-byte aligned, which is what x86-64 code and the
 * compiler-generated SSE spills expect from a malloc result. */
static size_t align_size(size_t size) {
    return (size + 15ull) & ~15ull;
}

static void split_block(haku64_block_t *block, size_t wanted) {
    haku64_block_t *rest;
    if (block->size < wanted + sizeof(haku64_block_t) + MIN_SPLIT) return;
    rest = (haku64_block_t *)((char *)(block + 1) + wanted);
    rest->size = block->size - wanted - sizeof(haku64_block_t);
    rest->next = block->next;
    rest->prev = block;
    rest->free = 1;
    rest->magic = BLOCK_MAGIC;
    if (rest->next)
        rest->next->prev = rest;
    else
        block_tail = rest;
    block->next = rest;
    block->size = wanted;
}

static haku64_block_t *grow_heap(size_t wanted) {
    size_t chunk = wanted + sizeof(haku64_block_t);
    haku64_block_t *block;
    if (chunk < wanted) return NULL;
    if (chunk < GROW_SIZE) chunk = GROW_SIZE;
    chunk = (chunk + 0xFFFull) & ~0xFFFull;
    block = (haku64_block_t *)sbrk((long long)chunk);
    if (block == (void *)-1) return NULL;
    if (block_tail && block_tail->free &&
        (char *)(block_tail + 1) + block_tail->size == (char *)block) {
        block_tail->size += chunk;
        return block_tail;
    }
    block->size = chunk - sizeof(haku64_block_t);
    block->next = NULL;
    block->prev = block_tail;
    block->free = 1;
    block->magic = BLOCK_MAGIC;
    if (block_tail)
        block_tail->next = block;
    else
        block_list = block;
    block_tail = block;
    return block;
}

void *malloc(size_t size) {
    haku64_block_t *block;
    size_t wanted;

    if (size > 0x7FFFFFFFFFFFFFFFull) {
        errno = HAKU64_ENOMEM;
        return NULL;
    }
    wanted = align_size(size ? size : 1u);

    for (block = block_list; block; block = block->next) {
        if (block->free && block->size >= wanted) {
            split_block(block, wanted);
            block->free = 0;
            return block + 1;
        }
    }
    block = grow_heap(wanted);
    if (!block) {
        errno = HAKU64_ENOMEM;
        return NULL;
    }
    split_block(block, wanted);
    block->free = 0;
    return block + 1;
}

void free(void *pointer) {
    haku64_block_t *block;
    if (!pointer) return;
    block = ((haku64_block_t *)pointer) - 1;
    if (block->magic != BLOCK_MAGIC || block->free) return;
    block->free = 1;
    if (block->next && block->next->free) {
        block->size += sizeof(haku64_block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next)
            block->next->prev = block;
        else
            block_tail = block;
    }
    if (block->prev && block->prev->free) {
        haku64_block_t *previous = block->prev;
        previous->size += sizeof(haku64_block_t) + block->size;
        previous->next = block->next;
        if (previous->next)
            previous->next->prev = previous;
        else
            block_tail = previous;
    }
}

void *calloc(size_t count, size_t size) {
    void *pointer;
    size_t total;
    if (count && size > 0xFFFFFFFFFFFFFFFFull / count) {
        errno = HAKU64_ENOMEM;
        return NULL;
    }
    total = count * size;
    pointer = malloc(total);
    if (pointer) memset(pointer, 0, total);
    return pointer;
}

void *realloc(void *pointer, size_t size) {
    haku64_block_t *block;
    void *fresh;
    size_t copy;
    if (!pointer) return malloc(size);
    if (size == 0) {
        free(pointer);
        return NULL;
    }
    block = ((haku64_block_t *)pointer) - 1;
    if (block->magic != BLOCK_MAGIC) return NULL;
    if (block->size >= align_size(size)) {
        split_block(block, align_size(size));
        return pointer;
    }
    fresh = malloc(size);
    if (!fresh) return NULL;
    copy = block->size < size ? block->size : size;
    memcpy(fresh, pointer, copy);
    free(pointer);
    return fresh;
}

int atoi(const char *text) {
    int sign = 1;
    int value = 0;
    while (*text == ' ' || *text == '\t' || *text == '\n' ||
           *text == '\r' || *text == '\f' || *text == '\v')
        text++;
    if (*text == '-') {
        sign = -1;
        text++;
    } else if (*text == '+') {
        text++;
    }
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        text++;
    }
    return value * sign;
}
