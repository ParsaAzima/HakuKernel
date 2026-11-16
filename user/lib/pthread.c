/* Haku 32-bit pthread subset: threads, robust mutexes, condition variables.
 *
 * A thread is a kernel task: clone(CLONE_VM|CLONE_THREAD) shares the address
 * space and the descriptor row, gives the child its own task id and its own
 * FS-relative thread block, and releases the task slot itself at exit
 * (HAKU_CLONE_DETACH). Join and detach are therefore library bookkeeping,
 * never wait4: a finished thread has no waitable zombie behind it.
 *
 * A thread owns exactly one page (its control block and its handle) and one
 * stack mapping. Who frees them depends on who claimed the thread:
 *
 *   joinable  the thread exits and a joiner frees both mappings
 *   detached  the thread unmaps both itself, as its last act
 *
 * The ownership token is `state`, and every transition is a compare-and-swap,
 * so the thread's own exit, pthread_join and pthread_detach can never both
 * free the same pages. A joiner cannot free a stack the exiting thread is
 * still standing on either: it waits for `tid`, the word the kernel zeroes
 * inside the exit syscall (CLONE_CHILD_CLEARTID), which is only reached once
 * the thread is done with its stack.
 */
#include <haku/errno.h>
#include <haku/futex.h>
#include <haku/mman.h>
#include <haku/pthread.h>
#include <haku/syscall.h>
#include <haku/unistd.h>

#include "tls_internal.h"

#define HAKU_THREAD_PAGE 4096u
#define HAKU_THREAD_STACK (128u * 1024u)

/* One thread page. `thread` must stay first: pthread_t names the page, and
 * both the join path and the detached exit unmapping it are one munmap. */
typedef struct {
    haku_pthread thread;
    haku_tcb_t tcb;
} thread_page_t;

typedef char thread_page_check[
    (sizeof(thread_page_t) <= HAKU_THREAD_PAGE) ? 1 : -1];

/* The startup thread's handle. It is not joinable and not detachable, and it
 * owns no page, so its state is REAPED for good. */
static haku_pthread main_thread;

pthread_t pthread_self(void) {
    haku_tcb_t *tcb = (haku_tcb_t *)haku_tcb_address();
    return tcb ? (pthread_t)tcb->thread : 0;
}

int pthread_equal(pthread_t left, pthread_t right) {
    return left == right;
}

void haku_pthread_boot(haku_tcb_t *tcb) {
    main_thread.state = HAKU_PTHREAD_REAPED;
    main_thread.tid = gettid();
    main_thread.main_thread = 1;
    tcb->thread = &main_thread;
}

/* The task id a mutex word records. It has to be the kernel's id, because the
 * exit walk compares it against the dying task's pid. A fork refreshes the
 * startup handle's id, so the fast path is a load and only a thread without a
 * handle (impossible after crt0) pays for a syscall. */
static int current_tid(void) {
    pthread_t self = pthread_self();
    int tid = self ? self->tid : 0;
    return tid ? tid : gettid();
}

void haku_after_fork(void) {
    /* The child owns the address space but not the parent's task ids: every
     * inherited robust head slot is stale, and only the calling thread
     * survives. It becomes this process's main thread, so its handle is
     * neither joinable nor detachable any more. */
    pthread_t self = pthread_self();

    haku_robust_forget_all();
    if (!self) return;
    self->tid = gettid();
    self->state = HAKU_PTHREAD_REAPED;
    self->main_thread = 1;
}

/* ------------------------------------------------------------- threads */

static __declspec(noreturn) void thread_finish(pthread_t thread, void *retval) {
    unsigned char *stack = thread->stack;
    size_t stack_size = thread->stack_size;

    thread->retval = retval;
    /* Free this task id's robust head slot before the id can be handed out
     * again; the kernel's exit walk uses the registered address, not the
     * slot, so nothing here depends on the head. */
    haku_robust_forget();
    if (__sync_val_compare_and_swap(&thread->state, HAKU_PTHREAD_RUNNING,
                                    HAKU_PTHREAD_FINISHED) ==
        HAKU_PTHREAD_RUNNING) {
        /* A joiner owns the stack and this page now. It waits for `tid` to
         * reach zero, which happens in this syscall, so it never frees
         * anything this thread still touches. */
        (void)haku_syscall1(HAKU_SYS_EXIT, 0);
    } else if (__sync_val_compare_and_swap(&thread->state,
                                           HAKU_PTHREAD_DETACHED,
                                           HAKU_PTHREAD_REAPED) ==
               HAKU_PTHREAD_DETACHED) {
        /* Detached: nobody will claim these pages, so unmap them here. The
         * stack goes last, and nothing below touches it again. */
        haku_thread_retire(stack, stack_size, thread, 0);
    } else {
        /* A joiner already claimed and freed the pages; just leave. */
        (void)haku_syscall1(HAKU_SYS_EXIT, 0);
    }
    for (;;) {
    }
}

