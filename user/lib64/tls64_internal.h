#ifndef HAKU64_TLS_INTERNAL_H
#define HAKU64_TLS_INTERNAL_H

/* The x86-64 thread state, shared by tls64.c, robust64.c, pthread64.c,
 * thread64.c and the clone stub.
 *
 * A thread has two 4 KiB pages of its own: the TEB page and, in it, the
 * pthread handle. The TLS block is a third mapping whose size comes from the
 * image's PT_TLS. The handle lives at HAKU64_TEB_HANDLE_OFFSET so that the
 * TEB page can hold everything a thread needs and haku64_tls_free() releases
 * the TEB, the handle and the block with one call.
 *
 * The robust list head is deliberately NOT in the TEB. A detached thread
 * frees its TEB and stack before it exits, and the kernel walks the robust
 * list during the exit syscall, after those unmaps. The heads live in
 * robust64.c's .bss instead, exactly as on the 32-bit port.
 */
#include <haku/abi64.h>
#include <haku/pthread.h>

#define HAKU64_TEB_HANDLE_OFFSET 0x200

/* One thread handle lives at the top of its TEB page; a stack is a separate
 * mapping so a detached thread can free the TEB before it unmaps the stack it
 * is standing on. */
#define HAKU64_THREAD_PAGE 4096u
#define HAKU64_THREAD_STACK (128u * 1024u)

/* syscall.asm: this thread's TEB address, read through GS:0x30. Returns 0
 * before haku64_tls_init() installed a base, which no supported entry path
 * leaves behind. */
void *haku64_tls_self(void);

/* robust64.c: register this thread's robust list head if that has not
 * happened yet, and return it. A null result means the task id is outside
 * the range the library keeps heads for, and the thread then simply has no
 * robust mutexes. */
haku_robust_head_t *haku64_robust_ensure(void);

/* robust64.c: release this thread's head slot, so the next task that is given
 * the same id registers its own list instead of inheriting a stale one. */
void haku64_robust_forget(void);

/* robust64.c: clear every slot. A forked child inherits the parent's table
 * but none of its kernel registrations, so ids the parent used must not look
 * "already registered" when the child's tasks are given them again. */
void haku64_robust_forget_all(void);

/* pthread64.c: fill in the startup thread's handle inside `teb`. Called by
 * haku64_tls_init() once the TEB is installed. */
void haku64_pthread_boot(void *teb);

/* pthread64.c: run the handle's function and finish the thread. Never
 * returns. */
__declspec(noreturn) void haku64_pthread_begin(pthread_t thread);

/* thread64.asm: unmap `stack` and exit with `status` as this thread's last
 * act. Never returns and never touches the stack after the unmap. */
__declspec(noreturn)
void haku64_thread_retire(void *stack, unsigned long long size, int status);

/* pthread64.c: a forked child keeps the parent's TEB and handle but has a
 * task id of its own, so its robust head and handle are refreshed. */
void haku64_after_fork(void);

/* thread64.asm: clone(2) for the thread layer. The child shares the address
 * space, starts on `child_stack` with `tls` as its FS base, and runs
 * `entry(record)`; the kernel publishes its id in *clear_tid and zeroes it at
 * exit when that word is not null, and `detached` adds HAKU64_CLONE_DETACH so
 * no wait4 is needed to release the task slot. Returns the tid, or -errno. */
long long haku64_clone_thread(void *child_stack, void *tls, void *record,
                              volatile int *clear_tid, int detached,
                              void (*entry)(void *));

/* pthread64.c: the first code a cloned pthread runs. Installs the thread's
 * GS base (FS was done by CLONE_SETTLS) and hands over to
 * haku64_pthread_begin(). `record` is the thread's handle. */
__declspec(noreturn) void haku64_pthread_entry(void *record);

#endif
