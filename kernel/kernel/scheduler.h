#ifndef HAKU_SCHEDULER_H
#define HAKU_SCHEDULER_H

#include "elf.h"
#include "haku/abi.h"
#include "idt.h"

#define SCHEDULER_MAX_PROCESSES 4u
/* Signals: Haku implements the standard range 1..31, so a mask fits in a
 * single word with bit (sig - 1) set. */
#define SCHEDULER_NSIG 31u
/* Longest canonical (root-relative) working directory a task keeps. Must
 * match VFS_PATH_MAX; scheduler.c asserts the two agree. */
#define SCHEDULER_PATH_MAX 64u
/* process name reported by PR_GET_NAME, NUL-terminated like Linux. */
#define SCHEDULER_COMM_MAX 16u

/* Event wait queue. A task that cannot make progress parks itself on a
 * queue and is woken by whoever satisfies the event (an IRQ handler or
 * another task); a parked task is off the run queue and does not spin.
 * The queue is a bitmap over the scheduler's task slots, so it needs no
 * memory and cannot fail. sleep/cancel must be called with interrupts
 * disabled (the scheduler's task-state rule); wake is IRQ-safe. */
typedef struct {
    volatile uint32_t waiters;
} wait_queue_t;

#define WAIT_QUEUE_INIT { 0u }

/* Woken once per timer tick so a task parked in poll(2)/select(2)
 * re-checks descriptor readiness even when nothing else signals it. The
 * poll loop treats that wake as the clock: a tick costs one re-scan. */
extern wait_queue_t scheduler_activity;

/* Mark the current task blocked on the queue. The caller must immediately
 * execute sti;hlt so either the waking IRQ returns straight back or the
 * timer IRQ switches away. Returns 1 when a task was parked. */
int wait_queue_sleep(wait_queue_t *queue);
/* Leave the queue and make the current task runnable again. */
void wait_queue_cancel(wait_queue_t *queue);
/* Wake every parked task; each re-checks its condition after resuming.
 * The bitmap entry is dropped even for tasks that are no longer blocked. */
int wait_queue_wake_all(wait_queue_t *queue);

int scheduler_init(loaded_elf_t *initial_program);
int scheduler_user_tasks_done(void);
void scheduler_start(void);
regs_t *scheduler_preempt(regs_t *frame);
regs_t *scheduler_exit_current(regs_t *frame);
int scheduler_exec_current(loaded_elf_t *replacement, regs_t *frame);
int32_t scheduler_spawn(loaded_elf_t *program);
/* fork(2)/clone(2): the child gets its own address space that shares the
 * parent's pages copy-on-write, and inherits its descriptors; the child
 * returns 0 from the call and the parent gets the new pid. */
int32_t scheduler_fork(regs_t *frame);
/* vfork(2): the child borrows the parent's address space and the parent
 * is parked until the child execs or exits. Returns the frame to resume,
 * which is the child's when the call switched tasks. */
regs_t *scheduler_vfork(regs_t *frame);
regs_t *scheduler_waitpid(uint32_t pid, uint32_t user_status,
                          uint32_t options, regs_t *frame);
/* wait4(2): like waitpid, but WNOHANG is accepted and the rusage block
 * is written when the caller provides one. */
regs_t *scheduler_wait4(uint32_t pid, uint32_t user_status, uint32_t options,
                        uint32_t user_rusage, regs_t *frame);
regs_t *scheduler_sleep_ticks(uint32_t ticks, regs_t *frame);
uint32_t scheduler_brk(uint32_t requested);
/* Grow the user stack down to cover fault_addr, if it lies within the
 * stack's growth window. Returns 1 when the page was mapped and the faulting
 * instruction can be retried, 0 when the fault is not a stack growth. */
int scheduler_grow_user_stack(uint32_t fault_addr);
uint32_t scheduler_mmap(uint32_t length, int writable);
/* The memory syscalls a libc needs: an anonymous mapping with the full
 * protection set and MAP_FIXED placement, protection changes, moving or
 * resizing a mapping, and the advice call. */
uint32_t scheduler_mmap2(uint32_t address, uint32_t length, uint32_t prot,
                         uint32_t flags);
uint32_t scheduler_mprotect(uint32_t address, uint32_t length, uint32_t prot);
uint32_t scheduler_mremap(uint32_t old_address, uint32_t old_length,
                          uint32_t new_length, uint32_t flags,
                          uint32_t new_address);
uint32_t scheduler_madvise(uint32_t address, uint32_t length, uint32_t advice);
/* The user FS base (the thread pointer) of the running task. */
uint32_t scheduler_set_thread_area(uint32_t base);
uint32_t scheduler_get_thread_area(void);
/* futex(2): park the caller on a user word until it is woken, and the
 * wake side. The wait returns the frame of the task that runs next, so
 * the caller only continues when the futex is woken (or the timeout
 * expires). */
regs_t *scheduler_futex_wait(uint32_t address, uint32_t pid,
                             uint32_t private_futex, uint32_t ticks,
                             regs_t *frame);
uint32_t scheduler_futex_wake(uint32_t address, uint32_t count,
                              uint32_t mask, uint32_t private_futex);
uint32_t scheduler_munmap(uint32_t address, uint32_t length);
uint32_t scheduler_current_pid(void);
uint32_t scheduler_parent_pid(void);
/* The per-task working directory is a canonical root-relative path (the
 * empty string is the volume root). get copies the NUL-terminated name and
 * returns its length, or -ERANGE when the buffer is too small. set takes an
 * already-validated path and copies it. */
int32_t scheduler_get_cwd(char *path, uint32_t size);
int32_t scheduler_set_cwd(const char *path);
/* The process name set by prctl(PR_SET_NAME). */
int32_t scheduler_get_comm(char *name, uint32_t size);
int32_t scheduler_set_comm(const char *name);
/* sched_yield(2): let the next runnable task run. Returns the frame that
 * resumes the caller (which may be this task when nobody else is ready). */
regs_t *scheduler_yield(regs_t *frame);
/* Signals. kill(2) takes a pid, 0 for the caller, or -1 for every other
 * process; Haku has no process groups. tkill/tgkill take thread ids, and
 * a thread id is a process id here. */
int32_t scheduler_kill_pid(uint32_t pid, uint32_t sig);
int32_t scheduler_kill_all(uint32_t sig);
int32_t scheduler_tkill(uint32_t tid, uint32_t sig);
int32_t scheduler_tgkill(uint32_t tgid, uint32_t tid, uint32_t sig);
int32_t scheduler_sigaction(uint32_t sig, const haku_sigaction_t *action,
                             haku_sigaction_t *previous);
uint32_t scheduler_sigprocmask(uint32_t how, uint32_t mask,
                               uint32_t *previous);
/* Deliver a pending signal before returning to user mode. */
regs_t *scheduler_signal_deliver(regs_t *frame);
/* rt_sigreturn(2): restore the context the signal interrupted. */
regs_t *scheduler_sigreturn(regs_t *frame);
/* Address space of the running task (0 outside a user process), for the
 * #PF handler and for copy_to_user. */
address_space_t *scheduler_current_space(void);
void scheduler_note_syscall(void);
int scheduler_get_usage(haku_usage_t *usage);

#endif
