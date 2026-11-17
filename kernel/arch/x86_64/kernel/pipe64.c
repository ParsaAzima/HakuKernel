/* Haku x86-64 pipes: a bounded byte ring shared by one read and one write
 * description.
 *
 * The ring is a fixed PIPE64_CAPACITY window allocated with kmalloc64() when
 * the pipe is created and released when the last description of both ends has
 * closed. head counts the bytes ever written and tail the bytes ever read, so
 * their difference is the fill level and the free space without a modulo in
 * the hot path; a ring index is a counter modulo the capacity.
 *
 * readers and writers count open *descriptions*, not descriptors. dup() and
 * fork() share a description and therefore change neither count: only
 * fs64_fd_close() calls pipe64_unref() once a description's own reference
 * count has reached zero. The last writer to go makes read() return 0 (EOF)
 * after the buffered bytes; a write that finds no reader left is reported to
 * the syscall layer, which raises SIGPIPE.
 *
 * Every helper takes pipe_lock for one bounded step: a 64-byte staging copy
 * at most. The lock also owns interrupts, so a holder is never preempted, and
 * the lock is always released before the caller can block. The readiness
 * helpers take it too instead of reading head/tail unlocked: a writer publishes
 * under the lock, so a reader either sees the new fill level or parks and is
 * woken by the next write, which is the same lost-wakeup argument the futex
 * path uses.
 *
 * The limits are stated rather than hidden: eight pipes at once, each 4 KiB.
 * Creating the ninth is -ENFILE rather than an allocation that would be
 * impossible to reason about during a signal or a fork.
 */
#include "haku64.h"

#define PIPE64_MAX      8u
#define PIPE64_CAPACITY 4096u

typedef struct {
    u32 used;
    u32 readers;               /* open read descriptions */
    u32 writers;               /* open write descriptions */
    u64 head;                  /* bytes written since creation */
    u64 tail;                  /* bytes read since creation */
    u8 *data;                  /* ring of PIPE64_CAPACITY bytes */
} pipe64_t;

static pipe64_t pipes[PIPE64_MAX];
static spinlock64_t pipe_lock = SPINLOCK64_INIT;

static int pipe_index_ok(int index) {
    return index >= 0 && index < (int)PIPE64_MAX;
}

/* Copy `count` bytes out of the ring at `tail`, wrapping at most once. The
 * caller holds the lock and has already checked that at least `count` bytes
 * are buffered. */
static void ring_copy_out(pipe64_t *pipe, void *dst, u32 count) {
    u32 start = (u32)(pipe->tail % PIPE64_CAPACITY);
    u32 first = PIPE64_CAPACITY - start;

    if (first > count) first = count;
    memcpy(dst, pipe->data + start, first);
    if (count > first)
        memcpy((u8 *)dst + first, pipe->data, count - first);
}

/* Copy `count` bytes into the ring at `head`, wrapping at most once. */
static void ring_copy_in(pipe64_t *pipe, const void *src, u32 count) {
    u32 start = (u32)(pipe->head % PIPE64_CAPACITY);
    u32 first = PIPE64_CAPACITY - start;

    if (first > count) first = count;
    memcpy(pipe->data + start, src, first);
    if (count > first)
        memcpy(pipe->data, (const u8 *)src + first, count - first);
}

/* Claim a pipe slot with one description on each end. The ring is allocated
 * before the slot is published so a second CPU never sees a pipe without its
 * storage. Returns the index, or -1 when the ring or the slot table is full. */
int pipe64_alloc(void) {
    u8 *data = kmalloc64(PIPE64_CAPACITY);
    int index = -1;
    u64 flags;

    if (!data) return -1;
    flags = spin64_lock(&pipe_lock);
    for (u32 i = 0; i < PIPE64_MAX; i++) {
        if (pipes[i].used) continue;
        index = (int)i;
        pipes[i].used = 1;
        pipes[i].readers = 1;
        pipes[i].writers = 1;
        pipes[i].head = 0;
        pipes[i].tail = 0;
        pipes[i].data = data;
        break;
    }
    spin64_unlock(&pipe_lock, flags);
    if (index < 0) kfree64(data);
    return index;
}