void haku_pthread_begin(pthread_t thread) {
    thread_finish(thread, thread->fn(thread->arg));
}

static void thread_pages_free(pthread_t thread) {
    munmap(thread->stack, thread->stack_size);
    /* The control block lives in the same page as the handle, so this frees
     * both. errno itself goes with it, which is why the caller must not use
     * errno after this point. */
    munmap(thread, HAKU_THREAD_PAGE);
}

int pthread_create(pthread_t *handle, void *(*fn)(void *), void *arg) {
    const int flags = HAKU_CLONE_VM | HAKU_CLONE_FS | HAKU_CLONE_FILES |
                      HAKU_CLONE_SIGHAND | HAKU_CLONE_SYSVSEM |
                      HAKU_CLONE_THREAD | HAKU_CLONE_SETTLS |
                      HAKU_CLONE_CHILD_SETTID | HAKU_CLONE_CHILD_CLEARTID |
                      HAKU_CLONE_DETACH;
    thread_page_t *page;
    unsigned char *stack;
    unsigned int stack_top;
    int tid, result;

    if (!handle || !fn) return EINVAL;
    page = (thread_page_t *)mmap(0, HAKU_THREAD_PAGE,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return errno ? errno : ENOMEM;
    stack = (unsigned char *)mmap(0, HAKU_THREAD_STACK, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) {
        result = errno ? errno : ENOMEM;
        munmap(page, HAKU_THREAD_PAGE);
        return result;
    }

    page->thread.state = HAKU_PTHREAD_RUNNING;
    page->thread.tid = 0;           /* the kernel publishes the real id */
    page->thread.retval = 0;
    page->thread.fn = fn;
    page->thread.arg = arg;
    page->thread.tcb = &page->tcb;
    page->thread.stack = stack;
    page->thread.stack_size = HAKU_THREAD_STACK;
    page->thread.main_thread = 0;
    page->tcb.self = (unsigned int)&page->tcb;
    page->tcb.error = 0;
    page->tcb.thread = &page->thread;

    /* The child starts on this stack, 16-byte aligned because the ABI wants
     * a call frame there. */
    stack_top = ((unsigned int)(stack + HAKU_THREAD_STACK) - 16u) & ~15u;
    tid = haku_thread_clone((void *)stack_top, &page->tcb, &page->thread,
                            flags, &page->thread.tid);
    if (tid < 0) {
        munmap(stack, HAKU_THREAD_STACK);
        munmap(page, HAKU_THREAD_PAGE);
        return -tid;
    }
    *handle = &page->thread;
    return 0;
}

/* FINISHED only says the thread took the joiner's path, not that it is off
 * its stack: it still has to issue the exit syscall from there.
 * CLONE_CHILD_CLEARTID zeroes `tid` inside that syscall, so a zero word is
 * the point after which the page and the stack are dead. A spurious wake or
 * a futex EAGAIN just re-reads the word. */
static void thread_wait_done(pthread_t thread) {
    for (;;) {
        int tid = thread->tid;

        if (!tid) return;
        (void)haku_futex(&thread->tid, FUTEX_WAIT, tid, 0, 0, 0);
    }
}

int pthread_join(pthread_t thread, void **retval) {
    int state;

    if (!thread) return EINVAL;
    if (thread->main_thread) return EINVAL;
    if (thread == pthread_self()) return EDEADLK;
    if (__sync_val_compare_and_swap(&thread->state, HAKU_PTHREAD_FINISHED,
                                    HAKU_PTHREAD_REAPED) !=
        HAKU_PTHREAD_FINISHED) {
        state = thread->state;
        if (state != HAKU_PTHREAD_RUNNING) return EINVAL;
        /* Sleep until the thread finishes, then claim it. The claim itself
         * can lose to nobody else: detach and exit need RUNNING or FINISHED
         * as their source state. */
        for (;;) {
            int tid = thread->tid;

            if (!tid) break;
            (void)haku_futex(&thread->tid, FUTEX_WAIT, tid, 0, 0, 0);
        }
        if (__sync_val_compare_and_swap(&thread->state, HAKU_PTHREAD_FINISHED,
                                        HAKU_PTHREAD_REAPED) !=
            HAKU_PTHREAD_FINISHED)
            return EINVAL;
    }
    /* Claimed, but the thread may still be inside the exit syscall. */
    thread_wait_done(thread);
    if (retval) *retval = thread->retval;
    thread_pages_free(thread);
    return 0;
}

int pthread_detach(pthread_t thread) {
    if (!thread) return EINVAL;
    if (thread->main_thread) return EINVAL;
    for (;;) {
        int state = thread->state;
        if (state == HAKU_PTHREAD_RUNNING) {
            if (__sync_val_compare_and_swap(&thread->state,
                                            HAKU_PTHREAD_RUNNING,
                                            HAKU_PTHREAD_DETACHED) ==
                HAKU_PTHREAD_RUNNING)
                return 0;
            continue;
        }
        /* Already finished: the detacher is the only claimant left, so it
         * takes the cleanup the joiner would have done. */
        if (state == HAKU_PTHREAD_FINISHED) {
            if (__sync_val_compare_and_swap(&thread->state,
                                            HAKU_PTHREAD_FINISHED,
                                            HAKU_PTHREAD_REAPED) ==
                HAKU_PTHREAD_FINISHED) {
                /* The thread may still be inside its exit syscall. */
                thread_wait_done(thread);
                thread_pages_free(thread);
                return 0;
            }
            continue;
        }
        return EINVAL;              /* detached already, or reaped */
    }
}

void pthread_exit(void *retval) {
    pthread_t self = pthread_self();
    if (!self) {
        /* No handle installed: end this task the only way left. */
        (void)haku_syscall1(HAKU_SYS_EXIT, 0);
        for (;;) {
        }
    }
    thread_finish(self, retval);
}

/* ------------------------------------------------------- robust mutexes */

/* Link the mutex's node into this thread's robust list if it is not there
 * already. `linked` records which task did it, so a mutex taken over from a
 * dead owner is relinked into the new owner's list.
 *
 * The node is linked *after* the word is taken, never before. A waiter that
 * linked first would move the node out of the holder's list, and if the
 * holder then died the exit walk would not find the mutex and every later
 * locker would sleep forever. The cost is a handful of instructions between
 * taking the word and linking the node, during which an asynchronously
 * delivered fatal signal would still leave the mutex held; closing that would
 * need the kernel to register the mutex on every acquisition. */
static void mutex_link(pthread_mutex_t *mutex, int me) {
    haku_robust_head_t *head;
    haku_robust_node_t *node = &mutex->node;

    if (mutex->linked == me) return;
    head = haku_robust_ensure();
    if (!head) return;              /* no head for this id: no robust exit */
    node->back = &head->next;
    node->next = head->next;
    if (node->next) ((haku_robust_node_t *)node->next)->back = &node->next;
    head->next = node;              /* publish last */
    mutex->linked = me;
}

static void mutex_unlink(pthread_mutex_t *mutex, int me) {
    haku_robust_node_t *node = &mutex->node;

    if (mutex->linked != me) return;
    *node->back = node->next;
    if (node->next) ((haku_robust_node_t *)node->next)->back = node->back;
    node->next = 0;
    mutex->linked = 0;
}

/* Give the word back with the owner cleared. The WAITERS hint is kept, so a
 * waiter that announced itself before this unlock still gets its wake-up.
 * `me` is the caller's task id: the word may still name it, and clearing
 * that is exactly the point. A word already owned by somebody else means a
 * new owner took over between the caller's check and here, and this release
 * must not touch it. */
static void mutex_release(pthread_mutex_t *mutex, int me) {
    int want;

    for (;;) {
        int old = mutex->word;
        int owner = old & HAKU_FUTEX_TID_MASK;

        if (owner != 0 && owner != me) return;
        want = old & HAKU_FUTEX_WAITERS;
        if (__sync_val_compare_and_swap(&mutex->word, old, want) == old) break;
    }
    if (want) (void)haku_futex(&mutex->word, FUTEX_WAKE, 1, 0, 0, 0);
}

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes) {
    (void)attributes;               /* attributes are deliberately absent */
    if (!mutex) return EINVAL;
    mutex->word = 0;
    mutex->linked = 0;
    mutex->consistent = 1;
    mutex->poisoned = 0;
    mutex->node.next = 0;
    mutex->node.back = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    if ((mutex->word & HAKU_FUTEX_TID_MASK) != 0) return EBUSY;
    mutex_unlink(mutex, current_tid());
    mutex->poisoned = 0;
    return 0;
}

