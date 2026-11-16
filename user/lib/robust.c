/* Haku 32-bit robust futex list registration.
 *
 * The kernel walks a task's robust list when that task exits: every mutex it
 * still owns is marked owner-died and one waiter is woken, so a lock is never
 * left held by a thread that no longer exists. The list itself is a user
 * structure whose head the thread registers once with set_robust_list(311).
 *
 * Two decisions shape this file.
 *
 * The head cannot live in the thread control block. A thread ends by
 * unmapping its own stack and control block and then issuing the exit
 * syscall, and the kernel walks the list during that syscall - after the
 * unmap. The heads therefore live in this image's .bss, one per task slot,
 * indexed by the task id, and nothing a thread frees can take them away.
 *
 * The head is also not recycled with the task slot. The kernel hands the ids
 * of dead tasks out again, so a stale "already registered" flag would leave
 * the next task with that id invisible to the exit walk. `owner` records
 * which task id the head was registered for, the first lock of a task
 * re-registers when it does not match, and the exit path clears it.
 */
#include <haku/abi.h>
#include <haku/errno.h>
#include <haku/pthread.h>
#include <haku/syscall.h>
#include <haku/unistd.h>

#include "tls_internal.h"

typedef struct {
    haku_robust_head_t head;    /* exactly the layout the kernel reads */
    int owner;                  /* task id it was registered for, 0 = none */
} robust_slot_t;

/* Task ids are 1..HAKU_PTHREAD_MAX_THREADS, so index 0 is never used and the
 * array does not have to be biased. */
static robust_slot_t slots[HAKU_PTHREAD_MAX_THREADS + 1];

static int slot_index(int tid) {
    return (tid > 0 && tid <= HAKU_PTHREAD_MAX_THREADS) ? tid : 0;
}

haku_robust_head_t *haku_robust_ensure(void) {
    int tid = slot_index(gettid());
    robust_slot_t *slot;

    if (!tid) return 0;         /* id out of range: no robust mutexes */
    slot = &slots[tid];
    if (slot->owner == tid) return &slot->head;
    /* A fresh head: no nodes, the offset that maps a node back to the futex
     * word of this library's mutex, and nothing pending. A null `next` is
     * what ends the kernel's walk, so an empty list needs no sentinel. */
    slot->head.next = 0;
    slot->head.futex_offset = -HAKU_PTHREAD_NODE_OFFSET;
    slot->head.pending = 0;
    if ((int)haku_syscall2(HAKU_SYS_SET_ROBUST_LIST, (int)&slot->head,
                           HAKU_ROBUST_LIST_SIZE) < 0)
        return 0;
    slot->owner = tid;
    return &slot->head;
}

void haku_robust_forget(void) {
    int tid = slot_index(gettid());
    if (tid) slots[tid].owner = 0;
}

void haku_robust_forget_all(void) {
    /* A fixed 17-entry table: the linear pass is deliberate and bounded by
     * HAKU_PTHREAD_MAX_THREADS, not by anything a program can control. */
    for (int i = 0; i <= HAKU_PTHREAD_MAX_THREADS; i++)
        slots[i].owner = 0;
}

int haku_tls_register(void) {
    return haku_robust_ensure() ? 0 : -(int)HAKU_EINVAL;
}
