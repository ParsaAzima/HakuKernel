/* Haku x86-64 threads: the legacy haku64_thread_* API.
 *
 * clone(2) with CLONE_VM starts a second task in the caller's address space;
 * CLONE_SETTLS gives it an FS base of its own. Haku's compiler TLS uses the
 * Windows x64 convention (`mov rcx, gs:[0x58]` -> TlsSlots -> block), so a
 * thread also needs its own TEB and GS base. The kernel only sets FS from
 * the clone argument - FS is the SysV thread pointer and plainly kernel
 * state - while GS is installed by the child's first C code, because which
 * TEB a thread uses is a library decision and not something the kernel can
 * infer.
 *
 * Two APIs share the same machinery:
 *
 *   haku64_thread_spawn  a joinable thread whose handle and start record sit
 *                        in the top of its own stack mapping; the joiner
 *                        reaps and frees both through haku64_thread_join().
 *   haku64_thread_create a detached thread with no handle at all: its tid is
 *                        the only thing the caller gets, and the thread frees
 *                        its TLS block and stack itself as its last act.
 *
 * The pthread layer (pthread64.c) is the third caller: it keeps its handle in
 * the TEB page and passes haku64_pthread_entry as the entry point, which is
 * why haku64_clone_thread takes both a record and an entry function.
 */
#include <haku/abi64.h>

#include "tls64_internal.h"

/* 64 KiB is the same order as a default pthread stack and leaves plenty of
 * room for the printf path and a few nested calls. It is a fixed size, so a
 * hostile program cannot turn this into unbounded allocation. */
#define HAKU64_THREAD_STACK_SIZE (64 * 1024)

/* Exactly what haku64_thread_entry() expects. The record lives in the top of
 * the thread's stack mapping, so the parent never frees anything the child
 * may still be reading. */
typedef struct {
    int (*fn)(void *);
    void *arg;
    void *teb;
    unsigned char *stack;           /* base of this thread's stack mapping */
    unsigned long long stack_size;
    int detached;                   /* no joiner: the thread cleans up itself */
} haku64_thread_start_t;

/* The child's first C code. A joinable thread simply _exit()s and leaves its
 * stack and TLS block for haku64_thread_join(); a detached one releases both
 * before it goes, which is the whole difference between the two kinds. The
 * record arrives as an untyped pointer because the clone stub calls every
 * entry point through one signature. */
void haku64_thread_entry(void *record) {
    haku64_thread_start_t *start = (haku64_thread_start_t *)record;
    int status;

    /* Raw arch_prctl: errno is already per thread through GS, and this is the
     * call that installs GS, so a failure must not try to record itself. */
    if (haku64_syscall2(HAKU64_SYS_ARCH_PRCTL, HAKU64_ARCH_SET_GS,
                        (long long)start->teb) < 0)
        _exit(70);                    /* EX_SOFTWARE: no TLS for this thread */
    /* _exit ends only this thread; exit() would end the whole group. */
    status = start->fn(start->arg);
    if (!start->detached) _exit(status);
    haku64_tls_free(start->teb);
    haku64_thread_retire(start->stack, start->stack_size, status);
    for (;;) {
    }
}

/* Place `size` bytes so that the result ends at or below `top` and is
 * 16-byte aligned: the kernel wants an aligned child stack, and the record
 * and handle both hold pointers. */
static unsigned char *place_below(unsigned char *top, unsigned long long size) {
    return (unsigned char *)(((unsigned long long)(top - size)) & ~15ull);
}

/* Spawn: run fn(arg) on a new thread that shares the caller's address space,
 * descriptor table and working directory. Returns the handle, or NULL with
 * errno set. This is the only place a thread's stack is allocated. */
