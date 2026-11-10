#include "scheduler.h"
#include "frame.h"
#include "fs_test.h"
#include "gdt.h"
#include "klog.h"
#include "paging.h"
#include "pit.h"
#include "sync.h"
#include "usermode.h"
#include "vfs.h"

#define TASK_KERNEL_STACK_PAGES 2u
/* Anonymous mappings a process may hold at once. A libc allocates its heap
 * with mmap whenever the program break is not usable, one mapping per
 * expansion, so the limit is reached long before a program looks large:
 * eight regions capped a process at a few kilobytes of malloc and made
 * `ls` on a 250-entry directory and `sort` on a few thousand lines fail with
 * "out of memory". */
#define TASK_MAX_MMAP_REGIONS 64u

/* The task keeps its working directory inline; the two limits must agree so
 * a validated path always fits. */
typedef char scheduler_path_must_match_vfs[
    (SCHEDULER_PATH_MAX == VFS_PATH_MAX) ? 1 : -1];

typedef enum {
    TASK_UNUSED,
    TASK_STARTING,
    TASK_RUNNABLE,
    TASK_BLOCKED,
    TASK_WAITING,
    TASK_SLEEPING,
    TASK_ZOMBIE,
    TASK_REAP_PENDING
} task_state_t;

typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t pgrp;          /* process group (a session leader's pid) */
    uint32_t sid;           /* session id */
    task_state_t state;
    loaded_elf_t program;
    uint32_t kernel_stack;
    regs_t *frame;
    uint32_t exit_status;
    uint32_t wait_pid;
    uint32_t wait_status_user;
    uint32_t wait_rusage_user;
    uint32_t vfork_parent;  /* this task is a vfork child of that pid */
    uint32_t vfork_child;   /* this task is parked in vfork for that child */
    uint32_t wake_tick;
    uint32_t heap_brk;    /* current program break (0 = not yet used) */
    uint32_t heap_top;    /* highest heap page ever mapped (grow only) */
    uint32_t fs_base;     /* user FS base: the thread pointer */
    char cwd[SCHEDULER_PATH_MAX];   /* canonical root-relative path */
    char comm[SCHEDULER_COMM_MAX];  /* prctl(PR_SET_NAME) name */
    uint32_t futex_address;   /* the futex word the task waits on (0 = none) */
    uint32_t futex_private;   /* the futex is process-private */
    uint32_t mmap_region_count;
    uint32_t user_ticks;          /* timer ticks spent in ring 3 */
    uint32_t kernel_ticks;        /* timer ticks spent in ring 0 */
    uint32_t syscall_count;
    uint32_t voluntary_switches;  /* blocked, slept, or waited */
    uint32_t involuntary_switches;/* preempted while runnable */
    /* Signals: bit (sig - 1) marks a queued or blocked signal. */
    uint32_t signal_pending;
    uint32_t signal_blocked;
    struct {
        uint32_t handler;   /* HAKU_SIG_DFL, HAKU_SIG_IGN, or user code */
        uint32_t flags;
        uint32_t restorer;
        uint32_t mask;      /* signals blocked while the handler runs */
    } sigactions[SCHEDULER_NSIG];
    struct {
        uint32_t base;
        uint32_t size;
        uint32_t prot;   /* HAKU_PROT_* bits of the mapping */
        uint32_t flags;  /* HAKU_MAP_SHARED / HAKU_MAP_PRIVATE */
    } mmap_regions[TASK_MAX_MMAP_REGIONS];
} task_t;

#define SCHEDULER_IDLE_INDEX ((int)SCHEDULER_MAX_PROCESSES)
static task_t tasks[SCHEDULER_MAX_PROCESSES];
static task_t idle_task;
static int current_task = -1;
static int scheduler_started;
static int timer_marker_written;
static int waitq_marker_written;

wait_queue_t scheduler_activity = WAIT_QUEUE_INIT;

/* A fresh task starts at the volume root with the kernel's default name.
 * The caller sets pgrp/sid once the pid is known. */
static void init_task_identity(task_t *task) {
    memset(task->cwd, 0, sizeof(task->cwd));
    memcpy(task->comm, "haku", 5);
}

/* fork(2)/vfork(2): the child inherits the parent's directory and name. */
static void inherit_task_identity(task_t *child, const task_t *parent) {
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
    memcpy(child->comm, parent->comm, sizeof(child->comm));
    /* A fork stays in the parent's process group and session; ash moves the
     * pipeline into a fresh group right after the fork. */
    child->pgrp = parent->pgrp;
    child->sid = parent->sid;
    /* The thread pointer. A child shares the parent's address space
     * copy-on-write, so the same virtual base is valid there, and a libc
     * writes errno through it before it does anything else: a child that
     * started with %gs based at zero wrote to address 0 and faulted. */
    child->fs_base = parent->fs_base;
}

/* Ends the current task with an explicit wait status (the value a parent
 * sees in wait4). A normal exit passes the shifted exit code; a signal
 * passes the signal number the way Linux reports it. */
static regs_t *exit_current_with_status(regs_t *frame, uint32_t wait_status);

/* Drops a task from the futex it waits on (see the futex section). */
static void futex_remove_waiter(task_t *task);

int scheduler_user_tasks_done(void) {
    if (!scheduler_started) return 0;
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        if (tasks[i].state == TASK_STARTING ||
            tasks[i].state == TASK_RUNNABLE ||
            tasks[i].state == TASK_BLOCKED ||
            tasks[i].state == TASK_WAITING ||
            tasks[i].state == TASK_SLEEPING)
            return 0;
    }
    return 1;
}

/* 1 while a user task can still make progress. Before the scheduler starts
 * this is 0, so early boot still reports timer progress; it turns 1 for
 * good once userland is running and the console belongs to it. */
int scheduler_user_tasks_live(void) {
    return scheduler_started && !scheduler_user_tasks_done();
}

/* Wake every sleeper whose deadline has been reached. Called from the
 * timer IRQ so a sleeping task resumes once its wake tick passes. The
 * unsigned subtraction compares correctly across the 32-bit tick wrap.
 * A futex waiter that runs out of time leaves the queue and reports
 * ETIMEDOUT. */
static void wake_expired_sleepers(void) {
    uint32_t now = pit_ticks();
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *task = &tasks[i];
        if (task->state == TASK_SLEEPING &&
            (int32_t)(now - task->wake_tick) >= 0) {
            if (task->futex_address) {
                futex_remove_waiter(task);
                if (task->frame) task->frame->eax = 0u - HAKU_ETIMEDOUT;
            }
            task->state = TASK_RUNNABLE;
        }
    }
}

static void scheduler_idle_loop(void) {
    for (;;) {
        /* Runs only while no user task is runnable, so this is the
         * post-user hook: verify FS error reporting once, then idle.
         * The volume stays usable for the test because every user
         * file I/O check has already finished. */
        fs_error_self_test();
        __asm {
            sti
            hlt
        }
    }
}

static int initialize_stack(task_t *task, int user_mode) {
    uint32_t stack_bytes = TASK_KERNEL_STACK_PAGES * FRAME_SIZE;
    uint32_t stack_top;
    regs_t *frame;

    task->kernel_stack = (uint32_t)frame_alloc_pages(TASK_KERNEL_STACK_PAGES);
    if (!task->kernel_stack ||
        task->kernel_stack + stack_bytes > PAGING_PHYS_LIMIT) {
        if (task->kernel_stack)
            frame_free_pages((void *)task->kernel_stack,
                             TASK_KERNEL_STACK_PAGES);
        task->kernel_stack = 0;
        return 0;
    }

    /* The frame lives on the direct-mapped view; the physical address is
     * what release_task_resources() gives back to the allocator. */
    stack_top = (uint32_t)PHYS_TO_VIRT(task->kernel_stack) + stack_bytes;
    frame = (regs_t *)(stack_top - sizeof(regs_t));
    memset(frame, 0, sizeof(*frame));
    frame->gs = user_mode ? USER_DS : KERNEL_DS;
    frame->fs = user_mode ? USER_FS : KERNEL_DS;
    frame->es = user_mode ? USER_DS : KERNEL_DS;
    frame->ds = user_mode ? USER_DS : KERNEL_DS;
    frame->int_no = 0;
    frame->err_code = 0;
    frame->eip = user_mode ? task->program.entry :
                             (uint32_t)scheduler_idle_loop;
    frame->cs = user_mode ? USER_CS : KERNEL_CS;
    frame->eflags = 0x202u;
    if (user_mode) {
        frame->user_esp = task->program.user_stack;
        frame->user_ss = USER_DS;
    }
    task->frame = frame;
    return 1;
}

static int find_next_task(void) {
    for (uint32_t offset = 1; offset <= SCHEDULER_MAX_PROCESSES; offset++) {
        uint32_t selected_index;
        if (current_task == SCHEDULER_IDLE_INDEX)
            selected_index = offset - 1u;
        else
            selected_index = ((uint32_t)current_task + offset) %
                             SCHEDULER_MAX_PROCESSES;
        if (tasks[selected_index].state == TASK_RUNNABLE)
            return (int)selected_index;
    }
    if (current_task >= 0 && current_task < SCHEDULER_IDLE_INDEX &&
        tasks[current_task].state == TASK_RUNNABLE)
        return current_task;
    return SCHEDULER_IDLE_INDEX;
}

static void activate_task(int selected_index) {
    task_t *task;
    /* Count the switch out of the previous task here, where it actually
     * happens: a runnable task preempted by the timer is involuntary,
     * while blocking/sleeping/waiting tasks yielded voluntarily. Exiting
     * (zombie) tasks are not context switches. */
    if (current_task >= 0 && current_task < SCHEDULER_IDLE_INDEX) {
        task_t *previous = &tasks[current_task];
        if (previous->state == TASK_RUNNABLE)
            previous->involuntary_switches++;
        else if (previous->state == TASK_BLOCKED ||
                 previous->state == TASK_WAITING ||
                 previous->state == TASK_SLEEPING)
            previous->voluntary_switches++;
    }
    current_task = selected_index;
    if (selected_index == SCHEDULER_IDLE_INDEX) {
        task = &idle_task;
        paging_activate_kernel_space();
    } else {
        task = &tasks[selected_index];
        paging_activate_user_space(&task->program.address_space);
        /* The user FS segment carries the thread pointer: the GDT entry
         * must match the task that is about to run before its iret
         * reloads the segment. */
        gdt_set_user_fs_base(task->fs_base);
    }
    gdt_set_kernel_stack((uint32_t)PHYS_TO_VIRT(task->kernel_stack) +
                         TASK_KERNEL_STACK_PAGES * FRAME_SIZE);
}

static void release_task_resources(task_t *task) {
    if (task->program.address_space.directory_phys)
        paging_destroy_user_space(&task->program.address_space);
    if (task->kernel_stack) {
        frame_free_pages((void *)task->kernel_stack,
                         TASK_KERNEL_STACK_PAGES);
        task->kernel_stack = 0;
    }
    task->frame = 0;
}

/* Free the slots of children that their parent already reaped. The
 * timer-driven reap does this too, but a new task must not depend on a
 * timer tick landing in the right window: the process table would
 * otherwise run out of slots while a freed one is still pending. */
