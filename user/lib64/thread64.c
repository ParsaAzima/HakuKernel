/* Haku x86-64 threads.
 *
 * clone(2) with CLONE_VM starts a second task in the caller's address space;
 * CLONE_SETTLS gives it an FS base of its own. Haku's compiler TLS uses the
 * Windows x64 convention (`mov rcx, gs:[0x58]` -> TlsSlots -> block), so a
 * thread also needs its own TEB and GS base. The kernel only sets FS from
 * the fifth clone argument — FS is the SysV thread pointer and plainly
 * kernel state — while GS is installed by haku64_thread_entry(), the first
 * user code the child runs, because which TEB a thread uses is a library
 * decision and not something the kernel can infer.
 *
 * Everything a thread needs lives at the top of its own stack mapping: the
 * joinable handle, then the start record, then the initial RSP. The parent
 * neither allocates nor frees anything the child may still be reading:
 * thread64.asm reloads the record from a callee-saved register that the
 * kernel copied into the child's frame. Nothing above the initial RSP is
 * touched by the child's stack, so the handle stays readable until
 * haku64_thread_join() releases the whole mapping.
 *
 * CLONE_CHILD_SETTID makes the kernel write the child's tid into the
 * handle before the child can run, and CLONE_CHILD_CLEARTID makes it zero
 * that word and wake one futex waiter when the child ends. A join is
 * therefore a futex wait on the handle, then waitpid() for the status, then
 * the stack and TLS block are released. The handle points into that stack,
 * so it is dangling once the join succeeds.
 */
#include <haku/abi64.h>

/* 64 KiB is the same order as a default pthread stack and leaves plenty of
 * room for the printf path and a few nested calls. It is a fixed size, so a
 * hostile program cannot turn this into unbounded allocation. */
#define HAKU64_THREAD_STACK_SIZE (64 * 1024)

/* Exactly what thread64.asm expects below the handle. Keep the field order
 * and the asm in sync; the struct is 24 bytes. */
typedef struct {
    int (*fn)(void *);
    void *arg;
    void *teb;
} haku64_thread_start_t;

/* thread64.asm: raw clone with CLONE_SETTLS and the two child tid flags.
 * `child_stack` is what the kernel loads into the child's RSP, `record`
 * stays in a register the child inherits, `tls` becomes the child's FS base
 * and `clear_tid` is the word the kernel publishes the tid in and zeroes at
 * exit. Returns the child tid to the parent and does not return at all in
 * the child, which enters haku64_thread_entry() on the new stack. */
long long haku64_clone_thread(void *child_stack, void *tls, void *record,
                              int *clear_tid);

/* The child's first C code. Only r12 (the record pointer) is guaranteed, so
 * everything else is read from the record. */
void haku64_thread_entry(haku64_thread_start_t *start) {
    if (arch_prctl(HAKU64_ARCH_SET_GS,
                   (unsigned long long)start->teb) != 0)
        _exit(70);                    /* EX_SOFTWARE: no TLS for this thread */
    /* _exit ends only this thread; exit() would end the whole group. */
    _exit(start->fn(start->arg));
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

    tid = haku64_clone_thread((unsigned char *)start - 32, teb, start,
                              &thread->done);
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

/* haku64_thread_create: run fn(arg) and return the tid, or -1 with errno.
 * The thread is a task like any other, so waitpid() reaps it, but there is
 * no handle: its stack and TLS block go away with the address space. */
int haku64_thread_create(int (*fn)(void *), void *arg) {
    haku64_thread_t *thread = haku64_thread_spawn(fn, arg);

    return thread ? thread->tid : -1;
}
