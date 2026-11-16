#ifndef HAKU_PTHREAD_H
#define HAKU_PTHREAD_H

/* A small pthread subset shared by the 32-bit and the x86-64 library.
 *
 * The threads are real kernel threads: clone(CLONE_VM|CLONE_THREAD) gives
 * every one of them its own task id, its own kernel stack and its own thread
 * control block, so a thread that exits does not take the process down and
 * errno is per thread. What is here is the subset a freestanding libc can
 * support honestly:
 *
 *   thread     create, join, detach, self, exit
 *   mutex      lock, trylock, unlock, destroy, consistent
 *   cond       wait, signal, broadcast, destroy
 *
 * Deliberately absent: attributes (no stack size, scheduling or detach state
 * to choose), recursive and error-checking mutexes, read/write locks,
 * thread-specific data keys, cancellation, timed waits, and pthread_atfork.
 * A program that needs one of them should not be told it exists.
 *
 * A thread is detached at the kernel level on both ports, so a finished
 * thread never keeps a waitable zombie task slot: join and detach are
 * library state only, and the kernel reclaims the slot at its next tick.
 *
 * A mutex is robust: if the owning thread dies without unlocking, the kernel
 * marks the futex word owner-died, the next locker gets EOWNERDEAD, and
 * pthread_mutex_consistent() decides whether the mutex may be used again.
 * That needs the thread to have registered a robust futex list, which the
 * library does for every thread it starts.
 */

#if defined(_M_X64) || defined(__x86_64__)
#include <haku/abi64.h>
typedef long long haku_pthread_word_t;
#else
#include <haku/abi.h>
#include <haku/types.h>
typedef int haku_pthread_word_t;
#endif

/* Offset of the robust list node inside pthread_mutex_t below: four 32-bit
 * flags precede it on both architectures. The kernel reads the head's
 * futex_offset once and applies it to every node, so this is the one layout
 * constant both the library and the kernel's walk depend on. */
#define HAKU_PTHREAD_NODE_OFFSET 16

/* How many robust list heads one process keeps, i.e. the largest task id the
 * kernel can hand out (SCHEDULER_MAX_PROCESSES on i386, K64_TASK_MAX on
 * x86-64). A thread whose tid is outside this range still runs; only its
 * mutexes lose the robust takeover behaviour. */
#define HAKU_PTHREAD_MAX_THREADS 16

/* The per-thread handle. pthread_t is a pointer to it, which is what
 * pthread_self() returns and what join/detach take. `state` is the ownership
 * token for the cleanup, and every transition is a compare-and-swap, so
 * detach, join and the thread's own exit cannot both free the same stack.
 */
#define HAKU_PTHREAD_RUNNING  0
#define HAKU_PTHREAD_FINISHED 1
#define HAKU_PTHREAD_DETACHED 2
#define HAKU_PTHREAD_REAPED   3

typedef struct haku_pthread {
    volatile int state;
    /* Written by the kernel: clone(CLONE_CHILD_SETTID) publishes the task id
     * here before the child can run, and CLONE_CHILD_CLEARTID zeroes it (and
     * wakes a futex) during the exit syscall. Zero therefore means "the task
     * is gone and no longer touches its stack", which is what a joiner waits
     * for. */
    volatile int tid;
    void *retval;
    void *(*fn)(void *);
    void *arg;
    void *tcb;
    unsigned char *stack;
    size_t stack_size;
    int main_thread;
} haku_pthread;

typedef haku_pthread *pthread_t;

int pthread_create(pthread_t *thread, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t left, pthread_t right);
__declspec(noreturn) void pthread_exit(void *retval);

/* The robust futex list. The head is the exact layout the kernel reads:
 * `next` anchors the list, `futex_offset` is applied to every node, and
 * `pending` is the node of an operation in progress (unused). One node is
 * embedded in every mutex; `next` is what the kernel follows and `back` is
 * the library's back-link, so both linking and unlinking are constant time.
 *
 * The list is null-terminated rather than anchored at the head, which is
 * what lets the kernel's walk and the O(1) unlink share one layout: a null
 * `next` ends the walk, and no link write ever lands on the head's
 * futex_offset word. The kernel only ever reads the first word of a node, so
 * the node type is free to carry the back-link in its second word.
 */
typedef struct haku_robust_head {
    void *next;
    haku_pthread_word_t futex_offset;
    void *pending;
} haku_robust_head_t;

typedef struct haku_robust_node {
    void *next;
    /* The link that points at this node: &head->next for the first node,
     * &previous->next otherwise. Unlinking is *back = next. */
    void **back;
} haku_robust_node_t;

/* A robust mutex. The futex word holds the owner tid in its low 30 bits plus
 * the kernel's OWNER_DIED and WAITERS bits; `consistent` records whether the
 * current owner repaired the state a dead owner left behind, and `poisoned`
 * is set when it did not. The node sits at HAKU_PTHREAD_NODE_OFFSET. */
typedef struct haku_mutex {
    volatile int word;
    volatile int linked;
    volatile int consistent;
    volatile int poisoned;
    haku_robust_node_t node;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 1, 0, { 0, 0 } }

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
/* Declare that the state a dead owner left behind has been repaired, so the
 * mutex may be used and released again. Without this call an unlock of a
 * mutex taken over after EOWNERDEAD leaves it ENOTRECOVERABLE. */
int pthread_mutex_consistent(pthread_mutex_t *mutex);

/* A condition variable. A signal is an increment of `seq` followed by a
 * wake, and a wait parks on the value it read before releasing the mutex:
 * the futex compare makes a signal that arrives in that window fail the
 * wait with EAGAIN instead of getting lost. */
typedef struct haku_cond {
    volatile int seq;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER { 0 }

int pthread_cond_init(pthread_cond_t *cond, const void *attributes);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

#endif