static void release_reaped_slots(void) {
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *task = &tasks[i];
        if (task->state != TASK_REAP_PENDING || (int)i == current_task)
            continue;
        release_task_resources(task);
        memset(task, 0, sizeof(*task));
    }
}

static void reap_completed_tasks(void) {
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *task = &tasks[i];
        if ((task->state != TASK_ZOMBIE &&
             task->state != TASK_REAP_PENDING) ||
            (int)i == current_task)
            continue;
        release_task_resources(task);
        if (task->state == TASK_REAP_PENDING) {
            klog_prefix();
            klog_write("SCHED-REAP-OK: reaped pid=");
            klog_dec(task->pid);
            klog_write("\n");
            memset(task, 0, sizeof(*task));
        }
    }
}

int scheduler_init(loaded_elf_t *initial_program) {
    if (!initial_program || !initial_program->address_space.directory_phys ||
        !initial_program->entry || !initial_program->user_stack)
        return 0;

    memset(tasks, 0, sizeof(tasks));
    memset(&idle_task, 0, sizeof(idle_task));
    current_task = -1;
    scheduler_started = 0;
    timer_marker_written = 0;
    waitq_marker_written = 0;

    tasks[0].pid = 1;
    tasks[0].parent_pid = 0;
    tasks[0].pgrp = 1;
    tasks[0].sid = 1;
    tasks[0].state = TASK_RUNNABLE;
    tasks[0].program = *initial_program;
    init_task_identity(&tasks[0]);
    if (!initialize_stack(&tasks[0], 1)) goto fail;
    /* The initial process owns the standard descriptors; every spawned
     * child inherits them from its parent. */
    if (!vfs_open_stdio(tasks[0].pid)) goto fail;
    idle_task.state = TASK_RUNNABLE;
    if (!initialize_stack(&idle_task, 0)) goto fail;
    initial_program->address_space.directory_phys = 0;

    klog_prefix();
    klog_write("sched: SCHED-READY tasks=1");
    klog_write(" kernel-stack=per-task\n");
    klog_prefix();
    klog_write("STDIO-OK: fds 0/1/2 on console\n");
    return 1;

fail:
    vfs_close_all(1);
    if (tasks[0].kernel_stack)
        frame_free_pages((void *)tasks[0].kernel_stack,
                         TASK_KERNEL_STACK_PAGES);
    memset(&tasks[0], 0, sizeof(tasks[0]));
    if (idle_task.kernel_stack)
        frame_free_pages((void *)idle_task.kernel_stack,
                         TASK_KERNEL_STACK_PAGES);
    memset(&idle_task, 0, sizeof(idle_task));
    return 0;
}

void scheduler_start(void) {
    int first = 0;
    (void)irq_lock();
    scheduler_started = 1;
    activate_task(first);
    klog_prefix();
    klog_write("sched: SCHED-START pid=");
    klog_dec(tasks[first].pid);
    klog_write(" (first task)\n");
    scheduler_enter_frame(tasks[first].frame);
    for (;;) __asm hlt
}

/* The scheduler's own state is protected by the IRQ-save rule below:
 * every task-array mutation runs with interrupts disabled (either in an IRQ
 * handler or under irq_lock()), so no spinlock is needed here. Never take a
 * task-context spinlock (VFS, ATA) with interrupts disabled: a preempted
 * holder would never run again to release it. */
regs_t *scheduler_preempt(regs_t *frame) {
    int next;

    if (!scheduler_started || !frame)
        return frame;
    /* Attribute this timer tick to the interrupted task. The frame's CS
     * tells us whether it was running in ring 3 or in a syscall/IRQ. */
    if (current_task >= 0 && current_task < SCHEDULER_IDLE_INDEX) {
        task_t *current = &tasks[current_task];
        if ((frame->cs & 3u) == 3u)
            current->user_ticks++;
        else
            current->kernel_ticks++;
    }
    if (!timer_marker_written) {
        klog_prefix();
        klog_write("sched: SCHED-TIMER-OK: timer interrupts reach scheduler\n");
        timer_marker_written = 1;
    }

    reap_completed_tasks();
    wake_expired_sleepers();
    /* The timer is the clock for a parked poll(2)/select(2): it re-scans
     * readiness and checks its deadline once per tick. */
    wait_queue_wake_all(&scheduler_activity);
    if (current_task == SCHEDULER_IDLE_INDEX) {
        idle_task.frame = frame;
    } else if (current_task >= 0 &&
               current_task < SCHEDULER_IDLE_INDEX) {
        tasks[current_task].frame = frame;
    }

    next = find_next_task();
    /* Observable proof that a blocked task is off the run queue: when a
     * task parks on a wait queue and nothing else is runnable, the idle
     * context is selected instead of scheduling that task again. */
    if (!waitq_marker_written && next == SCHEDULER_IDLE_INDEX) {
        for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
            if (tasks[i].state != TASK_BLOCKED) continue;
            klog_prefix();
            klog_write("sched: SCHED-WAITQ-OK: pid=");
            klog_dec(tasks[i].pid);
            klog_write(" parked on a wait queue, idle scheduled\n");
            waitq_marker_written = 1;
            break;
        }
    }
    if (next == current_task)
        return frame;

    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

/* Hand the wait result to the parent: the exit status and, when the
 * parent asked for one, a zeroed rusage block. The parent's return value
 * is the reaped child pid, or -EFAULT when a user pointer is invalid. */
static int deliver_wait_result(task_t *parent, task_t *child, uint32_t status) {
    haku_rusage_t usage;
    if (parent->wait_status_user &&
        !paging_copy_to_user_space(&parent->program.address_space,
                                   parent->wait_status_user, &status,
                                   sizeof(status)))
        return 0;
    if (parent->wait_rusage_user) {
        memset(&usage, 0, sizeof(usage));
        if (!paging_copy_to_user_space(&parent->program.address_space,
                                       parent->wait_rusage_user, &usage,
                                       sizeof(usage)))
            return 0;
    }
    parent->frame->eax = child->pid;
    return 1;
}

/* Release a parent that is parked inside vfork: the child either replaced
 * its image (exec) or exited, so the two processes no longer share the
 * address space and the parent resumes with the child pid. Called with
 * interrupts disabled. */
static void wake_vfork_parent(uint32_t child_pid) {
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *parent = &tasks[i];
        if (parent->vfork_child != child_pid) continue;
        parent->vfork_child = 0;
        /* vfork returns the child pid in the parent, as fork does. */
        if (parent->frame) parent->frame->eax = child_pid;
        if (parent->state == TASK_WAITING) parent->state = TASK_RUNNABLE;
        return;
    }
}

address_space_t *scheduler_current_space(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0;
    return &tasks[current_task].program.address_space;
}

regs_t *scheduler_exit_current(regs_t *frame) {
    return exit_current_with_status(frame, (frame->ebx & 0xFFu) << 8);
}

static regs_t *exit_current_with_status(regs_t *frame, uint32_t wait_status) {
    int next;
    task_t *exiting;
    uint32_t status;
    if (!scheduler_started || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;

    exiting = &tasks[current_task];
    /* A task that dies while waiting on a futex must not stay in the
     * queue: a later wake would find a stale waiter. */
    futex_remove_waiter(exiting);
    /* Closing descriptors takes the task-context VFS lock, so it must run
     * with interrupts enabled. The task-state transition below is then
     * committed under the scheduler's IRQ-save rule: interrupts stay off
     * through the context switch, and iret restores the next task's IF. */
    vfs_close_all(exiting->pid);
    (void)irq_lock();
    exiting->exit_status = wait_status;
    exiting->state = TASK_ZOMBIE;
    exiting->frame = frame;
    status = exiting->exit_status;

    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *child = &tasks[i];
        if (child->state == TASK_UNUSED ||
            child->parent_pid != exiting->pid)
            continue;
        child->parent_pid = exiting->pid == 1u ? 0u : 1u;
    }

    if (exiting->parent_pid == 0) {
        exiting->state = TASK_REAP_PENDING;
    } else {
        for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
            task_t *parent = &tasks[i];
            int pid_matches;
            if (parent->pid != exiting->parent_pid ||
                parent->state != TASK_WAITING)
                continue;
            pid_matches = parent->wait_pid == 0xFFFFFFFFu ||
                          parent->wait_pid == exiting->pid;
            if (!pid_matches) continue;
            if (!deliver_wait_result(parent, exiting, status))
                parent->frame->eax = 0u - 14u;
            parent->state = TASK_RUNNABLE;
            parent->wait_pid = 0;
            parent->wait_status_user = 0;
            parent->wait_rusage_user = 0;
            exiting->state = TASK_REAP_PENDING;
            break;
        }
    }
    /* A vfork child that exits without exec releases its parent; the child
     * stays a zombie so a later waitpid reports the status. */
    if (exiting->vfork_parent) {
        wake_vfork_parent(exiting->pid);
        exiting->vfork_parent = 0;
    }
    next = find_next_task();
    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

/* ------------------------------------------------------------------ */
/* Signals                                                            */
/* ------------------------------------------------------------------ */

/* The frame the kernel builds on the user stack when a handler runs:
 *
 *   +0x00  restorer    the handler's return address
 *   +0x04  argument 1  the signal number
 *   +0x08  argument 2  the siginfo pointer (SA_SIGINFO only)
 *   +0x0c  argument 3  the ucontext pointer (SA_SIGINFO only)
 *   +0x10  stub        "mov eax, 173; int 0x80", used when the program
 *                       did not ask for its own restorer
 *   +0x18  magic       identifies the frame for rt_sigreturn
 *   +0x1c  blocked     the blocked mask that was in effect
 *   +0x20  registers   the interrupted context
 *   +0x6c  siginfo     the signal information block
 *   +0xec  ucontext    the context a SA_SIGINFO handler may inspect
 *
 * The handler is entered with esp at the restorer, so the arguments sit
 * where the C calling convention expects them. The restorer runs with esp
 * just above the return address, which is where rt_sigreturn looks for
 * the frame. */
#define HAKU_SIGFRAME_MAGIC 0x48414B55u
#define HAKU_SIGINFO_SIZE 128u
/* Fixed offsets inside the frame (the layout is part of the ABI between
 * the kernel and the restorer that comes back with rt_sigreturn). */
#define HAKU_SIGFRAME_STUB 0x10u
#define HAKU_SIGFRAME_SIGINFO 0x6Cu
#define HAKU_SIGFRAME_UCONTEXT 0xECu

typedef struct {
    uint32_t flags;
    uint32_t link;
    uint32_t esp;
    uint32_t eip;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t cs;
    uint32_t eflags;
} haku_ucontext_t;

typedef struct {
    uint32_t restorer;
    uint32_t argument1;
    uint32_t argument2;
    uint32_t argument3;
    uint8_t stub[8];
    uint32_t magic;
    uint32_t blocked;
    regs_t registers;
    uint8_t siginfo[HAKU_SIGINFO_SIZE];
    haku_ucontext_t ucontext;
} haku_sigframe_t;

/* The offsets above must match the layout. */
typedef char haku_sigframe_layout_check[
    (sizeof(haku_sigframe_t) == 0x120u &&
     sizeof(haku_sigframe_t) == (HAKU_SIGFRAME_UCONTEXT +
                                 sizeof(haku_ucontext_t))) ? 1 : -1];