/* Release one description of `writer`'s end. When the last description of
 * both ends is gone, the ring is freed. The free happens after the lock is
 * dropped because kfree64() is allowed to take its own lock. */
void pipe64_unref(int index, int writer) {
    u8 *data = 0;
    u64 flags;

    if (!pipe_index_ok(index)) return;
    flags = spin64_lock(&pipe_lock);
    if (pipes[index].used) {
        if (writer) {
            if (pipes[index].writers) pipes[index].writers--;
        } else {
            if (pipes[index].readers) pipes[index].readers--;
        }
        if (!pipes[index].readers && !pipes[index].writers) {
            data = pipes[index].data;
            pipes[index].used = 0;
            pipes[index].data = 0;
        }
    }
    spin64_unlock(&pipe_lock, flags);
    if (data) kfree64(data);
}

/* Buffered bytes. */
u32 pipe64_readable(int index) {
    u32 filled = 0;
    u64 flags;

    if (!pipe_index_ok(index)) return 0;
    flags = spin64_lock(&pipe_lock);
    if (pipes[index].used)
        filled = (u32)(pipes[index].head - pipes[index].tail);
    spin64_unlock(&pipe_lock, flags);
    return filled;
}

/* Room left before write() has to wait. */
u32 pipe64_writable(int index) {
    u32 room = 0;
    u64 flags;

    if (!pipe_index_ok(index)) return 0;
    flags = spin64_lock(&pipe_lock);
    if (pipes[index].used)
        room = PIPE64_CAPACITY - (u32)(pipes[index].head - pipes[index].tail);
    spin64_unlock(&pipe_lock, flags);
    return room;
}

int pipe64_readers(int index) {
    int count = 0;
    u64 flags;

    if (!pipe_index_ok(index)) return 0;
    flags = spin64_lock(&pipe_lock);
    if (pipes[index].used) count = (int)pipes[index].readers;
    spin64_unlock(&pipe_lock, flags);
    return count;
}

int pipe64_writers(int index) {
    int count = 0;
    u64 flags;

    if (!pipe_index_ok(index)) return 0;
    flags = spin64_lock(&pipe_lock);
    if (pipes[index].used) count = (int)pipes[index].writers;
    spin64_unlock(&pipe_lock, flags);
    return count;
}

/* Take what is buffered, up to `count` bytes. Returns the number copied, 0
 * when the ring is empty. A dead pipe or a bad index is -EINVAL. */
int pipe64_read(int index, void *dst, u32 count) {
    pipe64_t *pipe;
    u32 n;
    u64 flags;

    if (!pipe_index_ok(index) || !dst) return -22;
    if (count == 0) return 0;
    flags = spin64_lock(&pipe_lock);
    pipe = &pipes[index];
    if (!pipe->used) {
        spin64_unlock(&pipe_lock, flags);
        return -22;
    }
    n = (u32)(pipe->head - pipe->tail);
    if (n > count) n = count;
    if (!n) {
        spin64_unlock(&pipe_lock, flags);
        return 0;
    }
    ring_copy_out(pipe, dst, n);
    pipe->tail += n;
    spin64_unlock(&pipe_lock, flags);
    return (int)n;
}

/* Put up to `count` bytes into the free part of the ring. Returns the number
 * copied, 0 when the ring is full. */
int pipe64_write(int index, const void *src, u32 count) {
    pipe64_t *pipe;
    u32 n;
    u64 flags;

    if (!pipe_index_ok(index) || !src) return -22;
    if (count == 0) return 0;
    flags = spin64_lock(&pipe_lock);
    pipe = &pipes[index];
    if (!pipe->used) {
        spin64_unlock(&pipe_lock, flags);
        return -22;
    }
    n = PIPE64_CAPACITY - (u32)(pipe->head - pipe->tail);
    if (n > count) n = count;
    if (!n) {
        spin64_unlock(&pipe_lock, flags);
        return 0;
    }
    ring_copy_in(pipe, src, n);
    pipe->head += n;
    spin64_unlock(&pipe_lock, flags);
    return (int)n;
}
