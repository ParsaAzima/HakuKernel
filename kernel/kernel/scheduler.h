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
/* Per-CPU scheduling. Every processor runs its own current task, its own
 * idle task and its own timer; a task is pinned to the CPU that created it
 * (user processes to the bootstrap processor), so no task ever migrates and
 * the task table needs one lock only for slot allocation and reaping. */
int scheduler_prepare_ap(uint32_t cpu);
/* Enter this CPU's idle task. Runs on the application processor itself and
 * never returns. */
void scheduler_start_ap(uint32_t cpu);
/* A kernel-mode task pinned to `cpu` (1..SMP_MAX_CPUS-1): the SMP boot test
 * and, later, per-CPU kernel workers. Returns the task slot or -errno. */
int32_t scheduler_create_kernel_on(uint32_t cpu, void (*entry)(void));
/* End a kernel-mode task from its own entry point. The CPU parks until its
 * next interrupt switches it to the idle task, and the bootstrap processor
 * frees the slot: a task cannot release the stack it is standing on. */
void scheduler_kernel_exit(void);
/* Boot-time handshake for the task above: wait until the slot is free again
 * and free it. Returns 1 when the slot is gone. */
int scheduler_join_kernel(int32_t slot);
int scheduler_user_tasks_done(void);
/* 1 while the scheduler is running and at least one user task can still
 * make progress. The timer heartbeat stays off the console while it is 1. */
int scheduler_user_tasks_live(void);
void scheduler_start(void);
regs_t *scheduler_preempt(regs_t *frame);
regs_t *scheduler_exit_current(regs_t *frame);
int scheduler_exec_current(loaded_elf_t *replacement, regs_t *frame);
int32_t scheduler_spawn(loaded_elf_t *program);
/* fork(2)/clone(2): the child gets its own address space that shares the
 * parent's pages copy-on-write, and inherits its descriptors; the child
 * returns 0 from the call and the parent gets the new pid. */
int32_t scheduler_fork(regs_t *frame);
/* clone(2): CLONE_VM shares the caller's address space (a thread),
 * CLONE_FILES shares the descriptor row and CLONE_THREAD joins the caller's
 * thread group. Without CLONE_VM it behaves like fork. Returns the new tid
 * or a negative -errno. */
int32_t scheduler_clone(regs_t *frame, uint32_t flags, uint32_t stack,
                        uint32_t parent_tid, uint32_t child_tid,
                        uint32_t tls);
/* exit_group(2): end every thread of the caller's group, then the caller.
 * Returns the frame the scheduler resumes next. */
regs_t *scheduler_exit_group(regs_t *frame);
/* execve(2) from a thread ends the other threads first (Linux does the
 * same); the caller keeps the address space so the image swap can free it. */
void scheduler_exec_detach_threads(void);
/* getpid(2) for a thread reports its group id. */
uint32_t scheduler_current_tgid(void);
/* set_tid_address(2): the word the kernel clears when this thread exits. */
uint32_t scheduler_set_tid_address(uint32_t user_tidptr);
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
/* Process groups and sessions. Every task records the group and session it
 * belongs to; a fresh task is its own leader. A shell that turns on job
 * control puts each pipeline in a new group and makes it the terminal's
 * foreground group, so these calls have to be real: reporting the caller's
 * pid for every query made the second member of a pipeline fail with EPERM
 * ("can't set tty process group") and the whole pipeline die. */
uint32_t scheduler_current_pgrp(void);
int32_t scheduler_get_pgrp(uint32_t pid, uint32_t *pgrp);
/* POSIX setpgid: pid 0 is the caller, pgid 0 means "the target's pid". A
 * task may move itself or one of its children; the group leader must exist
 * (a task with that pid) or the group must already have members. */
int32_t scheduler_set_pgrp(uint32_t pid, uint32_t pgid);
/* 1 when pgid names a live group: a task with that pid or a member. */
int scheduler_pgrp_valid(uint32_t pgid);
/* setsid(2): the caller becomes a session and group leader. */
uint32_t scheduler_setsid(void);
int32_t scheduler_get_sid(uint32_t pid, uint32_t *sid);
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
/* Signals. kill(2) takes a pid, 0 for the caller, -1 for every other
 * process, or a negative process-group id. tkill/tgkill take thread ids;
 * tgkill also checks that the thread belongs to the named group. */
int32_t scheduler_kill_pid(uint32_t pid, uint32_t sig);
int32_t scheduler_kill_all(uint32_t sig);
/* Send a signal to every member of a process group (kill(-pgid, sig)).
 * Signal 0 only checks that the group exists. */
int32_t scheduler_kill_pgrp(uint32_t pgid, uint32_t sig);
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