/* The one place a mutex word becomes owned, so the owner-died and poisoned
 * checks cannot disagree with the acquisition. Returns 0, EOWNERDEAD or
 * ENOTRECOVERABLE. */
static int mutex_acquired(pthread_mutex_t *mutex, int me, int old) {
    if (mutex->poisoned) {
        /* Nobody repaired the state the dead owner left, so this mutex is
         * dead for good. Undo the acquisition before reporting it. */
        mutex_release(mutex, me);
        return ENOTRECOVERABLE;
    }
    mutex_link(mutex, me);
    if (old & HAKU_FUTEX_OWNER_DIED) {
        /* We hold it, but the state it protects may be broken; the caller
         * decides with pthread_mutex_consistent(). */
        mutex->consistent = 0;
        return EOWNERDEAD;
    }
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    int me;

    if (!mutex) return EINVAL;
    me = current_tid();
    for (;;) {
        int old = mutex->word;
        if ((old & HAKU_FUTEX_TID_MASK) == 0) {
            int want = me | (old & HAKU_FUTEX_WAITERS);
            if (__sync_val_compare_and_swap(&mutex->word, old, want) == old)
                return mutex_acquired(mutex, me, old);
            continue;
        }
        if ((old & HAKU_FUTEX_TID_MASK) == me) return EDEADLK;
        /* Announce a waiter before sleeping, so an unlock that reads the word
         * next sees it and wakes us instead of dropping the wake-up. */
        if (!(old & HAKU_FUTEX_WAITERS)) {
            if (__sync_val_compare_and_swap(&mutex->word, old,
                                            old | HAKU_FUTEX_WAITERS) != old)
                continue;
            old |= HAKU_FUTEX_WAITERS;
        }
        (void)haku_futex(&mutex->word, FUTEX_WAIT, old, 0, 0, 0);
    }
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    int me, old;

    if (!mutex) return EINVAL;
    me = current_tid();
    for (;;) {
        old = mutex->word;
        if ((old & HAKU_FUTEX_TID_MASK) != 0) return EBUSY;
        if (__sync_val_compare_and_swap(&mutex->word, old,
                                        me | (old & HAKU_FUTEX_WAITERS)) ==
            old)
            break;
    }
    return mutex_acquired(mutex, me, old);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    int me;

    if (!mutex) return EINVAL;
    me = current_tid();
    if ((mutex->word & HAKU_FUTEX_TID_MASK) != me) return EPERM;
    /* An unlock after a takeover without pthread_mutex_consistent() makes the
     * mutex permanently unusable, which is what POSIX asks for. */
    if (!mutex->consistent) mutex->poisoned = 1;
    mutex_unlink(mutex, me);
    mutex_release(mutex, me);
    return 0;
}