/* The default disposition of these signals is to ignore them. The stop
 * signals (SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU) have no job control in
 * Haku, so they end the process like any other fatal signal. */
static int signal_default_ignored(uint32_t sig) {
    return sig == HAKU_SIGCHLD || sig == HAKU_SIGURG ||
           sig == HAKU_SIGWINCH || sig == HAKU_SIGCONT;
}

/* End the current task because of a signal. The delivery hook runs with
 * interrupts off (it is called from the interrupt and syscall stubs), but
 * closing the descriptors takes the task-context VFS lock, whose holder
 * may be preempted: the exit path therefore runs with interrupts on. An
 * interrupt taken from kernel mode is ignored by the delivery hook, so
 * nothing can observe the switch halfway through. */
static regs_t *exit_current_for_signal(regs_t *frame, uint32_t sig) {
    regs_t *next;
    __asm {
        sti
    }
    next = exit_current_with_status(frame, sig & 0x7Fu);
    __asm {
        cli
    }
    return next;
}

static task_t *signal_find_task(uint32_t pid) {
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        if (tasks[i].state == TASK_UNUSED) continue;
        if (tasks[i].pid == pid) return &tasks[i];
    }
    return 0;
}

/* Queue a signal for a task. A task that is blocked in a syscall is woken
 * so the signal reaches it when it returns to user mode, and the
 * interrupted call then reports EINTR. A parent parked in vfork stays
 * parked: it shares the child's address space and must not run until the
 * child is done. */
static int32_t queue_signal(task_t *target, uint32_t sig) {
    if (!target) return -(int32_t)HAKU_ESRCH;
    if (sig < 1u || sig > SCHEDULER_NSIG) return -(int32_t)HAKU_EINVAL;
    target->signal_pending |= 1u << (sig - 1u);
    if (target->state == TASK_SLEEPING || target->state == TASK_WAITING ||
        target->state == TASK_BLOCKED) {
        if (!target->vfork_child) {
            /* A futex wait ends with EINTR and leaves the queue. */
            if (target->futex_address) futex_remove_waiter(target);
            target->state = TASK_RUNNABLE;
            target->wait_pid = 0;
            target->wait_status_user = 0;
            target->wait_rusage_user = 0;
            if (target->frame) target->frame->eax = 0u - HAKU_EINTR;
        }
    }
    return 0;
}

int32_t scheduler_kill_pid(uint32_t pid, uint32_t sig) {
    task_t *target = signal_find_task(pid);
    if (!target) return -(int32_t)HAKU_ESRCH;
    /* Signal 0 only checks that the target exists. */
    if (sig == 0u) return 0;
    return queue_signal(target, sig);
}

int32_t scheduler_kill_all(uint32_t sig) {
    int32_t result = 0;
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *task = &tasks[i];
        if (task->state == TASK_UNUSED) continue;
        if (current_task >= 0 && (int)i == current_task) continue;
        if (sig == 0u) continue;
        result = queue_signal(task, sig);
        if (result < 0) return result;
    }
    return 0;
}

int32_t scheduler_kill_pgrp(uint32_t pgid, uint32_t sig) {
    int found = 0;
    if (pgid == 0u || pgid > SCHEDULER_MAX_PROCESSES)
        return -(int32_t)HAKU_ESRCH;
    if (sig > SCHEDULER_NSIG) return -(int32_t)HAKU_EINVAL;
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *task = &tasks[i];
        if (task->state == TASK_UNUSED) continue;
        if (task->pgrp != pgid) continue;
        found = 1;
        if (sig == 0u) continue;
        if (queue_signal(task, sig) < 0) return -(int32_t)HAKU_ESRCH;
    }
    return found ? 0 : -(int32_t)HAKU_ESRCH;
}

int32_t scheduler_tkill(uint32_t tid, uint32_t sig) {
    /* Haku runs one thread per process, so a thread id is a process id. */
    return scheduler_kill_pid(tid, sig);
}

int32_t scheduler_tgkill(uint32_t tgid, uint32_t tid, uint32_t sig) {
    if (tgid != tid) return -(int32_t)HAKU_ESRCH;
    return scheduler_kill_pid(tid, sig);
}

int32_t scheduler_sigaction(uint32_t sig, const haku_sigaction_t *action,
                             haku_sigaction_t *previous) {
    task_t *task;
    if (sig < 1u || sig > SCHEDULER_NSIG)
        return -(int32_t)HAKU_EINVAL;
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return -(int32_t)HAKU_ESRCH;
    task = &tasks[current_task];
    /* SIGKILL and SIGSTOP cannot be caught or ignored. */
    if (action && (sig == HAKU_SIGKILL || sig == HAKU_SIGSTOP))
        return -(int32_t)HAKU_EINVAL;
    if (previous) {
        previous->handler = task->sigactions[sig - 1u].handler;
        previous->flags = task->sigactions[sig - 1u].flags;
        previous->restorer = task->sigactions[sig - 1u].restorer;
        previous->mask = task->sigactions[sig - 1u].mask;
        previous->mask_high = 0u;
    }
    if (action) {
        task->sigactions[sig - 1u].handler = action->handler;
        task->sigactions[sig - 1u].flags = action->flags;
        task->sigactions[sig - 1u].restorer = action->restorer;
        task->sigactions[sig - 1u].mask =
            action->mask & ~((1u << (HAKU_SIGKILL - 1u)) |
                             (1u << (HAKU_SIGSTOP - 1u)));
    }
    return 0;
}

uint32_t scheduler_sigprocmask(uint32_t how, uint32_t mask,
                               uint32_t *previous) {
    task_t *task;
    uint32_t blocked;
    uint32_t old;
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX) return 0u;
    task = &tasks[current_task];
    old = task->signal_blocked;
    if (previous) *previous = old;
    blocked = mask & ~((1u << (HAKU_SIGKILL - 1u)) |
                       (1u << (HAKU_SIGSTOP - 1u)));
    if (how == HAKU_SIG_BLOCK)
        task->signal_blocked = old | blocked;
    else if (how == HAKU_SIG_UNBLOCK)
        task->signal_blocked = old & ~blocked;
    else
        task->signal_blocked = blocked;
    return old;
}

/* rt_sigreturn(2): restore the context the signal interrupted. The
 * restorer leaves esp just above the frame's return address, so the
 * frame starts one word below the user stack pointer. */
regs_t *scheduler_sigreturn(regs_t *frame) {
    uint32_t base;
    haku_sigframe_t saved;
    task_t *task;
    if (!frame || (frame->cs & 3u) != 3u) return frame;
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX) return frame;
    task = &tasks[current_task];
    base = frame->user_esp;
    if (base < 8u) return exit_current_for_signal(frame, HAKU_SIGSEGV);
    base -= 4u;
    if (!paging_user_range_valid(base, sizeof(saved), 0))
        return exit_current_for_signal(frame, HAKU_SIGSEGV);
    memcpy(&saved, (const void *)base, sizeof(saved));
    if (saved.magic != HAKU_SIGFRAME_MAGIC)
        return exit_current_for_signal(frame, HAKU_SIGSEGV);
    task->signal_blocked = saved.blocked;
    *frame = saved.registers;
    return frame;
}

/* Deliver a pending signal, if any, before the task returns to user
 * mode. The frame is rewritten so the task resumes inside the handler
 * instead of the interrupted instruction. Called from the interrupt and
 * syscall stubs with the frame they are about to restore. */
regs_t *scheduler_signal_deliver(regs_t *frame) {
    task_t *task;
    haku_sigframe_t signal_frame;
    uint32_t pending;
    uint32_t sig = 0u;
    uint32_t base;
    uint32_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint32_t mask;

    if (!frame || (frame->cs & 3u) != 3u) return frame;
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX) return frame;
    task = &tasks[current_task];
    if (task->state != TASK_RUNNABLE) return frame;
    pending = task->signal_pending & ~task->signal_blocked;
    if (!pending) return frame;
    for (uint32_t candidate = 1u; candidate <= SCHEDULER_NSIG; candidate++) {
        if (pending & (1u << (candidate - 1u))) {
            sig = candidate;
            break;
        }
    }
    if (!sig) return frame;

    task->signal_pending &= ~(1u << (sig - 1u));
    handler = task->sigactions[sig - 1u].handler;
    flags = task->sigactions[sig - 1u].flags;
    restorer = task->sigactions[sig - 1u].restorer;
    mask = task->sigactions[sig - 1u].mask;

    if (handler == HAKU_SIG_IGN) return frame;
    if (handler == HAKU_SIG_DFL) {
        /* The default disposition of most signals ends the process; the
         * wait status carries the signal number the way Linux does. */
        if (signal_default_ignored(sig)) return frame;
        return exit_current_for_signal(frame, sig);
    }

    memset(&signal_frame, 0, sizeof(signal_frame));
    signal_frame.registers = *frame;
    signal_frame.blocked = task->signal_blocked;
    signal_frame.magic = HAKU_SIGFRAME_MAGIC;
    signal_frame.argument1 = sig;
    /* "mov eax, 173; int 0x80" - the kernel's own restorer. */
    signal_frame.stub[0] = 0xB8u;
    signal_frame.stub[1] = 0xADu;
    signal_frame.stub[2] = 0x00u;
    signal_frame.stub[3] = 0x00u;
    signal_frame.stub[4] = 0x00u;
    signal_frame.stub[5] = 0xCDu;
    signal_frame.stub[6] = 0x80u;
    signal_frame.siginfo[0] = sig & 0xFFu;      /* si_signo */
    signal_frame.siginfo[4] = 0u;               /* si_errno */
    signal_frame.siginfo[8] = 0u;               /* si_code: SI_USER */
    signal_frame.ucontext.flags = 0u;
    signal_frame.ucontext.link = 0u;
    signal_frame.ucontext.esp = frame->user_esp;
    signal_frame.ucontext.eip = frame->eip;
    signal_frame.ucontext.eax = frame->eax;
    signal_frame.ucontext.ebx = frame->ebx;
    signal_frame.ucontext.ecx = frame->ecx;
    signal_frame.ucontext.edx = frame->edx;
    signal_frame.ucontext.esi = frame->esi;
    signal_frame.ucontext.edi = frame->edi;
    signal_frame.ucontext.ebp = frame->ebp;
    signal_frame.ucontext.cs = frame->cs;
    signal_frame.ucontext.eflags = frame->eflags;

    /* The frame sits below the interrupted stack pointer so the handler's
     * own calls cannot touch it. */
    base = (frame->user_esp - sizeof(signal_frame)) & ~0xFu;
    if (base < 0x40000000u)
        return exit_current_for_signal(frame, HAKU_SIGSEGV);

    signal_frame.restorer = (flags & HAKU_SA_RESTORER) && restorer ?
                            restorer :
                            (base + HAKU_SIGFRAME_STUB);
    signal_frame.argument2 = (flags & HAKU_SA_SIGINFO) ?
                             (base + HAKU_SIGFRAME_SIGINFO) : 0u;
    signal_frame.argument3 = (flags & HAKU_SA_SIGINFO) ?
                             (base + HAKU_SIGFRAME_UCONTEXT) : 0u;
    if (!paging_copy_to_user_space(&task->program.address_space, base,
                                   &signal_frame, sizeof(signal_frame)))
        return exit_current_for_signal(frame, HAKU_SIGSEGV);

    /* The handler runs with the signal blocked, plus whatever the
     * program asked for, unless SA_NODEFER says otherwise. */
    if (!(flags & HAKU_SA_NODEFER))
        task->signal_blocked |= 1u << (sig - 1u);
    task->signal_blocked |= mask;

    /* Resume in the handler with the interrupted context underneath. */
    *frame = signal_frame.registers;
    frame->eip = handler;
    frame->eax = sig;
    frame->user_esp = base;
    return frame;
}

