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
 * The start record sits at the top of the thread's own stack, above the
 * initial RSP, so the parent neither allocates nor frees anything the child
 * may still be reading: thread64.asm reloads the record from a callee-saved
 * register that the kernel copied into the child's frame. The stack and TLS
 * block are intentionally not reclaimed. A thread can exit at any moment and
 * there is no join here, so the parent has no point at which it knows the
 * child stopped touching its stack; both mappings go away with the address
 * space at process exit.
 */
#include <haku/abi64.h>

/* 64 KiB is the same order as a default pthread stack and leaves plenty of
 * room for the printf path and a few nested calls. It is a fixed size, so a
 * hostile program cannot turn this into unbounded allocation. */
#define HAKU64_THREAD_STACK_SIZE (64 * 1024)

/* Exactly what thread64.asm expects at the top of the new stack. Keep the
 * field order and the asm in sync; the struct is 24 bytes, so the aligned
 * base below still leaves the record inside the mapping. */
typedef struct {
    int (*fn)(void *);
    void *arg;
    void *teb;
} haku64_thread_start_t;

/* thread64.asm: raw clone with CLONE_SETTLS. `child_stack` is what the
 * kernel loads into the child's RSP, `record` stays in a register the child
 * inherits, and `tls` becomes the child's FS base. Returns the child tid to
 * the parent and does not return at all in the child, which enters
 * haku64_thread_entry() on the new stack. */
long long haku64_clone_thread(void *child_stack, void *tls, void *record);

/* The child's first C code. Only r12 (the record pointer) is guaranteed, so
 * everything else is read from the record. */
void haku64_thread_entry(haku64_thread_start_t *start) {
    if (arch_prctl(HAKU64_ARCH_SET_GS,
                   (unsigned long long)start->teb) != 0)
        _exit(70);                    /* EX_SOFTWARE: no TLS for this thread */
    /* _exit ends only this thread; exit() would end the whole group. */
    _exit(start->fn(start->arg));
}

/* haku64_thread_create: run fn(arg) on a new thread that shares the caller's
 * address space, descriptor table and working directory. Returns the child's
 * tid, or -1 with errno set. There is no handle and no join: the child is a
 * task like any other, so waitpid() reaps it if the caller needs the status.
 */
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
    if (stack == (void *)-1) return -1;   /* mmap set errno */

    teb = haku64_tls_new();
    if (!teb) {
        munmap(stack, HAKU64_THREAD_STACK_SIZE);
        errno = HAKU64_ENOMEM;
        return -1;
    }

    /* The kernel wants a 16-byte-aligned child stack. The record sits at
     * the top of the mapping and the initial RSP is 32 bytes below it: the
     * Microsoft x64 ABI's shadow space starts at the initial RSP and runs
     * for 32 bytes, so the callee's spills land between the two and never
     * overwrite the record. */
    start = (haku64_thread_start_t *)(
        ((unsigned long long)(stack + HAKU64_THREAD_STACK_SIZE -
                              sizeof(*start))) & ~15ull);
    start->fn = fn;
    start->arg = arg;
    start->teb = teb;

    tid = haku64_clone_thread((unsigned char *)start - 32, teb, start);
    if (tid < 0) {
        /* No child exists, so the resources are still ours to release. */
        haku64_tls_free(teb);
        munmap(stack, HAKU64_THREAD_STACK_SIZE);
        errno = (int)-tid;
        return -1;
    }
    return (int)tid;
}