int pthread_mutex_consistent(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    if ((mutex->word & HAKU_FUTEX_TID_MASK) != current_tid()) return EPERM;
    mutex->consistent = 1;
    mutex->poisoned = 0;
    return 0;
}

/* ---------------------------------------------------- condition variables */

/* A signal is an increment of `seq` followed by a wake. A waiter parks on the
 * value it read before releasing the mutex, so a signal that lands in the
 * window between the two makes FUTEX_WAIT fail with EAGAIN instead of
 * getting lost. */
int pthread_cond_init(pthread_cond_t *cond, const void *attributes) {
    (void)attributes;
    if (!cond) return EINVAL;
    cond->seq = 0;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    return cond ? 0 : EINVAL;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int seq, result, error;

    if (!cond || !mutex) return EINVAL;
    seq = cond->seq;
    result = pthread_mutex_unlock(mutex);
    if (result) return result;
    result = haku_futex(&cond->seq, FUTEX_WAIT, seq, 0, 0, 0);
    error = (result < 0) ? errno : 0;
    result = pthread_mutex_lock(mutex);
    if (result) return result;
    /* EAGAIN means the signal won the race and consumed the wait; any other
     * failure (EINTR) is reported after the mutex is held again. */
    if (error && error != EAGAIN) return error;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    __sync_fetch_and_add(&cond->seq, 1);
    (void)haku_futex(&cond->seq, FUTEX_WAKE, 1, 0, 0, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    __sync_fetch_and_add(&cond->seq, 1);
    (void)haku_futex(&cond->seq, FUTEX_WAKE, 0x7FFFFFFF, 0, 0, 0);
    return 0;
}