int scheduler_exec_current(loaded_elf_t *replacement, regs_t *frame) {
    task_t *task;
    loaded_elf_t previous;
    uint32_t inherited_peak;
    irqflags_t flags;
    if (!scheduler_started || !replacement || !frame ||
        !replacement->address_space.directory_phys ||
        !replacement->entry || !replacement->user_stack ||
        current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX ||
        (frame->cs & 3u) != 3u)
        return 0;

    flags = irq_lock();
    task = &tasks[current_task];
    previous = task->program;
    inherited_peak = previous.address_space.peak_mapped_pages;
    task->program = *replacement;
    /* The process keeps its lifetime peak across an image replacement;
     * the new image's current page counts start from its own mappings. */
    if (task->program.address_space.peak_mapped_pages < inherited_peak)
        task->program.address_space.peak_mapped_pages = inherited_peak;
    replacement->address_space.directory_phys = 0;
    /* A fresh image starts with a fresh, lazily-mapped heap and no
     * anonymous mappings. */
    task->heap_brk = 0;
    task->heap_top = 0;
    task->mmap_region_count = 0;
    /* execve resets the signal dispositions to the default, but the
     * blocked mask and the pending signals survive. */
    memset(task->sigactions, 0, sizeof(task->sigactions));

    frame->gs = USER_DS;
    frame->fs = USER_FS;
    frame->es = USER_DS;
    frame->ds = USER_DS;
    frame->edi = 0;
    frame->esi = 0;
    frame->ebp = 0;
    frame->esp_dummy = 0;
    frame->ebx = 0;
    frame->edx = 0;
    frame->ecx = 0;
    frame->eax = 0;
    frame->eip = task->program.entry;
    frame->cs = USER_CS;
    frame->eflags = 0x202u;
    frame->user_esp = task->program.user_stack;
    frame->user_ss = USER_DS;
    task->frame = frame;

    paging_activate_user_space(&task->program.address_space);
    if (previous.address_space.shared) {
        /* The replaced image was the parent's address space (vfork): it is
         * still in use, so only detach from it and release the parent. */
        if (task->vfork_parent) {
            wake_vfork_parent(task->pid);
            task->vfork_parent = 0;
        }
    } else {
        paging_destroy_user_space(&previous.address_space);
    }
    irq_unlock(flags);

    klog_prefix();
    klog_write("exec: EXECVE-OK pid=");
    klog_dec(task->pid);
    klog_write(" replaced image, descriptors preserved\n");
    return 1;
}

uint32_t scheduler_current_pid(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0;
    return tasks[current_task].pid;
}

uint32_t scheduler_parent_pid(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0;
    return tasks[current_task].parent_pid;
}

uint32_t scheduler_current_pgrp(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0;
    return tasks[current_task].pgrp;
}

int32_t scheduler_get_pgrp(uint32_t pid, uint32_t *pgrp) {
    task_t *task;
    if (!pgrp) return -(int32_t)HAKU_EINVAL;
    if (pid == 0u) pid = scheduler_current_pid();
    task = signal_find_task(pid);
    if (!task) return -(int32_t)HAKU_ESRCH;
    *pgrp = task->pgrp;
    return 0;
}

/* A group is live when its leader (the task whose pid is the group id) is
 * still in the table, or when one of its members is. The leader may not have
 * joined yet: the parent and the second child of a pipeline both call
 * setpgid before the first child has run, so the pid alone is enough. */
int scheduler_pgrp_valid(uint32_t pgid) {
    if (pgid == 0u || pgid > SCHEDULER_MAX_PROCESSES) return 0;
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        if (tasks[i].state == TASK_UNUSED) continue;
        if (tasks[i].pid == pgid || tasks[i].pgrp == pgid) return 1;
    }
    return 0;
}

int32_t scheduler_set_pgrp(uint32_t pid, uint32_t pgid) {
    task_t *self;
    task_t *target;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return -(int32_t)HAKU_ESRCH;
    self = &tasks[current_task];
    if (pid == 0u) pid = self->pid;
    if (pgid == 0u) pgid = pid;
    if (pid > SCHEDULER_MAX_PROCESSES || pgid > SCHEDULER_MAX_PROCESSES)
        return -(int32_t)HAKU_ESRCH;
    target = signal_find_task(pid);
    if (!target) return -(int32_t)HAKU_ESRCH;
    /* A process may only move itself or one of its children. */
    if (target != self && target->parent_pid != self->pid)
        return -(int32_t)HAKU_EPERM;
    /* The target is allowed to create a group named after itself even when
     * nobody has joined it yet; joining any other group needs a live one. */
    if (pgid != target->pid && !scheduler_pgrp_valid(pgid))
        return -(int32_t)HAKU_EPERM;
    target->pgrp = pgid;
    return 0;
}

uint32_t scheduler_setsid(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX) return 0;
    /* Haku has no controlling-terminal/session teardown to do, and a shell
     * calls setsid on itself even when it is already a leader, so the call
     * always succeeds and just makes the caller a leader of both. */
    tasks[current_task].pgrp = tasks[current_task].pid;
    tasks[current_task].sid = tasks[current_task].pid;
    return tasks[current_task].pid;
}

int32_t scheduler_get_sid(uint32_t pid, uint32_t *sid) {
    task_t *task;
    if (!sid) return -(int32_t)HAKU_EINVAL;
    if (pid == 0u) pid = scheduler_current_pid();
    task = signal_find_task(pid);
    if (!task) return -(int32_t)HAKU_ESRCH;
    *sid = task->sid;
    return 0;
}

int32_t scheduler_get_cwd(char *path, uint32_t size) {
    uint32_t length;
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX || !path)
        return -(int32_t)HAKU_EINVAL;
    length = (uint32_t)strlen(tasks[current_task].cwd);
    if (size <= length) return -(int32_t)HAKU_ERANGE;
    memcpy(path, tasks[current_task].cwd,
           length + 1u);
    return (int32_t)length;
}

int32_t scheduler_set_cwd(const char *path) {
    uint32_t length;
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX || !path)
        return -(int32_t)HAKU_EINVAL;
    length = (uint32_t)strlen(path);
    if (length >= SCHEDULER_PATH_MAX) return -(int32_t)HAKU_ENAMETOOLONG;
    memcpy(tasks[current_task].cwd, path, length + 1u);
    return 1;
}

int32_t scheduler_get_comm(char *name, uint32_t size) {
    uint32_t length;
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX || !name)
        return -(int32_t)HAKU_EINVAL;
    length = (uint32_t)strlen(tasks[current_task].comm);
    if (size <= length) return -(int32_t)HAKU_ERANGE;
    memcpy(name, tasks[current_task].comm, length + 1u);
    return (int32_t)length;
}

int32_t scheduler_set_comm(const char *name) {
    uint32_t length;
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX || !name)
        return -(int32_t)HAKU_EINVAL;
    length = (uint32_t)strlen(name);
    if (length >= SCHEDULER_COMM_MAX) length = SCHEDULER_COMM_MAX - 1u;
    memset(tasks[current_task].comm, 0, sizeof(tasks[current_task].comm));
    memcpy(tasks[current_task].comm, name, length);
    return 1;
}