haku64_thread_t *haku64_thread_spawn(int (*fn)(void *), void *arg) {
    unsigned char *stack;
    haku64_thread_t *thread;
    haku64_thread_start_t *start;
    void *teb;
    long long tid;

    if (!fn) {
        errno = HAKU64_EINVAL;
        return NULL;
    }
    stack = (unsigned char *)mmap(NULL, HAKU64_THREAD_STACK_SIZE,
                                  HAKU64_PROT_READ | HAKU64_PROT_WRITE,
                                  HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS,
                                  -1, 0);
    if (stack == (void *)-1) return NULL;   /* mmap set errno */

    teb = haku64_tls_new();
    if (!teb) {
        munmap(stack, HAKU64_THREAD_STACK_SIZE);
        errno = HAKU64_ENOMEM;
        return NULL;
    }

    /* Layout, from the top of the mapping down: handle, start record, then
     * the initial RSP 32 bytes lower. Those 32 bytes are the Microsoft x64
     * ABI shadow space, so the child's first calls spill between the two and
     * never reach the record. */
    thread = (haku64_thread_t *)place_below(stack + HAKU64_THREAD_STACK_SIZE,
                                            sizeof(*thread));
    start = (haku64_thread_start_t *)place_below((unsigned char *)thread,
                                                 sizeof(*start));
    thread->tid = 0;
    thread->done = 0;               /* the kernel writes the tid here */
    thread->teb = teb;
    thread->stack = stack;
    thread->stack_size = HAKU64_THREAD_STACK_SIZE;
    start->fn = fn;
    start->arg = arg;
    start->teb = teb;
    start->stack = stack;
    start->stack_size = HAKU64_THREAD_STACK_SIZE;
    start->detached = 0;

    /* Joinable: the tid word is published before the child runs and zeroed at
     * exit, and the task stays waitable until haku64_thread_join() reaps it. */
    tid = haku64_clone_thread((unsigned char *)start - 32, teb, start,
                              &thread->done, 0, haku64_thread_entry);
    if (tid < 0) {
        /* No child exists, so the resources are still ours to release. */
        haku64_tls_free(teb);
        munmap(stack, HAKU64_THREAD_STACK_SIZE);
        errno = (int)-tid;
        return NULL;
    }
    thread->tid = (int)tid;
    return thread;
}

/* Join: sleep until the child has zeroed the clear-tid word, then reap it and
 * release the two mappings this library owns. A wakeup that finds the word
 * unchanged is spurious and simply retries, and -EAGAIN means the value
 * changed between the read and the park, which is the same retry. */
int haku64_thread_join(haku64_thread_t *thread) {
    int status = 0;

    if (!thread) {
        errno = HAKU64_EINVAL;
        return -1;
    }
    while (thread->done != 0) {
        long long result = haku64_syscall3(
            HAKU64_SYS_FUTEX, (long long)(void *)&thread->done,
            HAKU64_FUTEX_WAIT | HAKU64_FUTEX_PRIVATE_FLAG,
            (long long)thread->done);

        if (result < 0 && (int)-result != HAKU64_EAGAIN) {
            errno = (int)-result;
            return -1;
        }
    }
    if (waitpid(thread->tid, &status, 0) != thread->tid) return -1;

    haku64_tls_free(thread->teb);
    munmap(thread->stack, thread->stack_size);
    return (status >> 8) & 0xFF;
}

/* haku64_thread_create: run fn(arg) on a detached thread and return its tid,
 * or -1 with errno. There is no handle and nobody joins it: when the function
 * returns, the thread frees its TLS block, unmaps its own stack and exits,
 * and the kernel reclaims the task slot at the next switch. The tid is only
 * useful for kill() and for /proc-style inspection. */
int haku64_thread_create(int (*fn)(void *), void *arg) {
    unsigned char *stack;
    haku64_thread_start_t *start;
    void *teb;
    long long tid;

    if (!fn) {
        errno = HAKU64_EINVAL;
        return -1;
    }
    stack = (unsigned char *)mmap(NULL, HAKU64_THREAD_STACK_SIZE,
                                  HAKU64_PROT_READ | HAKU64_PROT_WRITE,
                                  HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS,
                                  -1, 0);
    if (stack == (void *)-1) return -1;     /* mmap set errno */

    teb = haku64_tls_new();
    if (!teb) {
        munmap(stack, HAKU64_THREAD_STACK_SIZE);
        errno = HAKU64_ENOMEM;
        return -1;
    }

    /* Only the start record lives in the mapping: the child frees the rest,
     * and there is no handle to keep it readable. */
    start = (haku64_thread_start_t *)place_below(stack + HAKU64_THREAD_STACK_SIZE,
                                                 sizeof(*start));
    start->fn = fn;
    start->arg = arg;
    start->teb = teb;
    start->stack = stack;
    start->stack_size = HAKU64_THREAD_STACK_SIZE;
    start->detached = 1;

    tid = haku64_clone_thread((unsigned char *)start - 32, teb, start,
                              NULL, 1, haku64_thread_entry);
    if (tid < 0) {
        haku64_tls_free(teb);
        munmap(stack, HAKU64_THREAD_STACK_SIZE);
        errno = (int)-tid;
        return -1;
    }
    return (int)tid;
}
