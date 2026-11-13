/* Haku x86-64 kernel heap.
 *
 * The same first-fit, address-ordered free list the 32-bit heap uses, over
 * a fixed 8 MiB window at K64_HEAP_START. Headers are linked in address
 * order, so freeing coalesces with both neighbours in O(1).
 *
 * The window is identity mapped and reserved in the frame allocator, so it
 * is never handed out twice. A growable heap (backed by frames) is a later
 * step; 8 MiB is far more than the current kernel allocates.
 *
 * Once every CPU schedules, two of them can allocate at the same time (a
 * syscall building a task stack on one, a copy-on-write fault on the other),
 * so the free list is behind one lock. That lock is never held while calling
 * into the scheduler, and the scheduler's own lock is the outer one where the
 * two meet, so there is no order to get wrong.
 */
#include "haku64.h"

#define HEAP_MAGIC_FREE 0x48504652u /* 'HPFR' */
#define HEAP_MAGIC_USED 0x48505553u /* 'HPUS' */
#define HEAP_MIN_PAYLOAD 32ull

typedef struct heap_block64 {
    struct heap_block64 *prev;
    struct heap_block64 *next;
    u64 size;
    u32 magic;
} heap_block64_t;

static heap_block64_t *heap_head;
static spinlock64_t heap_lock = SPINLOCK64_INIT;

static u64 align16(u64 value) {
    return (value + 15ull) & ~15ull;
}

int heap64_init(void) {
    heap_block64_t *first = (heap_block64_t *)K64_HEAP_START;

    first->prev = 0;
    first->next = 0;
    first->size = K64_HEAP_SIZE - sizeof(heap_block64_t);
    first->magic = HEAP_MAGIC_FREE;
    heap_head = first;
    return 1;
}

u64 heap64_free_bytes(void) {
    u64 total = 0;
    u64 flags = spin64_lock(&heap_lock);

    for (heap_block64_t *block = heap_head; block; block = block->next)
        if (block->magic == HEAP_MAGIC_FREE) total += block->size;
    spin64_unlock(&heap_lock, flags);
    return total;
}

static void split(heap_block64_t *block, u64 need) {
    if (block->size >= need + sizeof(heap_block64_t) + HEAP_MIN_PAYLOAD) {
        heap_block64_t *rest =
            (heap_block64_t *)((u8 *)(block + 1) + need);
        rest->prev = block;
        rest->next = block->next;
        rest->size = block->size - need - sizeof(heap_block64_t);
        rest->magic = HEAP_MAGIC_FREE;
        if (block->next) block->next->prev = rest;
        block->next = rest;
        block->size = need;
    }
}

void *kmalloc64(u64 bytes) {
    heap_block64_t *block;
    u64 flags;

    if (bytes == 0) return 0;
    bytes = align16(bytes);
    if (bytes > K64_HEAP_SIZE) return 0;   /* can never fit: skip the scan */

    flags = spin64_lock(&heap_lock);
    for (block = heap_head; block; block = block->next) {
        if (block->magic == HEAP_MAGIC_FREE && block->size >= bytes) {
            split(block, bytes);
            block->magic = HEAP_MAGIC_USED;
            spin64_unlock(&heap_lock, flags);
            return (void *)(block + 1);
        }
    }
    spin64_unlock(&heap_lock, flags);
    return 0;
}

void kfree64(void *ptr) {
    heap_block64_t *block;
    u64 address = (u64)ptr;
    u64 flags;

    if (!ptr || address < K64_HEAP_START + sizeof(heap_block64_t) ||
        address >= K64_HEAP_START + K64_HEAP_SIZE)
        return;
    flags = spin64_lock(&heap_lock);
    block = (heap_block64_t *)ptr - 1;
    if (block->magic != HEAP_MAGIC_USED) {   /* double free or corruption */
        spin64_unlock(&heap_lock, flags);
        return;
    }
    block->magic = HEAP_MAGIC_FREE;

    if (block->next && block->next->magic == HEAP_MAGIC_FREE) {
        block->size += sizeof(heap_block64_t) + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }
    if (block->prev && block->prev->magic == HEAP_MAGIC_FREE) {
        block->prev->size += sizeof(heap_block64_t) + block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
    }
    spin64_unlock(&heap_lock, flags);
}