regs_t *scheduler_yield(regs_t *frame) {
    int next;
    if (!scheduler_started || !frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;
    /* The caller stays runnable: a yield only hands the CPU to another
     * ready task. Remember its frame so the switch can come straight back
     * to it when it is selected again. */
    tasks[current_task].frame = frame;
    next = find_next_task();
    if (next == current_task)
        return frame;
    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame : tasks[next].frame;
}

void scheduler_note_syscall(void) {
    if (current_task >= 0 && current_task < SCHEDULER_IDLE_INDEX)
        tasks[current_task].syscall_count++;
}

/* Fill a getusage(0x1001) snapshot for the calling process. Memory counts
 * come from the address space's incremental accounting; resource counts are
 * updated by the timer IRQ, syscall entry, and blocking paths. */
int scheduler_get_usage(haku_usage_t *usage) {
    task_t *task;
    address_space_t *space;
    uint32_t kernel_pages;
    if (!usage || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return -(int32_t)HAKU_EINVAL;

    task = &tasks[current_task];
    space = &task->program.address_space;
    memset(usage, 0, sizeof(*usage));
    usage->pid = task->pid;
    usage->user_ticks = task->user_ticks;
    usage->kernel_ticks = task->kernel_ticks;
    usage->syscalls = task->syscall_count;
    usage->voluntary_switches = task->voluntary_switches;
    usage->involuntary_switches = task->involuntary_switches;
    usage->mapped_pages = space->mapped_pages;
    usage->peak_pages = space->peak_mapped_pages;
    usage->page_tables = space->page_tables;
    usage->kernel_stack_pages = TASK_KERNEL_STACK_PAGES;
    if (task->heap_brk > USER_HEAP_BASE)
        usage->heap_bytes = task->heap_brk - USER_HEAP_BASE;
    for (uint32_t i = 0; i < task->mmap_region_count; i++)
        usage->mmap_bytes += task->mmap_regions[i].size;

    usage->resident_bytes = usage->mapped_pages * FRAME_SIZE;
    kernel_pages = usage->page_tables + 1u + usage->kernel_stack_pages;
    usage->kernel_bytes = kernel_pages * FRAME_SIZE;
    usage->total_bytes = usage->resident_bytes + usage->kernel_bytes;
    usage->peak_bytes = usage->peak_pages * FRAME_SIZE;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wait queues.                                                       */
/*                                                                    */
/* Parking a task is just a state transition: it keeps its kernel     */
/* stack, and when the event fires the scheduler resumes it right      */
/* after the hlt that follows wait_queue_sleep. No frame is crafted    */
/* here; either the waking IRQ returns straight to the halted task or  */
/* the timer IRQ saves that interrupted frame for the next activation. */
/* ------------------------------------------------------------------ */
static int current_slot(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return -1;
    return current_task;
}

int wait_queue_sleep(wait_queue_t *queue) {
    int slot;
    if (!queue) return 0;
    slot = current_slot();
    if (slot < 0) return 0;
    queue->waiters |= 1u << (uint32_t)slot;
    tasks[slot].state = TASK_BLOCKED;
    return 1;
}

void wait_queue_cancel(wait_queue_t *queue) {
    int slot;
    if (!queue) return;
    slot = current_slot();
    if (slot < 0) return;
    queue->waiters &= ~(1u << (uint32_t)slot);
    if (tasks[slot].state == TASK_BLOCKED)
        tasks[slot].state = TASK_RUNNABLE;
}

int wait_queue_wake_all(wait_queue_t *queue) {
    uint32_t mask;
    int woken = 0;
    if (!queue || !queue->waiters) return 0;
    /* Drop the whole bitmap first: a waiter that already cancelled must
     * not be woken again by a stale bit. */
    mask = queue->waiters;
    queue->waiters = 0;
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        if (!(mask & (1u << i))) continue;
        if (tasks[i].state != TASK_BLOCKED) continue;
        tasks[i].state = TASK_RUNNABLE;
        woken++;
    }
    return woken;
}

int32_t scheduler_spawn(loaded_elf_t *program) {
    irqflags_t flags;
    uint32_t parent_pid;
    uint32_t parent_pgrp;
    uint32_t parent_sid;
    uint32_t slot;
    task_t *child = 0;

    if (!scheduler_started || !program ||
        !program->address_space.directory_phys ||
        !program->entry || !program->user_stack ||
        current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return -(int32_t)HAKU_ENOMEM;

    /* Reserve a slot without holding interrupts across the VFS inherit:
     * vfs_inherit takes the task-context VFS spinlock, which must never be
     * acquired with interrupts disabled. A STARTING task is ignored by the
     * scheduler until it is fully built. */
    flags = irq_lock();
    release_reaped_slots();
    parent_pid = tasks[current_task].pid;
    parent_pgrp = tasks[current_task].pgrp;
    parent_sid = tasks[current_task].sid;
    for (slot = 1; slot < SCHEDULER_MAX_PROCESSES; slot++) {
        if (tasks[slot].state == TASK_UNUSED) {
            child = &tasks[slot];
            break;
        }
    }
    if (!child) {
        irq_unlock(flags);
        return -(int32_t)HAKU_EAGAIN;
    }
    memset(child, 0, sizeof(*child));
    child->pid = slot + 1u;
    child->parent_pid = parent_pid;
    child->state = TASK_STARTING;
    /* A spawned program replaces the image of a would-be child, so it
     * inherits the caller's group and session like an exec does. */
    child->pgrp = parent_pgrp;
    child->sid = parent_sid;
    init_task_identity(child);
    irq_unlock(flags);

    child->program = *program;
    if (!initialize_stack(child, 1)) {
        flags = irq_lock();
        memset(child, 0, sizeof(*child));
        irq_unlock(flags);
        return -12;
    }
    /* Inherit the parent's descriptors: the child shares the same open
     * file descriptions (refcounted), so its standard handles and any
     * redirections work from the first instruction. They are released
     * together when the child exits. */
    if (vfs_inherit(parent_pid, child->pid) != 1) {
        flags = irq_lock();
        frame_free_pages((void *)child->kernel_stack,
                         TASK_KERNEL_STACK_PAGES);
        memset(child, 0, sizeof(*child));
        irq_unlock(flags);
        return -(int32_t)HAKU_EMFILE;
    }
    program->address_space.directory_phys = 0;

    flags = irq_lock();
    child->state = TASK_RUNNABLE;
    irq_unlock(flags);
    klog_prefix();
    klog_write("sched: SPAWN-OK parent=");
    klog_dec(parent_pid);
    klog_write(" child=");
    klog_dec(child->pid);
    klog_write("\n");
    return (int32_t)child->pid;
}

/* Build the child's kernel stack and register frame for a fork: the frame
 * is a copy of the parent's syscall frame, so the child resumes at the
 * same instruction with eax = 0 (the fork return value). */
static int initialize_fork_stack(task_t *task, const regs_t *parent_frame) {
    uint32_t stack_bytes = TASK_KERNEL_STACK_PAGES * FRAME_SIZE;
    uint32_t stack_top;
    regs_t *frame;

    task->kernel_stack = (uint32_t)frame_alloc_pages(TASK_KERNEL_STACK_PAGES);
    if (!task->kernel_stack ||
        task->kernel_stack + stack_bytes > PAGING_PHYS_LIMIT) {
        if (task->kernel_stack)
            frame_free_pages((void *)task->kernel_stack,
                             TASK_KERNEL_STACK_PAGES);
        task->kernel_stack = 0;
        return 0;
    }
    stack_top = (uint32_t)PHYS_TO_VIRT(task->kernel_stack) + stack_bytes;
    frame = (regs_t *)(stack_top - sizeof(regs_t));
    memcpy(frame, parent_frame, sizeof(*frame));
    frame->eax = 0u;
    task->frame = frame;
    return 1;
}

int32_t scheduler_fork(regs_t *frame) {
    irqflags_t flags;
    uint32_t parent_pid;
    uint32_t slot;
    task_t *child = 0;
    task_t *parent;

    if (!scheduler_started || !frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return -(int32_t)HAKU_EINVAL;
    if ((frame->cs & 3u) != 3u)
        return -(int32_t)HAKU_EINVAL;
    parent = &tasks[current_task];

    /* Reserve a slot first: a STARTING task is ignored by the scheduler
     * until it is fully built. */
    flags = irq_lock();
    release_reaped_slots();
    parent_pid = parent->pid;
    for (slot = 1; slot < SCHEDULER_MAX_PROCESSES; slot++) {
        if (tasks[slot].state == TASK_UNUSED) {
            child = &tasks[slot];
            break;
        }
    }
    if (!child) {
        irq_unlock(flags);
        return -(int32_t)HAKU_EAGAIN;
    }
    memset(child, 0, sizeof(*child));
    child->pid = slot + 1u;
    child->parent_pid = parent_pid;
    child->state = TASK_STARTING;
    inherit_task_identity(child, parent);
    irq_unlock(flags);

    /* The child gets its own address space that shares the parent's frames
     * copy-on-write: nothing is copied until either side writes. */
    if (!paging_create_user_space(&child->program.address_space))
        goto failed;
    if (!paging_share_user_space(&parent->program.address_space,
                                 &child->program.address_space))
        goto failed;
    child->program.entry = parent->program.entry;
    child->program.user_stack = parent->program.user_stack;

    if (!initialize_fork_stack(child, frame))
        goto failed;

    /* The child inherits the parent's descriptors and shares the same open
     * file descriptions (refcounted), exactly like a Linux fork. */
    if (vfs_inherit(parent_pid, child->pid) != 1)
        goto failed;

    /* The heap break and the anonymous mappings belong to the child too. */
    child->heap_brk = parent->heap_brk;
    child->heap_top = parent->heap_top;
    child->mmap_region_count = parent->mmap_region_count;
    memcpy(child->mmap_regions, parent->mmap_regions,
           sizeof(child->mmap_regions));

    /* A forked child starts with the default signal dispositions and an
     * empty pending set, but it keeps the parent's blocked mask. */
    child->signal_blocked = parent->signal_blocked;

    flags = irq_lock();
    child->state = TASK_RUNNABLE;
    irq_unlock(flags);
    klog_prefix();
    klog_write("sched: FORK-OK parent=");
    klog_dec(parent_pid);
    klog_write(" child=");
    klog_dec(child->pid);
    klog_write("\n");
    return (int32_t)child->pid;

failed:
    /* The partially built child is discarded whole: its address space and
     * kernel stack are released and the slot returns to the table. It is
     * still TASK_STARTING here, so the scheduler never runs it. */
    release_task_resources(child);
    flags = irq_lock();
    memset(child, 0, sizeof(*child));
    irq_unlock(flags);
    return -(int32_t)HAKU_ENOMEM;
}

/* vfork(2): the child runs in the parent's address space until it calls
 * execve or exits, and the parent is suspended until then (that is the
 * whole point: the child owns the parent's memory and stack). The child
 * therefore returns 0 and the parent returns 0 once the child is done,
 * exactly like Linux. */
regs_t *scheduler_vfork(regs_t *frame) {
    irqflags_t flags;
    uint32_t parent_pid;
    uint32_t slot;
    task_t *child = 0;
    task_t *parent;
    int next;

    if (!scheduler_started || !frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX ||
        (frame->cs & 3u) != 3u) {
        frame->eax = 0u - HAKU_EINVAL;
        return frame;
    }
    parent = &tasks[current_task];

    flags = irq_lock();
    release_reaped_slots();
    parent_pid = parent->pid;
    for (slot = 1; slot < SCHEDULER_MAX_PROCESSES; slot++) {
        if (tasks[slot].state == TASK_UNUSED) {
            child = &tasks[slot];
            break;
        }
    }
    if (!child) {
        frame->eax = 0u - HAKU_EAGAIN;
        irq_unlock(flags);
        return frame;
    }
    memset(child, 0, sizeof(*child));
    child->pid = slot + 1u;
    child->parent_pid = parent_pid;
    child->state = TASK_STARTING;
    child->vfork_parent = parent_pid;
    inherit_task_identity(child, parent);
    irq_unlock(flags);

    /* Borrow the parent's address space: same CR3, same frames, no copy.
     * Nothing may tear it down while the child runs (see
     * paging_destroy_user_space), and the parent cannot run until the
     * child has finished with it. */
    child->program = parent->program;
    child->program.address_space.shared = 1;
    if (!initialize_fork_stack(child, frame))
        goto failed;
    if (vfs_inherit(parent_pid, child->pid) != 1)
        goto failed;

    child->heap_brk = parent->heap_brk;
    child->heap_top = parent->heap_top;
    child->mmap_region_count = parent->mmap_region_count;
    memcpy(child->mmap_regions, parent->mmap_regions,
           sizeof(child->mmap_regions));
    /* Like fork, the vfork child gets the default dispositions. */
    child->signal_blocked = parent->signal_blocked;

    klog_prefix();
    klog_write("sched: VFORK-OK parent=");
    klog_dec(parent_pid);
    klog_write(" child=");
    klog_dec(child->pid);
    klog_write(" shared-space\n");

    /* Park the parent and switch to the child with interrupts masked: the
     * saved frame is the only way back in, so no timer tick may replace it
     * (a preempted frame resumes inside this function, not after the
     * syscall). wake_vfork_parent() releases the parent with the child pid
     * once the child execs or exits. */
    flags = irq_lock();
    child->state = TASK_RUNNABLE;
    parent->state = TASK_WAITING;
    parent->vfork_child = child->pid;
    parent->frame = frame;
    frame->eax = 0u;
    next = find_next_task();
    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;

failed:
    /* The parent never started the child, so it must not stay parked. The
     * borrowed space belongs to the parent and is not released here. */
    release_task_resources(child);
    flags = irq_lock();
    memset(child, 0, sizeof(*child));
    irq_unlock(flags);
    frame->eax = 0u - HAKU_ENOMEM;
    return frame;
}

regs_t *scheduler_waitpid(uint32_t pid, uint32_t user_status,
                          uint32_t options, regs_t *frame) {
    task_t *parent;
    task_t *zombie = 0;
    int has_child = 0;
    int next;
    irqflags_t flags;

    if (!frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;
    flags = irq_lock();
    if (options & ~(uint32_t)(HAKU_WUNTRACED | HAKU_WCONTINUED) || pid == 0) {
        frame->eax = 0u - HAKU_EINVAL;
        irq_unlock(flags);
        return frame;
    }
    if (pid != 0xFFFFFFFFu && pid > 0x7FFFFFFFu) {
        frame->eax = 0u - HAKU_EINVAL;
        irq_unlock(flags);
        return frame;
    }
    if (pid != 0xFFFFFFFFu && pid > SCHEDULER_MAX_PROCESSES) {
        frame->eax = 0u - HAKU_ECHILD;
        irq_unlock(flags);
        return frame;
    }

    parent = &tasks[current_task];
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *child = &tasks[i];
        if (child->state == TASK_UNUSED ||
            child->state == TASK_REAP_PENDING ||
            child->parent_pid != parent->pid)
            continue;
        if (pid != 0xFFFFFFFFu && child->pid != pid) continue;
        has_child = 1;
        if (child->state == TASK_ZOMBIE) {
            zombie = child;
            break;
        }
    }
    if (!has_child) {
        frame->eax = 0u - HAKU_ECHILD;
        irq_unlock(flags);
        return frame;
    }

    if (zombie) {
        parent->frame = frame;
        parent->wait_status_user = user_status;
        parent->wait_rusage_user = 0;
        if (!deliver_wait_result(parent, zombie, zombie->exit_status)) {
            frame->eax = 0u - 14u;
            irq_unlock(flags);
            return frame;
        }
        zombie->state = TASK_REAP_PENDING;
        irq_unlock(flags);
        return frame;
    }

    parent->state = TASK_WAITING;
    parent->wait_pid = pid;
    parent->wait_status_user = user_status;
    parent->wait_rusage_user = 0;
    parent->frame = frame;
    next = find_next_task();
    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

/* wait4(2): the Linux form of waitpid. It accepts WNOHANG, treats pid 0
 * as "any child" (Haku has no process groups to wait on), ignores
 * WUNTRACED/WCONTINUED because a task never stops or resumes, and writes a
 * rusage block when the caller passes one. */
regs_t *scheduler_wait4(uint32_t pid, uint32_t user_status, uint32_t options,
                        uint32_t user_rusage, regs_t *frame) {
    task_t *parent;
    task_t *zombie = 0;
    int has_child = 0;
    int next;
    irqflags_t flags;

    if (!frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;
    flags = irq_lock();
    if (options & ~(uint32_t)(HAKU_WNOHANG | HAKU_WUNTRACED |
                              HAKU_WCONTINUED)) {
        frame->eax = 0u - HAKU_EINVAL;
        irq_unlock(flags);
        return frame;
    }
    if (pid == 0) pid = 0xFFFFFFFFu;
    if (pid != 0xFFFFFFFFu && pid > 0x7FFFFFFFu) {
        frame->eax = 0u - HAKU_EINVAL;
        irq_unlock(flags);
        return frame;
    }
    if (pid != 0xFFFFFFFFu && pid > SCHEDULER_MAX_PROCESSES) {
        frame->eax = 0u - HAKU_ECHILD;
        irq_unlock(flags);
        return frame;
    }

    parent = &tasks[current_task];
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *child = &tasks[i];
        if (child->state == TASK_UNUSED ||
            child->state == TASK_REAP_PENDING ||
            child->parent_pid != parent->pid)
            continue;
        if (pid != 0xFFFFFFFFu && child->pid != pid) continue;
        has_child = 1;
        if (child->state == TASK_ZOMBIE) {
            zombie = child;
            break;
        }
    }
    if (!has_child) {
        frame->eax = 0u - HAKU_ECHILD;
        irq_unlock(flags);
        return frame;
    }

    if (zombie) {
        parent->frame = frame;
        parent->wait_status_user = user_status;
        parent->wait_rusage_user = user_rusage;
        if (!deliver_wait_result(parent, zombie, zombie->exit_status)) {
            frame->eax = 0u - 14u;
            parent->wait_rusage_user = 0;
            irq_unlock(flags);
            return frame;
        }
        parent->wait_rusage_user = 0;
        zombie->state = TASK_REAP_PENDING;
        irq_unlock(flags);
        return frame;
    }

    if (options & HAKU_WNOHANG) {
        /* No child has exited yet: report "nothing ready" and stay
         * runnable instead of blocking. */
        frame->eax = 0u;
        irq_unlock(flags);
        return frame;
    }

    parent->state = TASK_WAITING;
    parent->wait_pid = pid;
    parent->wait_status_user = user_status;
    parent->wait_rusage_user = user_rusage;
    parent->frame = frame;
    next = find_next_task();
    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

/* ------------------------------------------------------------------ */
/* Futex                                                             */
/* ------------------------------------------------------------------ */

/* A futex is a 32-bit word in user memory that a task can wait on.
 * Waiters are parked in the kernel and woken by FUTEX_WAKE. Haku has
 * one thread per process and no shared memory between processes, so the
 * number of distinct futexes in use at the same time is bounded by the
 * number of tasks: a small table of queues is enough and needs no
 * allocation.
 *
 * A private futex (FUTEX_PRIVATE_FLAG) belongs to one process, so its
 * key carries the pid; a shared futex is keyed by its address alone. */
#define FUTEX_MAX_QUEUES 8u

typedef struct {
    uint32_t address;   /* the futex word; 0 means the slot is free */
    uint32_t pid;       /* the owner of a private futex */
    uint32_t private_futex;
    uint32_t waiters[SCHEDULER_MAX_PROCESSES];
    uint32_t waiter_count;
} futex_queue_t;

static futex_queue_t futex_queues[FUTEX_MAX_QUEUES];

/* The queue that holds the waiters of a futex, or 0 when there is none
 * yet. */
static futex_queue_t *futex_find_queue(uint32_t address, uint32_t pid,
                                        uint32_t private_futex) {
    for (uint32_t i = 0; i < FUTEX_MAX_QUEUES; i++) {
        futex_queue_t *queue = &futex_queues[i];
        if (!queue->address) continue;
        if (queue->address != address) continue;
        if (queue->private_futex != private_futex) continue;
        if (private_futex && queue->pid != pid) continue;
        return queue;
    }
    return 0;
}

/* The queue a new waiter joins: an existing one, or a free slot. */
static futex_queue_t *futex_queue_for(uint32_t address, uint32_t pid,
                                       uint32_t private_futex) {
    futex_queue_t *queue = futex_find_queue(address, pid, private_futex);
    if (queue) return queue;
    for (uint32_t i = 0; i < FUTEX_MAX_QUEUES; i++) {
        if (!futex_queues[i].address) {
            futex_queues[i].address = address;
            futex_queues[i].pid = pid;
            futex_queues[i].private_futex = private_futex;
            futex_queues[i].waiter_count = 0;
            return &futex_queues[i];
        }
    }
    return 0;
}

static void futex_queue_release(futex_queue_t *queue) {
    queue->address = 0;
    queue->pid = 0;
    queue->private_futex = 0;
    queue->waiter_count = 0;
}

/* Drop a task from the futex it waits on. Called when the wait ends for
 * any reason: a wake, a timeout, a signal, or the task's exit. */
static void futex_remove_waiter(task_t *task) {
    futex_queue_t *queue;
    uint32_t index;

    if (!task->futex_address) return;
    queue = futex_find_queue(task->futex_address, task->pid,
                              task->futex_private);
    task->futex_address = 0;
    task->futex_private = 0;
    if (!queue) return;
    for (index = 0; index < queue->waiter_count; index++) {
        if (queue->waiters[index] != (uint32_t)(task - tasks)) continue;
        for (uint32_t i = index; i + 1u < queue->waiter_count; i++)
            queue->waiters[i] = queue->waiters[i + 1u];
        queue->waiter_count--;
        break;
    }
    if (!queue->waiter_count) futex_queue_release(queue);
}

/* FUTEX_WAKE: release up to `count` waiters of a futex. The woken tasks
 * see a return value of 0. */
uint32_t scheduler_futex_wake(uint32_t address, uint32_t count,
                              uint32_t mask, uint32_t private_futex) {
    futex_queue_t *queue;
    uint32_t woken = 0;
    uint32_t pid = scheduler_current_pid();

    if (count == 0) return 0;
    queue = futex_find_queue(address, pid, private_futex);
    if (!queue) return 0;
    while (woken < count && queue->waiter_count) {
        uint32_t slot = queue->waiters[0];
        task_t *waiter;
        if (slot >= SCHEDULER_MAX_PROCESSES) break;
        waiter = &tasks[slot];
        for (uint32_t i = 0; i + 1u < queue->waiter_count; i++)
            queue->waiters[i] = queue->waiters[i + 1u];
        queue->waiter_count--;
        waiter->futex_address = 0;
        waiter->futex_private = 0;
        if (waiter->state != TASK_SLEEPING) continue;
        waiter->state = TASK_RUNNABLE;
        if (waiter->frame) waiter->frame->eax = 0;
        woken++;
    }
    if (!queue->waiter_count) futex_queue_release(queue);
    return woken;
}

/* FUTEX_WAIT: park the caller until the futex is woken. `ticks` is the
 * timeout in timer ticks (0 means no timeout). The value check itself
 * happens in the syscall layer, which reads the user word. */
regs_t *scheduler_futex_wait(uint32_t address, uint32_t pid,
                             uint32_t private_futex, uint32_t ticks,
                             regs_t *frame) {
    task_t *task;
    futex_queue_t *queue;
    int next;

    if (!frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;

    (void)irq_lock();
    task = &tasks[current_task];
    queue = futex_queue_for(address, pid, private_futex);
    if (!queue) {
        frame->eax = 0u - HAKU_EAGAIN;
        return frame;
    }
    if (queue->waiter_count >= SCHEDULER_MAX_PROCESSES) {
        frame->eax = 0u - HAKU_EAGAIN;
        return frame;
    }
    queue->waiters[queue->waiter_count++] = (uint32_t)current_task;
    task->futex_address = address;
    task->futex_private = private_futex;
    frame->eax = 0;
    if (ticks) {
        task->wake_tick = pit_ticks() + ticks;
    } else {
        /* No timeout: a deadline far in the future that the signed tick
         * comparison never reaches. */
        task->wake_tick = pit_ticks() + 0x3FFFFFFFu;
    }
    task->state = TASK_SLEEPING;
    task->frame = frame;
    next = find_next_task();
    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

regs_t *scheduler_sleep_ticks(uint32_t ticks, regs_t *frame) {
    task_t *task;
    int next;

    if (!frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;
    if (ticks == 0) {
        frame->eax = 0;
        return frame;
    }

    (void)irq_lock();
    task = &tasks[current_task];
    frame->eax = 0;
    task->wake_tick = pit_ticks() + ticks;
    task->state = TASK_SLEEPING;
    task->frame = frame;
    next = find_next_task();
    activate_task(next);
    /* IRQs stay masked here; the returned frame's eflags restore them
     * when the isr return path executes iret. */
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

/* Map [start, end) as zero-filled user pages in the given address space
 * (page-aligned range), writable when `writable` is set. On failure the
 * pages mapped by this call are unmapped again, so the caller can report
 * -ENOMEM without leaking address space or frames. */
static int map_user_zero_pages(address_space_t *space, uint32_t start,
                               uint32_t end, int writable) {
    uint32_t mapped = start;
    for (uint32_t address = start; address < end; address += USER_PAGE_SIZE) {
        void *page = frame_alloc();
        uint32_t physical;
        if (!page) break;
        physical = (uint32_t)page;
        memset(PHYS_TO_VIRT(physical), 0, USER_PAGE_SIZE);
        if (!paging_map_user_page(space, address, physical, writable)) {
            frame_free(page);
            break;
        }
        mapped = address + USER_PAGE_SIZE;
    }
    if (mapped == end) return 1;
    while (mapped > start) {
        mapped -= USER_PAGE_SIZE;
        paging_unmap_user_page(space, mapped);
    }
    return 0;
}

/* brk(2) with a fixed heap region: grow or shrink the per-process break,
 * mapping zero-filled pages as it rises and releasing them as it falls.
 * The break is page-rounded and never returns below USER_HEAP_BASE. A zero
 * request just queries the current break; an invalid size returns
 * -EINVAL/-ENOMEM. */
/* Demand-grown user stack. A process starts with two pages, which is all a
 * libc's start-up needs, but any program that recurses or puts a buffer on
 * the stack walks off them: `sort` faulted 9 KiB below the stack top and
 * was killed. Linux grows the stack on the fault instead of reserving it up
 * front, and so does this. The growth is bounded so runaway recursion still
 * dies at a predictable point instead of eating the address space down to
 * the mmap window at 0x60000000. */
int scheduler_grow_user_stack(uint32_t fault_addr) {
    task_t *task;
    address_space_t *space;
    uint32_t page;
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX) return 0;
    if (fault_addr >= USER_STACK_TOP) return 0;
    if (fault_addr < USER_STACK_TOP - USER_STACK_GROWTH) return 0;
    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0;
    page = fault_addr & 0xFFFFF000u;
    if (page < USER_STACK_TOP - USER_STACK_GROWTH) return 0;
    return map_user_zero_pages(space, page, page + USER_PAGE_SIZE, 1);
}

uint32_t scheduler_brk(uint32_t requested) {
    task_t *task;
    address_space_t *space;
    uint32_t current;
    uint32_t rounded;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0u - HAKU_EINVAL;
    if (task->heap_brk == 0) task->heap_brk = USER_HEAP_BASE;
    if (task->heap_top == 0) task->heap_top = USER_HEAP_BASE;
    current = task->heap_brk;
    if (requested == 0) return current;
    if (requested < USER_HEAP_BASE) return 0u - HAKU_EINVAL;
    if (requested > USER_HEAP_LIMIT) return 0u - HAKU_ENOMEM;

    rounded = (requested + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    if (rounded > task->heap_top) {
        if (!map_user_zero_pages(space, task->heap_top, rounded, 1))
            return 0u - HAKU_ENOMEM;
        task->heap_top = rounded;
    } else if (rounded < task->heap_top) {
        for (uint32_t address = rounded; address < task->heap_top;
             address += USER_PAGE_SIZE)
            paging_unmap_user_page(space, address);
        task->heap_top = rounded;
    }
    /* Linux returns exactly the break the caller asked for, even though it
     * only guarantees the pages up to the next boundary are usable. A libc
     * checks that return value against its own request, and musl treats a
     * mismatch as a failed brk and falls back to mmap for every heap
     * expansion. Haku only allows eight mmap regions per task, so a program
     * that made a few hundred allocations ran out of regions and reported
     * "out of memory" with megabytes free. Returning the request keeps the
     * heap on brk, which is unbounded here. */
    task->heap_brk = requested;
    return requested;
}

/* First fit for a page-aligned anonymous mapping, scanning the sorted
 * region list so released address space is reused. Returns 0 when the
 * mapping does not fit. */
static uint32_t mmap_find_gap(const task_t *task, uint32_t size) {
    uint32_t candidate = USER_MMAP_BASE;
    for (uint32_t i = 0; i < task->mmap_region_count; i++) {
        uint32_t base = task->mmap_regions[i].base;
        uint32_t end = base + task->mmap_regions[i].size;
        if (candidate + size <= base) return candidate;
        if (candidate < end) candidate = end;
    }
    if (candidate > USER_MMAP_LIMIT ||
        size > USER_MMAP_LIMIT - candidate)
        return 0;
    return candidate;
}

/* Insert a tracked region, keeping the table sorted by base. */
static void mmap_insert_region_prot(task_t *task, uint32_t base,
                                     uint32_t size, uint32_t prot,
                                     uint32_t flags) {
    uint32_t index = 0;
    while (index < task->mmap_region_count &&
           task->mmap_regions[index].base < base)
        index++;
    for (uint32_t i = task->mmap_region_count; i > index; i--)
        task->mmap_regions[i] = task->mmap_regions[i - 1u];
    task->mmap_regions[index].base = base;
    task->mmap_regions[index].size = size;
    task->mmap_regions[index].prot = prot;
    task->mmap_regions[index].flags = flags;
    task->mmap_region_count++;
}

static void mmap_insert_region(task_t *task, uint32_t base, uint32_t size) {
    mmap_insert_region_prot(task, base, size, HAKU_PROT_READ | HAKU_PROT_WRITE,
                            HAKU_MAP_PRIVATE);
}

/* The tracked region that fully contains [start, end), or
 * TASK_MAX_MMAP_REGIONS when there is none. */
static uint32_t mmap_find_region(const task_t *task, uint32_t start,
                                  uint32_t end) {
    for (uint32_t i = 0; i < task->mmap_region_count; i++) {
        uint32_t base = task->mmap_regions[i].base;
        uint32_t size = task->mmap_regions[i].size;
        if (start >= base && end <= base + size) return i;
    }
    return TASK_MAX_MMAP_REGIONS;
}

/* True when no tracked region overlaps [start, end). */
static int mmap_range_free(const task_t *task, uint32_t start, uint32_t end) {
    for (uint32_t i = 0; i < task->mmap_region_count; i++) {
        uint32_t base = task->mmap_regions[i].base;
        uint32_t size = task->mmap_regions[i].size;
        if (start < base + size && end > base) return 0;
    }
    return 1;
}

/* mmap2(2) and the old mmap(2): anonymous mappings with the full
 * protection set, MAP_FIXED placement, and the shared/private flag.
 * File-backed mappings are not supported (Haku has no page cache), so
 * MAP_ANONYMOUS is required. Returns the mapped base or -errno. */
uint32_t scheduler_mmap2(uint32_t address, uint32_t length, uint32_t prot,
                         uint32_t flags) {
    task_t *task;
    address_space_t *space;
    uint32_t rounded;
    uint32_t base;
    uint32_t map_flags;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    if (length == 0) return 0u - HAKU_EINVAL;
    if (prot & ~(uint32_t)(HAKU_PROT_READ | HAKU_PROT_WRITE | HAKU_PROT_EXEC))
        return 0u - HAKU_EINVAL;
    if (flags & ~(uint32_t)(HAKU_MAP_SHARED | HAKU_MAP_PRIVATE |
                            HAKU_MAP_FIXED | HAKU_MAP_ANONYMOUS))
        return 0u - HAKU_EINVAL;
    /* One of the two sharing modes must be given, and they are mutually
     * exclusive. */
    if ((flags & (HAKU_MAP_SHARED | HAKU_MAP_PRIVATE)) != HAKU_MAP_SHARED &&
        (flags & (HAKU_MAP_SHARED | HAKU_MAP_PRIVATE)) != HAKU_MAP_PRIVATE)
        return 0u - HAKU_EINVAL;
    if (!(flags & HAKU_MAP_ANONYMOUS)) return 0u - HAKU_EINVAL;

    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0u - HAKU_EINVAL;
    if (length > USER_MMAP_LIMIT - USER_MMAP_BASE) return 0u - HAKU_ENOMEM;
    if (task->mmap_region_count >= TASK_MAX_MMAP_REGIONS)
        return 0u - HAKU_ENOMEM;

    rounded = (length + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    if (flags & HAKU_MAP_FIXED) {
        /* A fixed mapping must land on a free, page-aligned range inside
         * the mmap window: the image, the stack, and the heap are never
         * replaced. */
        if ((address & 0xFFFu) != 0u) return 0u - HAKU_EINVAL;
        if (address < USER_MMAP_BASE || address > USER_MMAP_LIMIT - rounded)
            return 0u - HAKU_EINVAL;
        if (!mmap_range_free(task, address, address + rounded))
            return 0u - HAKU_EINVAL;
        base = address;
    } else {
        base = mmap_find_gap(task, rounded);
        if (!base) return 0u - HAKU_ENOMEM;
    }

    if (!map_user_zero_pages(space, base, base + rounded, 1))
        return 0u - HAKU_ENOMEM;
    /* The requested protection is applied after the pages exist: PROT_NONE
     * keeps the pages mapped but unreachable from user mode. */
    if (!paging_protect_user_range(space, base, rounded,
                                   prot != (uint32_t)HAKU_PROT_NONE,
                                   (prot & HAKU_PROT_WRITE) != 0u)) {
        for (uint32_t page = base; page < base + rounded;
             page += USER_PAGE_SIZE)
            paging_unmap_user_page(space, page);
        return 0u - HAKU_ENOMEM;
    }
    map_flags = flags & (uint32_t)(HAKU_MAP_SHARED | HAKU_MAP_PRIVATE);
    mmap_insert_region_prot(task, base, rounded, prot, map_flags);
    return base;
}

/* mprotect(2): change the protection of a mapped range. The range must
 * lie inside one tracked region. Returns 0 or -errno. */
uint32_t scheduler_mprotect(uint32_t address, uint32_t length, uint32_t prot) {
    task_t *task;
    address_space_t *space;
    uint32_t start;
    uint32_t end;
    uint32_t index;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    if (length == 0 || address > 0xFFFFFFFFu - length) return 0u - HAKU_EINVAL;
    if (prot & ~(uint32_t)(HAKU_PROT_READ | HAKU_PROT_WRITE | HAKU_PROT_EXEC))
        return 0u - HAKU_EINVAL;
    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0u - HAKU_EINVAL;

    start = address & 0xFFFFF000u;
    end = address + length;
    if ((end & 0xFFFu) != 0) {
        if (end > 0xFFFFF000u) return 0u - HAKU_EINVAL;
        end = (end + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    }
    if (end <= start || start < USER_MMAP_BASE || end > USER_MMAP_LIMIT)
        return 0u - HAKU_EINVAL;

    index = mmap_find_region(task, start, end);
    /* Linux reports an unmapped range as ENOMEM ("addresses in the range
     * are not valid"), and libcs test for it, so Haku matches. */
    if (index == TASK_MAX_MMAP_REGIONS) return 0u - HAKU_ENOMEM;
    if (!paging_protect_user_range(space, start, end - start,
                                   prot != (uint32_t)HAKU_PROT_NONE,
                                   (prot & HAKU_PROT_WRITE) != 0u))
        return 0u - HAKU_ENOMEM;
    task->mmap_regions[index].prot = prot;
    return 0;
}

/* Copy the contents of a mapped range into freshly mapped pages (the
 * mremap move path). The frames are read and written through the direct
 * map, so the user protection of the source does not matter. */
static int mmap_copy_range(address_space_t *space, uint32_t from,
                            uint32_t to, uint32_t length) {
    for (uint32_t offset = 0; offset < length; offset += USER_PAGE_SIZE) {
        uint32_t source = paging_user_page_physical(space, from + offset);
        uint32_t target = paging_user_page_physical(space, to + offset);
        if (!source || !target) return 0;
        memcpy(PHYS_TO_VIRT(target), PHYS_TO_VIRT(source), USER_PAGE_SIZE);
    }
    return 1;
}

/* mremap(2): grow, shrink, or move a mapping. Without MREMAP_MAYMOVE a
 * mapping may only grow in place; with it the mapping moves to a free
 * range and its contents are preserved. MREMAP_FIXED places it at the
 * given address. Returns the new base or -errno. */
uint32_t scheduler_mremap(uint32_t old_address, uint32_t old_length,
                          uint32_t new_length, uint32_t flags,
                          uint32_t new_address) {
    task_t *task;
    address_space_t *space;
    uint32_t index;
    uint32_t base;
    uint32_t size;
    uint32_t prot;
    uint32_t map_flags;
    uint32_t old_rounded;
    uint32_t new_rounded;
    uint32_t target;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    if (old_length == 0 || new_length == 0) return 0u - HAKU_EINVAL;
    if (old_address > 0xFFFFFFFFu - old_length) return 0u - HAKU_EINVAL;
    if (flags & ~(uint32_t)(HAKU_MREMAP_MAYMOVE | HAKU_MREMAP_FIXED))
        return 0u - HAKU_EINVAL;
    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0u - HAKU_EINVAL;

    old_rounded = (old_length + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    new_rounded = (new_length + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    if (new_rounded > USER_MMAP_LIMIT - USER_MMAP_BASE) return 0u - HAKU_ENOMEM;

    index = mmap_find_region(task, old_address & 0xFFFFF000u,
                              (old_address & 0xFFFFF000u) + old_rounded);
    if (index == TASK_MAX_MMAP_REGIONS) return 0u - HAKU_EINVAL;
    base = task->mmap_regions[index].base;
    size = task->mmap_regions[index].size;
    prot = task->mmap_regions[index].prot;
    map_flags = task->mmap_regions[index].flags;

    if (new_rounded <= size) {
        /* Shrink (or keep the size): release the tail and keep the base. */
        for (uint32_t page = base + new_rounded; page < base + size;
             page += USER_PAGE_SIZE)
            paging_unmap_user_page(space, page);
        task->mmap_regions[index].size = new_rounded;
        return base;
    }

    /* Growth: in place when the pages above the region are free. */
    if (base + new_rounded <= USER_MMAP_LIMIT &&
        mmap_range_free(task, base + size, base + new_rounded) &&
        map_user_zero_pages(space, base + size, base + new_rounded, 1)) {
        if (!paging_protect_user_range(space, base + size,
                                       new_rounded - size,
                                       prot != (uint32_t)HAKU_PROT_NONE,
                                       (prot & HAKU_PROT_WRITE) != 0u)) {
            for (uint32_t page = base + size; page < base + new_rounded;
                 page += USER_PAGE_SIZE)
                paging_unmap_user_page(space, page);
            return 0u - HAKU_ENOMEM;
        }
        task->mmap_regions[index].size = new_rounded;
        return base;
    }

    if (!(flags & HAKU_MREMAP_MAYMOVE) && !(flags & HAKU_MREMAP_FIXED))
        return 0u - HAKU_EINVAL;
    if (flags & HAKU_MREMAP_FIXED) {
        if ((new_address & 0xFFFu) != 0u) return 0u - HAKU_EINVAL;
        if (new_address < USER_MMAP_BASE ||
            new_address > USER_MMAP_LIMIT - new_rounded)
            return 0u - HAKU_EINVAL;
        target = new_address;
    } else {
        target = mmap_find_gap(task, new_rounded);
        if (!target) return 0u - HAKU_ENOMEM;
    }
    if (!mmap_range_free(task, target, target + new_rounded))
        return 0u - HAKU_EINVAL;

    /* Move: map the new pages, copy the contents, then release the old
     * ones. The old mapping stays valid until the copy is complete. */
    if (!map_user_zero_pages(space, target, target + new_rounded, 1))
        return 0u - HAKU_ENOMEM;
    if (!mmap_copy_range(space, base, target, size)) {
        for (uint32_t page = target; page < target + new_rounded;
             page += USER_PAGE_SIZE)
            paging_unmap_user_page(space, page);
        return 0u - HAKU_ENOMEM;
    }
    if (!paging_protect_user_range(space, target, new_rounded,
                                   prot != (uint32_t)HAKU_PROT_NONE,
                                   (prot & HAKU_PROT_WRITE) != 0u)) {
        for (uint32_t page = target; page < target + new_rounded;
             page += USER_PAGE_SIZE)
            paging_unmap_user_page(space, page);
        return 0u - HAKU_ENOMEM;
    }
    for (uint32_t page = base; page < base + size; page += USER_PAGE_SIZE)
        paging_unmap_user_page(space, page);
    /* The moved region replaces the old one in the table. */
    task->mmap_regions[index].base = target;
    task->mmap_regions[index].size = new_rounded;
    /* Keep the table sorted by base. */
    for (uint32_t i = index; i > 0 && task->mmap_regions[i].base <
                                   task->mmap_regions[i - 1u].base; i--) {
        uint32_t swap = task->mmap_regions[i].base;
        task->mmap_regions[i].base = task->mmap_regions[i - 1u].base;
        task->mmap_regions[i - 1u].base = swap;
        swap = task->mmap_regions[i].size;
        task->mmap_regions[i].size = task->mmap_regions[i - 1u].size;
        task->mmap_regions[i - 1u].size = swap;
        swap = task->mmap_regions[i].prot;
        task->mmap_regions[i].prot = task->mmap_regions[i - 1u].prot;
        task->mmap_regions[i - 1u].prot = swap;
        swap = task->mmap_regions[i].flags;
        task->mmap_regions[i].flags = task->mmap_regions[i - 1u].flags;
        task->mmap_regions[i - 1u].flags = swap;
    }
    return target;
}

/* madvise(2): MADV_DONTNEED releases the pages of a range, which come
 * back zero-filled on the next access (an anonymous mapping has no file
 * behind it). The other advice values are hints and change nothing.
 * Returns 0 or -errno. */
uint32_t scheduler_madvise(uint32_t address, uint32_t length, uint32_t advice) {
    task_t *task;
    address_space_t *space;
    uint32_t start;
    uint32_t end;
    uint32_t index;
    uint32_t prot;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    if (length == 0 || address > 0xFFFFFFFFu - length) return 0u - HAKU_EINVAL;
    if (advice > HAKU_MADV_FREE) return 0u - HAKU_EINVAL;
    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0u - HAKU_EINVAL;

    start = address & 0xFFFFF000u;
    end = address + length;
    if ((end & 0xFFFu) != 0) {
        if (end > 0xFFFFF000u) return 0u - HAKU_EINVAL;
        end = (end + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    }
    if (end <= start || start < USER_MMAP_BASE || end > USER_MMAP_LIMIT)
        return 0u - HAKU_EINVAL;
    index = mmap_find_region(task, start, end);
    if (index == TASK_MAX_MMAP_REGIONS) return 0u - HAKU_EINVAL;
    if (advice != HAKU_MADV_DONTNEED) return 0;

    prot = task->mmap_regions[index].prot;
    for (uint32_t page = start; page < end; page += USER_PAGE_SIZE) {
        void *fresh = frame_alloc();
        uint32_t physical;
        if (!fresh) break;
        physical = (uint32_t)fresh;
        memset(PHYS_TO_VIRT(physical), 0, USER_PAGE_SIZE);
        paging_unmap_user_page(space, page);
        if (!paging_map_user_page(space, page, physical,
                                  (prot & HAKU_PROT_WRITE) != 0u)) {
            frame_free(fresh);
            break;
        }
        paging_protect_user_range(space, page, USER_PAGE_SIZE,
                                  prot != (uint32_t)HAKU_PROT_NONE,
                                  (prot & HAKU_PROT_WRITE) != 0u);
    }
    return 0;
}

/* set_thread_area(2): the user FS base is the thread pointer. The GDT
 * entry is updated so the next iret into user mode loads the segment
 * with the new base. */
uint32_t scheduler_set_thread_area(uint32_t base) {
    task_t *task;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    task = &tasks[current_task];
    task->fs_base = base;
    gdt_set_user_fs_base(base);
    return 0;
}

uint32_t scheduler_get_thread_area(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    return tasks[current_task].fs_base;
}

/* Limited mmap(2): anonymous private pages from the per-process mmap
 * region, always zero-filled; PROT_WRITE decides whether the PTEs carry
 * R/W. Returns the mapped base address or -errno. */
uint32_t scheduler_mmap(uint32_t length, int writable) {
    uint32_t prot = HAKU_PROT_READ;
    if (writable) prot |= HAKU_PROT_WRITE;
    return scheduler_mmap2(0u, length, prot,
                           HAKU_MAP_PRIVATE | HAKU_MAP_ANONYMOUS);
}

/* munmap(2) for anonymous mappings: the range must lie inside exactly one
 * tracked region. Partial unmap trims the region or splits it, and the
 * released pages (plus empty page tables) return to the frame allocator.
 * Returns 0 or -errno. */
uint32_t scheduler_munmap(uint32_t address, uint32_t length) {
    task_t *task;
    address_space_t *space;
    uint32_t start;
    uint32_t end;
    uint32_t index = TASK_MAX_MMAP_REGIONS;
    uint32_t base = 0;
    uint32_t size = 0;
    int split;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    if (length == 0 || address > 0xFFFFFFFFu - length)
        return 0u - HAKU_EINVAL;
    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0u - HAKU_EINVAL;

    start = address & 0xFFFFF000u;
    end = address + length;
    if ((end & 0xFFFu) != 0) {
        if (end > 0xFFFFF000u) return 0u - HAKU_EINVAL;
        end = (end + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    }
    if (end <= start || start < USER_MMAP_BASE || end > USER_MMAP_LIMIT)
        return 0u - HAKU_EINVAL;

    for (uint32_t i = 0; i < task->mmap_region_count; i++) {
        base = task->mmap_regions[i].base;
        size = task->mmap_regions[i].size;
        if (start >= base && end <= base + size) {
            index = i;
            break;
        }
    }
    if (index == TASK_MAX_MMAP_REGIONS) return 0u - HAKU_EINVAL;

    /* A middle cut becomes two regions: check for a free slot before any
     * page is released so a failure cannot leave the table inconsistent. */
    split = start > base && end < base + size;
    if (split && task->mmap_region_count >= TASK_MAX_MMAP_REGIONS)
        return 0u - HAKU_EINVAL;

    for (uint32_t page = start; page < end; page += USER_PAGE_SIZE)
        paging_unmap_user_page(space, page);

    if (start == base && end == base + size) {
        for (uint32_t i = index; i + 1u < task->mmap_region_count; i++)
            task->mmap_regions[i] = task->mmap_regions[i + 1u];
        task->mmap_region_count--;
    } else if (start == base) {
        task->mmap_regions[index].base = end;
        task->mmap_regions[index].size = base + size - end;
    } else if (end == base + size) {
        task->mmap_regions[index].size = start - base;
    } else {
        uint32_t old_end = base + size;
        task->mmap_regions[index].size = start - base;
        mmap_insert_region_prot(task, end, old_end - end,
                                task->mmap_regions[index].prot,
                                task->mmap_regions[index].flags);
    }
    return 0;
}
