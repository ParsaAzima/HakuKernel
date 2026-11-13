/* Haku x86-64 tasks and the preemptive round-robin scheduler.
 *
 * The 64-bit interrupt stubs already finish by returning a regs64_t frame
 * to IRETQ (see isr64.asm and the comment in isr64_dispatch), so a context
 * switch is not a second mechanism: it is the dispatcher returning a frame
 * that lives on *another* task's kernel stack. The stub moves RSP to that
 * frame, pops the general registers and IRETQs, and the CPU is in the next
 * task.
 *
 * That choice makes ring-3 entry fall out of the same path. A user task is
 * a frame whose RIP is the user entry point, CS is 0x1B and SS is 0x23;
 * IRETQ performs the privilege change and loads RSP from the frame. When
 * the task traps back into the kernel, the CPU loads RSP0 from the TSS, so
 * every switch also repoints RSP0 at the incoming task's kernel stack.
 *
 * Task 0 is the boot context: it never runs in ring 3, and its saved frame
 * is whatever the timer interrupted. Every other task starts on a private
 * 16 KiB stack and a frame built on top of it. Tasks run until the timer
 * preempts them; nothing blocks yet, so the scheduler never has to pick a
 * task that cannot run.
 *
 * SSE2 is part of the x86-64 ABI, so the compiler may keep live state in
 * XMM registers. sched64_init enables the FPU (CR4.OSFXSR) and every switch
 * FXSAVEs the outgoing task and FXRSTORs the incoming one.
 */
#include "haku64.h"

typedef struct {
    u32 state;
    u32 id;
    int parent;     /* task index that may reap this task, -1 for none */
    u32 tgid;       /* task index of the thread-group leader (getpid) */
    u32 cpu;        /* CPU this task is pinned to; never changes */
    u64 space;      /* PML4 physical address this task runs under */
    u64 stack_base; /* kernel heap block the 16 KiB stack was carved from */
    u64 stack_top;  /* kernel stack, i.e. TSS.RSP0 while the task runs */
    regs64_t *frame;
    u64 exit_status; /* exit/exit_group argument, readable after DONE */
    u32 term_signal; /* non-zero when a signal (or a fault) ended the task */
    u64 fault_vector; /* non-zero when the task stopped on a ring-3 fault */
    u64 fault_address;
    u64 mmap_next;   /* per-task bump pointer for anonymous mappings */
    u64 brk;         /* program break; 0 until the first brk() call */
    u64 futex_addr;  /* user address a K64_TASK_WAIT task is parked on */
    char cwd[K64_FS_PATH_MAX + 1]; /* working directory, canonical and absolute */
    char comm[16];   /* prctl(PR_SET_NAME) name, NUL-terminated */
} task64_t;

static task64_t tasks[K64_TASK_MAX];
static u32 enabled;

/* One lock for the task table. The slots, the mapping records and the
 * address-space reference counts are read by every CPU (a kill from the
 * bootstrap processor touches a task that an AP is running), so the state
 * transitions are serialised even though a switch itself needs no lock: once
 * switch_to() has updated the per-CPU `current` and the frame pointer, the
 * outgoing CPU is done with the table until it is interrupted again. */
static spinlock64_t sched_lock = SPINLOCK64_INIT;

/* The CPU this code runs on and the task slot it is running. The task table
 * is indexed by slot, so "current" is per CPU and no longer a single global:
 * two CPUs run two different tasks at the same time. */
static k64_percpu_t *this_cpu(void) {
    return k64_cpu();
}

static int cur_task(void) {
    k64_percpu_t *cpu = this_cpu();
    return cpu ? (int)cpu->current : 0;
}

/* Threads share one address space, so the PML4, the mapping records and the
 * anonymous bump pointer are owned by one task slot and every thread points
 * at it. `space_refs[owner]` counts the live users: the space and its frames
 * are released only when the last one is reaped, and the records move to a
 * surviving thread if the owner dies first. A task whose space_owner is
 * itself is an ordinary process. */
static int space_owner[K64_TASK_MAX];
static u32 space_refs[K64_TASK_MAX];

/* ------------------------------------------------------- mapping records
 * munmap frees the frames of a mapping it created, but a MAP_SHARED file
 * mapping aliases the file's own frames: freeing those would empty the file.
 * Each task keeps a small table of ranges and whether the frames are owned,
 * and munmap consults it page by page. A range that is not in the table is
 * treated as owned, which keeps anonymous mappings freed as before.
 *
 * `shared` is a separate flag because fork needs it: a shared range is
 * aliased into the child (the frames are the same memory), while every other
 * range is copied. It is also what tells task exit to leave a file cache
 * page alone when the mapping only borrowed it. */
#define K64_TASK_MAPS 8u

typedef struct {
    u64 base;
    u64 pages;
    u32 owned;
    u32 shared;
    u32 used;
} map64_t;

static map64_t mappings[K64_TASK_MAX][K64_TASK_MAPS];

/* One FXSAVE area per task. The area is 512 bytes and must be 16-byte
 * aligned, which a __declspec(align(16)) array guarantees. */
static __declspec(align(16)) u8 fxsave_areas[K64_TASK_MAX][512];
static __declspec(align(16)) u8 fxsave_default[512];

static int valid_id(int id) {
    return id >= 0 && id < (int)K64_TASK_MAX;
}

/* The slot that owns the address space and mapping records a task uses.
 * Threads share one, so every space/mapping lookup goes through here. */
static int as_owner(int id) {
    if (!valid_id(id) || !valid_id(space_owner[id])) return id;
    return space_owner[id];
}

static int find_free(void) {
    for (int i = 1; i < (int)K64_TASK_MAX; i++)
        if (tasks[i].state == K64_TASK_UNUSED) return i;
    return -1;
}

/* PR_SET_NAME/PR_GET_NAME storage: at most 15 bytes plus the terminator,
 * like Linux's TASK_COMM_LEN. */
static void comm_set(int id, const char *name) {
    u32 i = 0;

    while (name && name[i] && i < sizeof(tasks[id].comm) - 1u) {
        tasks[id].comm[i] = name[i];
        i++;
    }
    tasks[id].comm[i] = 0;
}

/* The frame is placed so that the IRETQ path leaves RSP 8 mod 16 at the
 * entry point, the same alignment a CALL would produce: the compiler is
 * allowed to assume it. A frame is 176 bytes, so `top - 8 - 176` aligned
 * down and then offset by 8 lands entirely below the stack top. */
static regs64_t *frame_on_stack(u64 stack_top, u64 rip) {
    u64 base = ((stack_top - 8 - sizeof(regs64_t)) & ~15ull) + 8;
    regs64_t *frame = (regs64_t *)base;

    memset(frame, 0, sizeof(*frame));
    frame->rip = rip;
    frame->cs = K64_KERNEL_CS;
    frame->ss = K64_KERNEL_DS;
    /* In long mode a same-privilege IRETQ does not consume these two
     * slots, and the RSP it leaves is exactly frame + 15 registers + the
     * vector/error words; setting the field to the same value keeps the
     * frame sane either way. */
    frame->rsp = base + 160;
    frame->rflags = 0x202;          /* IF, plus the reserved bit 1 */
    return frame;
}

static void prepare_sse(int id) {
    memcpy(fxsave_areas[id], fxsave_default, sizeof(fxsave_default));
}

/* Reset the task table and start scheduling on the bootstrap processor.
 *
 * This wipes every slot, so it must run before smp64_release_aps() hands the
 * application processors their idle tasks. The boot order guarantees that:
 * the single-CPU scheduler self-test calls it, the SMP test releases the APs
 * afterwards. The per-CPU fields it owns are CPU 0's, not a global cursor. */
void sched64_init(void) {
    k64_percpu_t *cpu = this_cpu();
    u64 flags;

    flags = spin64_lock(&sched_lock);

    memset(tasks, 0, sizeof(tasks));
    memset(mappings, 0, sizeof(mappings));

    for (u32 i = 0; i < K64_TASK_MAX; i++) {
        space_owner[i] = (int)i;
        space_refs[i] = 0;
    }

    enable_sse64();
    fxsave64(fxsave_default);

    tasks[0].id = 0;
    tasks[0].state = K64_TASK_RUNNING;
    tasks[0].parent = -1;
    tasks[0].tgid = 0;
    tasks[0].cpu = 0;
    tasks[0].space = read_cr3_64() & K64_PAGE_MASK;
    tasks[0].stack_base = 0;
    tasks[0].stack_top = 0;        /* ring 0 only, so RSP0 is not used */
    tasks[0].frame = 0;
    tasks[0].exit_status = 0;
    tasks[0].term_signal = 0;
    tasks[0].fault_vector = 0;
    tasks[0].fault_address = 0;
    tasks[0].mmap_next = 0;
    tasks[0].futex_addr = 0;
    tasks[0].brk = 0;
    tasks[0].cwd[0] = '/';
    tasks[0].cwd[1] = 0;
    comm_set(0, "kernel");
    space_refs[0] = 1;
    memcpy(fxsave_areas[0], fxsave_default, sizeof(fxsave_default));

    if (cpu) {
        cpu->current = 0;
        cpu->idle = 0;                 /* task 0 doubles as the BSP idle */
        cpu->cursor = 0;
        cpu->switches = 0;
        cpu->ticks = 0;
        cpu->active_space = tasks[0].space;
    }

    enabled = 1;
    spin64_unlock(&sched_lock, flags);
}

/* Create a kernel task on `cpu`. The whole record is filled before the state
 * becomes READY, so a scheduler tick that sees the slot ready also sees a
 * complete task; the lock covers that publication. */
int task64_create_kernel_on(u32 cpu_affinity, void (*entry)(void)) {
    int id;
    u8 *stack;
    u64 top;
    u64 flags;

    if (!enabled || cpu_affinity >= K64_SMP_MAX_CPUS) return -1;
    if (!cpus64[cpu_affinity].online) return -1;

    flags = spin64_lock(&sched_lock);
    id = find_free();
    if (id < 0) {
        spin64_unlock(&sched_lock, flags);
        return -1;
    }

    stack = (u8 *)kmalloc64(K64_TASK_STACK_SIZE);
    if (!stack) {
        spin64_unlock(&sched_lock, flags);
        return -1;
    }
    top = ((u64)stack + K64_TASK_STACK_SIZE) & ~15ull;

    tasks[id].id = (u32)id;
    tasks[id].parent = -1;
    tasks[id].tgid = (u32)id;
    tasks[id].cpu = cpu_affinity;
    tasks[id].space = read_cr3_64() & K64_PAGE_MASK;
    tasks[id].stack_base = (u64)stack;
    tasks[id].stack_top = top;
    tasks[id].frame = frame_on_stack(top, (u64)entry);
    tasks[id].exit_status = 0;
    tasks[id].term_signal = 0;
    tasks[id].fault_vector = 0;
    tasks[id].fault_address = 0;
    tasks[id].mmap_next = 0;
    tasks[id].futex_addr = 0;
    tasks[id].brk = 0;
    tasks[id].cwd[0] = '/';
    tasks[id].cwd[1] = 0;
    comm_set(id, "kthread");
    space_owner[id] = id;
    space_refs[id] = 1;
    prepare_sse(id);
    tasks[id].state = K64_TASK_READY;
    spin64_unlock(&sched_lock, flags);

    /* A remote CPU has no reason to look at the table until it is told to. */
    if (cpu_affinity != k64_cpu_index()) smp64_resched_cpu(cpu_affinity);
    return id;
}

int task64_create_kernel(void (*entry)(void)) {
    return task64_create_kernel_on(k64_cpu_index(), entry);
}

int task64_create_user(u64 entry, u64 user_stack_top, u64 space) {
    int id;
    u8 *stack;
    u64 top;
    regs64_t *frame;
    u64 flags;

    if (!enabled || !space) return -1;

    flags = spin64_lock(&sched_lock);
    id = find_free();
    if (id < 0) {
        spin64_unlock(&sched_lock, flags);
        return -1;
    }

    stack = (u8 *)kmalloc64(K64_TASK_STACK_SIZE);
    if (!stack) {
        spin64_unlock(&sched_lock, flags);
        return -1;
    }
    top = ((u64)stack + K64_TASK_STACK_SIZE) & ~15ull;

    frame = frame_on_stack(top, entry);
    /* Same frame, but IRETQ now changes privilege: CS/SS carry RPL 3 and
     * RSP is the user stack, which is what makes the CPU switch stacks and
     * load RSP0 from the TSS on the way back in. */
    frame->cs = K64_USER_CS | 3u;
    frame->ss = K64_USER_DS | 3u;
    frame->rsp = user_stack_top;
    frame->rflags = 0x202;

    tasks[id].id = (u32)id;
    tasks[id].parent = -1;
    tasks[id].tgid = (u32)id;
    /* User tasks stay on the CPU that created them. The syscall layer's
     * global state (the descriptor table, the filesystem) is not locked yet,
     * so letting a user process float between CPUs would race it; pinning is
     * the honest alternative to pretending it is safe. */
    tasks[id].cpu = k64_cpu_index();
    tasks[id].space = space;
    tasks[id].stack_base = (u64)stack;
    tasks[id].stack_top = top;
    tasks[id].frame = frame;
    tasks[id].exit_status = 0;
    tasks[id].term_signal = 0;
    tasks[id].fault_vector = 0;
    tasks[id].fault_address = 0;
    tasks[id].mmap_next = K64_USER_MMAP_BASE;
    tasks[id].futex_addr = 0;
    tasks[id].brk = 0;
    tasks[id].cwd[0] = '/';
    tasks[id].cwd[1] = 0;
    comm_set(id, "user");
    space_owner[id] = id;
    space_refs[id] = 1;
    prepare_sse(id);
    tasks[id].state = K64_TASK_READY;
    spin64_unlock(&sched_lock, flags);
    return id;
}

/* Round-robin on one CPU. The cursor is per CPU, so two CPUs scanning the
 * shared table do not have to agree on where the last dispatch ended, and the
 * affinity check keeps a CPU from running a task pinned elsewhere. The scan
 * is bounded by K64_TASK_MAX (8) and runs with the table lock held. */
static int pick_ready(u32 cpu_index) {
    k64_percpu_t *cpu = &cpus64[cpu_index];
    int start = (int)cpu->cursor;

    if (start < 0 || start >= (int)K64_TASK_MAX) start = 0;
    for (u32 step = 1; step <= K64_TASK_MAX; step++) {
        int idx = (start + (int)step) % (int)K64_TASK_MAX;

        if (tasks[idx].state != K64_TASK_READY) continue;
        if (tasks[idx].cpu != cpu_index) continue;
        cpu->cursor = (u32)idx;
        return idx;
    }
    return -1;
}

/* The task this CPU should run when the current one cannot continue: another
 * ready task, or the CPU's idle task. -1 means "park". */
static int pick_or_idle(u32 cpu_index) {
    int next = pick_ready(cpu_index);

    if (next >= 0) return next;
    if (cpus64[cpu_index].idle == (u32)-1) return -1;
    return (int)cpus64[cpu_index].idle;
}

static regs64_t *switch_to(int next, int save_current) {
    k64_percpu_t *cpu = this_cpu();
    int old = (int)cpu->current;

    if (save_current && old >= 0 && tasks[old].state == K64_TASK_RUNNING)
        fxsave64(fxsave_areas[old]);

    if (old >= 0 && tasks[old].state == K64_TASK_RUNNING)
        tasks[old].state = K64_TASK_READY;
    tasks[next].state = K64_TASK_RUNNING;
    cpu->current = (u32)next;
    cpu->switches++;

    /* The incoming area belongs to the kernel heap, which is identity
     * mapped in every space, so this is valid before the CR3 write too. */
    fxrstor64(fxsave_areas[next]);
    if (tasks[next].space) {
        write_cr3_64(tasks[next].space);
        cpu->active_space = tasks[next].space;
    }
    /* Two stacks matter: the TSS one is what a ring-3 trap switches to and
     * the per-CPU copy is what SYSCALL loads before it has a stack. Both must
     * describe the task this CPU is about to run. Task 0 has no kernel stack
     * of its own, so it keeps whatever the previous task left. */
    if (tasks[next].stack_top) {
        gdt64_set_rsp0_cpu(cpu->index, tasks[next].stack_top);
        cpu->rsp0 = tasks[next].stack_top;
    }
    return tasks[next].frame;
}

/* The timer (or a reschedule IPI) interrupted this CPU. Save the frame and
 * hand back the one to resume, which may belong to another task. */
regs64_t *sched64_tick(regs64_t *frame) {
    k64_percpu_t *cpu = this_cpu();
    int me, next;
    regs64_t *result;
    u64 flags;

    if (!enabled || !cpu) return frame;
    me = (int)cpu->current;
    if (me < 0) return frame;          /* CPU is not scheduling yet */

    flags = spin64_lock(&sched_lock);
    cpu->ticks++;
    if (tasks[me].state == K64_TASK_RUNNING) tasks[me].frame = frame;

    next = pick_ready(cpu->index);
    if (next < 0) {
        spin64_unlock(&sched_lock, flags);
        return frame;                  /* nothing else to run: stay here */
    }
    result = switch_to(next, 1);
    /* Unlock on the outgoing stack: the frame returned above is popped by
     * the assembly after this function has already returned. */
    spin64_unlock(&sched_lock, flags);
    return result;
}

/* sched_yield(): give another READY task the CPU and resume at the caller's
 * syscall return. With no other runnable task the caller simply continues,
 * which is what POSIX allows. */
regs64_t *sched64_yield(regs64_t *frame) {
    k64_percpu_t *cpu = this_cpu();
    int me, next;
    regs64_t *result;
    u64 flags;

    if (!enabled || !cpu) return frame;
    me = (int)cpu->current;
    if (me < 0) return frame;

    flags = spin64_lock(&sched_lock);
    if (tasks[me].state == K64_TASK_RUNNING) tasks[me].frame = frame;
    next = pick_ready(cpu->index);
    if (next < 0) {
        spin64_unlock(&sched_lock, flags);
        return frame;
    }
    result = switch_to(next, 1);
    spin64_unlock(&sched_lock, flags);
    return result;
}

/* Switch away from a task that can no longer run (exited, blocked, faulted or
 * signalled). The idle task is the floor: with nothing ready and no idle task
 * this CPU has nothing to return to, which is reported rather than silently
 * resuming a dead frame. Runs with the task lock, which it takes itself
 * because the callers release it after publishing their state change. */
static regs64_t *switch_away(const char *halt_message) {
    k64_percpu_t *cpu = this_cpu();
    int next;
    regs64_t *result;
    u64 flags;

    flags = spin64_lock(&sched_lock);
    next = pick_or_idle(cpu->index);
    if (next < 0 || next == (int)cpu->current) {
        spin64_unlock(&sched_lock, flags);
        k64_puts(halt_message);
        cli64();
        for (;;) cpu_halt64();
    }
    result = switch_to(next, 0);
    spin64_unlock(&sched_lock, flags);
    return result;
}

/* Called from the system call handler for SYS_EXIT. The exit frame is not
 * saved because the task never resumes; a process that wants to continue
 * after a syscall just returns through the normal path. */
regs64_t *sched64_exit(regs64_t *frame) {
    k64_percpu_t *cpu = this_cpu();
    int me;
    u64 flags;

    if (!enabled || !cpu || (int)cpu->current < 0) return frame;
    me = (int)cpu->current;

    flags = spin64_lock(&sched_lock);
    tasks[me].state = K64_TASK_DONE;
    tasks[me].term_signal = 0;
    /* exit()/exit_group() pass the status in RDI; the frame is not saved
     * because the task never resumes, but the value is what a parent would
     * read back. */
    tasks[me].exit_status = frame->rdi;
    spin64_unlock(&sched_lock, flags);

    return switch_away("HAKU64-TASK-HALT: no runnable task after exit\r\n");
}

/* A ring-3 exception ends the task: there is no signal model on the 64-bit
 * port, so the closest honest behaviour is a terminated process. The fault
 * reason is kept for the boot test (and a future parent). */
regs64_t *sched64_fault(regs64_t *frame, u32 vector, u64 address) {
    k64_percpu_t *cpu = this_cpu();
    int me;
    u64 flags;

    if (!enabled || !cpu || (int)cpu->current < 0) return frame;
    me = (int)cpu->current;

    flags = spin64_lock(&sched_lock);
    tasks[me].state = K64_TASK_DONE;
    tasks[me].term_signal = K64_SIGSEGV;
    tasks[me].fault_vector = vector;
    tasks[me].fault_address = address;
    spin64_unlock(&sched_lock, flags);

    return switch_away("HAKU64-TASK-HALT: no runnable task after a user"
                       " fault\r\n");
}

/* End the current task because a signal has no handler on this port. The
 * exit status is not used; wait4 reports the signal instead. */
regs64_t *sched64_terminate(regs64_t *frame, u32 signal) {
    k64_percpu_t *cpu = this_cpu();
    int me;
    u64 flags;

    if (!enabled || !cpu || (int)cpu->current < 0) return frame;
    me = (int)cpu->current;

    flags = spin64_lock(&sched_lock);
    tasks[me].state = K64_TASK_DONE;
    tasks[me].term_signal = signal;
    spin64_unlock(&sched_lock, flags);

    return switch_away("HAKU64-TASK-HALT: no runnable task after a"
                       " signal\r\n");
}

/* Mark a task finished from outside (the boot self-test does this to the
 * CPU-burner tasks once they have proven they ran). The current task is
 * never killed this way, because it would then have no frame to return
 * from. */
void task64_kill(int id) {
    u32 owner;
    u64 flags;

    if (!valid_id(id) || id == cur_task()) return;
    flags = spin64_lock(&sched_lock);
    tasks[id].state = K64_TASK_DONE;
    owner = tasks[id].cpu;
    spin64_unlock(&sched_lock, flags);
    /* If another CPU is running it, the state change alone will not make it
     * switch: nudge it. */
    smp64_resched_cpu(owner);
}

int task64_state(int id) {
    return valid_id(id) ? (int)tasks[id].state : (int)K64_TASK_UNUSED;
}

u64 task64_kernel_stack(int id) {
    return valid_id(id) ? tasks[id].stack_top : 0;
}

u64 sched64_space(int id) {
    return valid_id(id) ? tasks[as_owner(id)].space : 0;
}

int sched64_tgid(int id) {
    return valid_id(id) ? (int)tasks[id].tgid : -1;
}

u64 task64_exit_status(int id) {
    return valid_id(id) ? tasks[id].exit_status : 0;
}

u64 task64_fault_vector(int id) {
    return valid_id(id) ? tasks[id].fault_vector : 0;
}

u64 task64_fault_address(int id) {
    return valid_id(id) ? tasks[id].fault_address : 0;
}

u64 task64_mmap_base(int id) {
    return valid_id(id) ? tasks[as_owner(id)].mmap_next : 0;
}

/* The program break lives with the address space, like the bump pointer:
 * a thread sees the break its process set. */
u64 task64_get_brk(int id) {
    return valid_id(id) ? tasks[as_owner(id)].brk : 0;
}

void task64_set_brk(int id, u64 value) {
    if (valid_id(id)) tasks[as_owner(id)].brk = value;
}

/* Working-directory accessors. The path is stored canonical and absolute
 * ("/" at the volume root), so getcwd only has to copy it and a relative
 * path is the stored prefix plus the user's name. Threads resolve through
 * the space owner (CLONE_FS semantics): a chdir in one thread moves the
 * whole group, which is what a process with threads expects. */
int task64_get_cwd(int id, char *out, u64 size) {
    int owner;
    u64 length = 0;

    if (!valid_id(id)) return -3;                 /* ESRCH */
    owner = as_owner(id);
    while (tasks[owner].cwd[length]) length++;
    if (size < length + 1) return -34;            /* ERANGE */
    for (u64 i = 0; i <= length; i++) out[i] = tasks[owner].cwd[i];
    return 0;
}

int task64_set_cwd(int id, const char *path) {
    int owner;
    u64 length = 0;

    if (!valid_id(id) || !path || path[0] != '/') return -22;   /* EINVAL */
    while (path[length]) {
        if (length >= (u64)K64_FS_PATH_MAX) return -36;         /* ENAMETOOLONG */
        length++;
    }
    owner = as_owner(id);
    for (u64 i = 0; i <= length; i++) tasks[owner].cwd[i] = path[i];
    return 0;
}

int task64_set_name(int id, const char *name) {
    if (!valid_id(id) || !name) return -22;                     /* EINVAL */
    comm_set(id, name);
    return 0;
}

void task64_get_name(int id, char out[16]) {
    int i = 0;

    if (!valid_id(id)) {
        out[0] = 0;
        return;
    }
    while (i < 15 && tasks[id].comm[i]) {
        out[i] = tasks[id].comm[i];
        i++;
    }
    out[i] = 0;
}

/* Reserve `pages` at the task's bump pointer. The reservation is recorded
 * before the caller maps anything, so a failed mapping does not have to
 * rewind the pointer: the address range is simply never reused. Threads
 * share the pointer through the space owner, so two threads never reserve
 * the same range. */
u64 task64_mmap_reserve(int id, u64 pages) {
    u64 base, next;

    if (!valid_id(id) || pages == 0 || pages > K64_USER_MMAP_PAGES_MAX)
        return 0;
    id = as_owner(id);
    base = tasks[id].mmap_next;
    if (base < K64_USER_MMAP_BASE) base = K64_USER_MMAP_BASE;
    next = base + pages * K64_PAGE_SIZE;
    if (next < base || next > K64_USER_STACK_BOTTOM) return 0;
    tasks[id].mmap_next = next;
    return base;
}

int task64_map_record(int id, u64 base, u64 pages, int owned, int shared) {
    if (!valid_id(id) || pages == 0) return 0;
    id = as_owner(id);
    for (u32 i = 0; i < K64_TASK_MAPS; i++) {
        map64_t *map = &mappings[id][i];

        if (map->used) continue;
        map->base = base;
        map->pages = pages;
        map->owned = owned ? 1u : 0u;
        map->shared = shared ? 1u : 0u;
        map->used = 1;
        return 1;
    }
    return 0;
}

/* 1 when `address` falls in a range another space also maps. */
int task64_map_shared(int id, u64 address) {
    if (!valid_id(id)) return 0;
    id = as_owner(id);
    for (u32 i = 0; i < K64_TASK_MAPS; i++) {
        map64_t *map = &mappings[id][i];
        u64 end;

        if (!map->used) continue;
        end = map->base + map->pages * K64_PAGE_SIZE;
        if (address >= map->base && address < end) return map->shared != 0;
    }
    return 0;
}

/* 1 when munmap may free the frame behind `address`. */
int task64_map_owned(int id, u64 address) {
    if (!valid_id(id)) return 1;
    id = as_owner(id);
    for (u32 i = 0; i < K64_TASK_MAPS; i++) {
        map64_t *map = &mappings[id][i];
        u64 end;

        if (!map->used) continue;
        end = map->base + map->pages * K64_PAGE_SIZE;
        if (address >= map->base && address < end) return map->owned != 0;
    }
    return 1;
}

/* Forget the part of a record that munmap released. A range that is covered
 * exactly is dropped and a partial overlap trims the record; a hole in the
 * middle is left in place on purpose, because keeping a range costs nothing
 * while splitting it would need a second slot that may not exist, and a
 * stale record never frees a frame that munmap did not unmap. */
void task64_map_drop(int id, u64 base, u64 length) {
    u64 end = base + length;

    if (!valid_id(id) || length == 0 || end < base) return;
    id = as_owner(id);
    for (u32 i = 0; i < K64_TASK_MAPS; i++) {
        map64_t *map = &mappings[id][i];
        u64 map_end;

        if (!map->used) continue;
        map_end = map->base + map->pages * K64_PAGE_SIZE;
        if (map_end <= base || map->base >= end) continue;
        if (map->base >= base && map_end <= end) {
            map->used = 0;
        } else if (map->base < base) {
            map->pages = (base - map->base) / K64_PAGE_SIZE;
            if (map->pages == 0) map->used = 0;
        } else {
            map->base = end;
            map->pages = (map_end - end) / K64_PAGE_SIZE;
            if (map->pages == 0) map->used = 0;
        }
    }
}

/* ------------------------------------------------------------ fork / exit
 * A forked child gets its own address space: paging64_clone_space shares
 * every page except the ones a mapping record marks shared, which are the
 * same frames writable in both spaces. A private page is mapped read-only
 * and copy-on-write in both, so the copy is paid for by whichever side
 * writes first and a process that never writes costs nothing.
 *
 * A child costs a 16 KiB kernel stack and a copied register frame. The
 * frame is the parent's syscall frame with RAX set to zero, so the child
 * resumes exactly where the parent returned from fork() but with the
 * child's return value.
 *
 * Nothing is released at exit: a DONE task keeps its space, frames and
 * stack until someone reaps it, which is what lets wait4() read the status
 * and the boot test inspect a fault. Reaping is the single cleanup point.
 */
/* `fork` aliases a shared range into the child, so this callback reports
 * every mapping record marked shared. It is used for the clone and for the
 * teardown of a half-built child space: an aliased frame belongs to the
 * parent (or the file cache), never to the child. */
static int fork_shared(u64 virt, void *ctx) {
    return task64_map_shared((int)(u64)ctx, virt);
}

/* At task exit only borrowed frames survive: a shared range the task itself
 * allocated (owned) is freed here like any other frame, while a shared file
 * page or a parent's mapping must stay alive. */
static int exit_keep(u64 virt, void *ctx) {
    int id = (int)(u64)ctx;

    return task64_map_shared(id, virt) && !task64_map_owned(id, virt);
}

/* Drop this task's claim on the address space. The last user frees the
 * frames (borrowed shared pages survive) and the page tables. If the owner
 * dies while a thread still runs, the mapping records and the bump pointer
 * move to that thread, so a later munmap — or the final exit — still sees
 * them, and no frame is leaked or freed twice. */
static void space_detach(int id) {
    int owner = as_owner(id);

    if (owner != id) {
        if (space_refs[owner] > 0) space_refs[owner]--;
        return;
    }
    if (space_refs[id] > 1) {
        for (int i = 1; i < (int)K64_TASK_MAX; i++) {
            if (i == id || tasks[i].state == K64_TASK_UNUSED) continue;
            if (space_owner[i] != id) continue;
            memcpy(mappings[i], mappings[id], sizeof(mappings[id]));
            memset(mappings[id], 0, sizeof(mappings[id]));
            tasks[i].space = tasks[id].space;
            tasks[i].mmap_next = tasks[id].mmap_next;
            {
                u64 k = 0;
                while (tasks[id].cwd[k] && k < K64_FS_PATH_MAX) {
                    tasks[i].cwd[k] = tasks[id].cwd[k];
                    k++;
                }
                tasks[i].cwd[k] = 0;
            }
            space_owner[i] = i;
            for (int j = 1; j < (int)K64_TASK_MAX; j++)
                if (j != i && space_owner[j] == id) space_owner[j] = i;
            space_refs[i] = space_refs[id] - 1;
            space_refs[id] = 0;
            space_owner[id] = id;
            return;
        }
    }
    if (tasks[id].space && space_refs[id] > 0) {
        paging64_free_space_frames(tasks[id].space, exit_keep,
                                   (void *)(u64)id);
        paging64_destroy_space(tasks[id].space);
    }
    space_refs[id] = 0;
}

/* Release everything a reaped task owns: descriptors, the user frames it
 * did not share with another space, its page tables and kernel stack. */
static void finalize_task(int id) {
    u64 stack;

    if (!valid_id(id) || id == 0) return;
    fs64_task_cleanup(id);
    space_detach(id);
    stack = tasks[id].stack_base;
    if (stack) kfree64((void *)stack);
    memset(&mappings[id], 0, sizeof(mappings[id]));
    memset(&tasks[id], 0, sizeof(tasks[id]));
    space_owner[id] = id;
    space_refs[id] = 0;
    tasks[id].state = K64_TASK_UNUSED;
}

regs64_t *sched64_fork(regs64_t *frame) {
    int parent = cur_task();
    int child;
    u64 space;
    u8 *stack;
    u64 top;
    regs64_t *child_frame;

    if (!enabled || parent <= 0 || tasks[parent].state != K64_TASK_RUNNING ||
        !tasks[parent].space) {
        frame->rax = (u64)0 - 11;          /* EAGAIN */
        return frame;
    }

    /* Claim the slot before the slow part. DONE is never scheduled, so the
     * half-built record stays invisible to another CPU's scheduler until the
     * READY store at the end of this function. */
    {
        u64 claim = spin64_lock(&sched_lock);
        child = find_free();
        if (child >= 0) tasks[child].state = K64_TASK_DONE;
        spin64_unlock(&sched_lock, claim);
    }
    if (child < 0) {
        frame->rax = (u64)0 - 11;
        return frame;
    }

    space = paging64_clone_space(tasks[parent].space, fork_shared,
                                 (void *)(u64)parent);
    if (!space) {
        tasks[child].state = K64_TASK_UNUSED;
        frame->rax = (u64)0 - 12;          /* ENOMEM */
        return frame;
    }

    stack = (u8 *)kmalloc64(K64_TASK_STACK_SIZE);
    if (!stack) {
        paging64_free_space_frames(space, fork_shared, (void *)(u64)parent);
        paging64_destroy_space(space);
        tasks[child].state = K64_TASK_UNUSED;
        frame->rax = (u64)0 - 12;
        return frame;
    }
    top = ((u64)stack + K64_TASK_STACK_SIZE) & ~15ull;

    child_frame = frame_on_stack(top, frame->rip);
    *child_frame = *frame;
    child_frame->rax = 0;                  /* fork() returns 0 in the child */

    /* SSE state lives in the registers while the task runs: save the parent
     * now so the child starts with exactly the same state. */
    fxsave64(fxsave_areas[parent]);
    memcpy(fxsave_areas[child], fxsave_areas[parent], sizeof(fxsave_default));

    tasks[child].id = (u32)child;
    tasks[child].parent = parent;
    /* A forked child leads its own thread group, so getpid() has to report
     * the child and not the parent it was copied from. Only clone() with
     * CLONE_THREAD shares a group id. */
    tasks[child].tgid = (u32)child;
    /* Inherit the CPU: the child's address space is a copy and the syscall
     * layer's shared state is not locked, so the child must not run beside
     * its parent on another CPU. */
    tasks[child].cpu = tasks[parent].cpu;
    tasks[child].space = space;
    tasks[child].stack_base = (u64)stack;
    tasks[child].stack_top = top;
    tasks[child].frame = child_frame;
    tasks[child].exit_status = 0;
    tasks[child].term_signal = 0;
    tasks[child].fault_vector = 0;
    tasks[child].fault_address = 0;
    tasks[child].mmap_next = tasks[parent].mmap_next;
    tasks[child].futex_addr = 0;
    tasks[child].brk = 0;
    tasks[child].cwd[0] = '/';
    tasks[child].cwd[1] = 0;
    {
        int owner = as_owner(parent);
        u64 i = 0;
        tasks[child].brk = tasks[owner].brk;
        while (tasks[owner].cwd[i] && i < K64_FS_PATH_MAX) {
            tasks[child].cwd[i] = tasks[owner].cwd[i];
            i++;
        }
        tasks[child].cwd[i] = 0;
        comm_set(child, tasks[owner].comm);
    }
    space_owner[child] = child;
    space_refs[child] = 1;

    /* Inherit the mapping table. A shared range becomes borrow-only in the
     * child, because the frames belong to the original mapping or to the
     * file cache; everything else was copied and is the child's to free. */
    for (u32 i = 0; i < K64_TASK_MAPS; i++) {
        mappings[child][i] = mappings[parent][i];
        if (mappings[child][i].used && mappings[child][i].shared)
            mappings[child][i].owned = 0;
    }

    fs64_fork_fds(parent, child);
    tasks[child].state = K64_TASK_READY;

    frame->rax = (u64)(child + 1);          /* pid is index + 1, like getpid */
    return frame;
}

/* clone() with CLONE_VM: a second task in the same address space. Nothing
 * is copied — not the page tables, not the mapping records, not the bump
 * pointer — so a store in either task is visible to the other, which is what
 * makes this a thread instead of a process. The child resumes at the same
 * RIP as the caller (right after the syscall) with RAX=0 and the caller's
 * requested stack. It is reapable by the task that created it. */
regs64_t *sched64_clone(regs64_t *frame, u64 flags, u64 child_stack) {
    int parent = cur_task();
    int child, owner;
    u8 *stack;
    u64 top;
    regs64_t *child_frame;

    if (!(flags & K64_CLONE_VM)) return sched64_fork(frame);

    if (!enabled || parent <= 0 || tasks[parent].state != K64_TASK_RUNNING ||
        !tasks[parent].space || !child_stack ||
        child_stack < K64_USER_BASE || child_stack >= K64_USER_END ||
        (child_stack & 15ull) ||
        (flags & ~(u64)K64_CLONE_SUPPORTED)) {
        frame->rax = (u64)0 - 22;          /* EINVAL */
        return frame;
    }
    if ((flags & K64_CLONE_THREAD) && !valid_id((int)tasks[parent].tgid)) {
        frame->rax = (u64)0 - 22;
        return frame;
    }

    {
        u64 claim = spin64_lock(&sched_lock);
        child = find_free();
        if (child >= 0) tasks[child].state = K64_TASK_DONE;
        spin64_unlock(&sched_lock, claim);
    }
    if (child < 0) {
        frame->rax = (u64)0 - 11;          /* EAGAIN */
        return frame;
    }

    stack = (u8 *)kmalloc64(K64_TASK_STACK_SIZE);
    if (!stack) {
        tasks[child].state = K64_TASK_UNUSED;
        frame->rax = (u64)0 - 12;          /* ENOMEM */
        return frame;
    }
    top = ((u64)stack + K64_TASK_STACK_SIZE) & ~15ull;

    child_frame = frame_on_stack(top, frame->rip);
    *child_frame = *frame;
    child_frame->rax = 0;                  /* clone() returns 0 in the child */
    child_frame->rsp = child_stack;

    /* The XMM state belongs to the new thread too: it starts where the
     * caller is right now. */
    fxsave64(fxsave_areas[parent]);
    memcpy(fxsave_areas[child], fxsave_areas[parent], sizeof(fxsave_default));

    owner = as_owner(parent);
    tasks[child].id = (u32)child;
    tasks[child].parent = parent;
    tasks[child].tgid = (flags & K64_CLONE_THREAD) ? tasks[parent].tgid
                                                   : (u32)child;
    tasks[child].cpu = tasks[parent].cpu;
    tasks[child].space = tasks[owner].space;
    tasks[child].stack_base = (u64)stack;
    tasks[child].stack_top = top;
    tasks[child].frame = child_frame;
    tasks[child].exit_status = 0;
    tasks[child].term_signal = 0;
    tasks[child].fault_vector = 0;
    tasks[child].fault_address = 0;
    tasks[child].futex_addr = 0;
    tasks[child].brk = 0;
    {
        int source = as_owner(parent);
        u64 i = 0;
        while (tasks[source].cwd[i] && i < K64_FS_PATH_MAX) {
            tasks[child].cwd[i] = tasks[source].cwd[i];
            i++;
        }
        tasks[child].cwd[i] = 0;
        comm_set(child, tasks[source].comm);
    }
    /* The space, its mapping records and the bump pointer stay with the
     * owner; the new thread is one more user until it is reaped. */
    space_owner[child] = owner;
    space_refs[owner]++;

    if (flags & K64_CLONE_FILES)
        fs64_share_fds(parent, child);
    else
        fs64_fork_fds(parent, child);

    tasks[child].state = K64_TASK_READY;
    frame->rax = (u64)(child + 1);
    return frame;
}

/* Park the caller on a futex word. The value check already happened in the
 * syscall, so this only has to record where the task sleeps, complete the
 * syscall (RAX=0) and switch away. The frame is saved *before* the state
 * changes, because the task is not RUNNING any more when switch_to looks at
 * it, yet a wake must resume it through that exact frame. */
regs64_t *sched64_futex_wait(regs64_t *frame, u64 address) {
    k64_percpu_t *cpu = this_cpu();
    int id;
    u64 flags;

    if (!enabled || !cpu) {
        frame->rax = (u64)0 - 22;          /* EINVAL */
        return frame;
    }
    id = (int)cpu->current;
    if (id <= 0) {
        frame->rax = (u64)0 - 22;          /* EINVAL */
        return frame;
    }

    fxsave64(fxsave_areas[id]);
    frame->rax = 0;
    flags = spin64_lock(&sched_lock);
    tasks[id].frame = frame;
    tasks[id].futex_addr = address;
    tasks[id].state = K64_TASK_WAIT;
    spin64_unlock(&sched_lock, flags);

    return switch_away("HAKU64-TASK-HALT: every task is blocked on a"
                       " futex\r\n");
}

/* Wake up to `count` tasks parked on `address`. The match is on the virtual
 * address alone, not on the address space: a futex on a MAP_SHARED page is
 * the same word in two processes, and a spurious wakeup is allowed by the
 * contract (the waiter re-checks the word) while a missed one deadlocks. */
int sched64_futex_wake(int id, u64 address, int count) {
    int woken = 0;
    u32 cpus_mask = 0;
    u64 flags;

    if (!valid_id(id) || count <= 0) return 0;

    flags = spin64_lock(&sched_lock);
    for (int i = 1; i < (int)K64_TASK_MAX; i++) {
        if (tasks[i].state != K64_TASK_WAIT) continue;
        if (tasks[i].futex_addr != address) continue;
        tasks[i].futex_addr = 0;
        tasks[i].brk = 0;
        tasks[i].state = K64_TASK_READY;
        if (tasks[i].cpu < K64_SMP_MAX_CPUS)
            cpus_mask |= 1u << tasks[i].cpu;
        if (++woken >= count) break;
    }
    spin64_unlock(&sched_lock, flags);

    /* A waiter on another CPU only notices the state change when its own
     * scheduler next runs, so ask each affected CPU to reschedule now. */
    for (u32 c = 0; c < K64_SMP_MAX_CPUS; c++)
        if (cpus_mask & (1u << c)) smp64_resched_cpu(c);
    return woken;
}

/* execve's hand-off: the caller has just loaded a fresh image, so the old
 * space is unreachable from ring 3 and is released here (aliased shared
 * pages survive, exactly as they do at exit). The mapping table belongs to
 * the old image and is cleared; the new image starts with an empty anonymous
 * window. The caller is responsible for RIP/RSP and for switching CR3.
 * Replacing a shared space is refused: there is no way to stop the other
 * threads, so execve from a thread fails instead of leaving them behind. */
int task64_exec(int id, u64 space) {
    if (!valid_id(id) || id == 0 || !space) return 0;
    if (as_owner(id) != id || space_refs[id] > 1) return 0;
    if (tasks[id].space && tasks[id].space != space) {
        paging64_free_space_frames(tasks[id].space, exit_keep,
                                   (void *)(u64)id);
        paging64_destroy_space(tasks[id].space);
    }
    memset(&mappings[id], 0, sizeof(mappings[id]));
    tasks[id].space = space;
    tasks[id].mmap_next = K64_USER_MMAP_BASE;
    tasks[id].fault_vector = 0;
    tasks[id].fault_address = 0;
    return 1;
}

/* Reap one child of `parent`. `pid` < 0 means any child, otherwise it must
 * match the child's pid. Returns the child's pid, -ECHILD when there is no
 * such child, or -EAGAIN when a matching child is still running. */
int task64_reap(int parent, int pid, u64 *status) {
    int match = -1;

    if (!valid_id(parent)) return -3;      /* ESRCH */

    for (int i = 1; i < (int)K64_TASK_MAX; i++) {
        if (tasks[i].state == K64_TASK_UNUSED) continue;
        if (tasks[i].parent != parent) continue;
        /* `pid` is the user-visible id (index + 1), not the slot index. */
        if (pid >= 0 && (int)tasks[i].id + 1 != pid) continue;
        if (tasks[i].state == K64_TASK_DONE) {
            u64 code = tasks[i].term_signal
                           ? (u64)tasks[i].term_signal
                           : (tasks[i].exit_status & 0xFFu) << 8;
            int id = i;
            int result = id + 1;

            if (status) *status = code;
            finalize_task(id);
            return result;
        }
        match = i;
    }
    return match < 0 ? -10 : -11;          /* ECHILD / EAGAIN */
}

/* Deliver a signal. There are no handlers, so the only action is the
 * default one: terminate. Signal 0 is the usual "does this pid exist"
 * probe and only checks the target. A task cannot signal itself here
 * because the dispatcher has to know it must not resume the frame; kill()
 * of the caller goes through exit() instead. */
int task64_signal(int id, u32 signal) {
    u32 owner;
    u64 flags;

    if (!valid_id(id) || tasks[id].state == K64_TASK_UNUSED) return -3;
    if (id == cur_task()) return -22;      /* EINVAL: use exit() */
    if (signal == 0) return 0;

    flags = spin64_lock(&sched_lock);
    if (tasks[id].state == K64_TASK_DONE) {
        spin64_unlock(&sched_lock, flags);
        return 0;
    }
    tasks[id].term_signal = signal;
    tasks[id].state = K64_TASK_DONE;
    owner = tasks[id].cpu;
    spin64_unlock(&sched_lock, flags);
    smp64_resched_cpu(owner);
    return 0;
}

int task64_parent(int id) {
    if (!valid_id(id) || tasks[id].parent < 0) return -1;
    return tasks[id].parent;
}

/* Total across CPUs: the self-test asserts that switching happened, and with
 * a per-CPU counter that question only has a machine-wide answer. */
u64 sched64_switches(void) {
    u64 total = 0;

    for (u32 i = 0; i < K64_SMP_MAX_CPUS; i++) total += cpus64[i].switches;
    return total;
}

int sched64_current(void) {
    return cur_task();
}

/* ------------------------------------------------------- application CPUs
 * An AP starts with no task: `current` is (u32)-1 until the bootstrap
 * processor has built its idle task and released it. The idle task is a
 * normal kernel task that never exits, so every later switch has somewhere to
 * land, and `isr64_resume64` is the one-time lie that "this CPU was
 * preempted right there". */

static void k64_idle_main(void) {
    /* The frame is built with IF set, so HLT sleeps until the local timer or
     * an IPI wakes the CPU; the interrupt stub returns here afterwards. */
    for (;;) cpu_halt64();
}

/* Runs on the bootstrap processor, before that AP is released. The stack is
 * allocated from the shared kernel heap while one CPU is still the only
 * allocator user, which is what makes the hand-off simple. */
int sched64_prepare_ap(u32 cpu_index) {
    int id;

    if (cpu_index == 0 || cpu_index >= K64_SMP_MAX_CPUS) return -1;
    if (cpus64[cpu_index].idle != (u32)-1) return (int)cpus64[cpu_index].idle;

    id = task64_create_kernel_on(cpu_index, k64_idle_main);
    if (id < 0) return -1;
    cpus64[cpu_index].idle = (u32)id;
    return id;
}

/* Runs on the AP. Never returns: the CPU is now a task context, so every
 * later entry into the kernel is an ordinary interrupt frame. */
void sched64_start_ap(u32 cpu_index) {
    k64_percpu_t *cpu = &cpus64[cpu_index];
    int idle;
    u64 flags;

    if (cpu_index == 0 || cpu_index >= K64_SMP_MAX_CPUS || !cpu->online) return;

    flags = spin64_lock(&sched_lock);
    idle = (int)cpu->idle;
    if (idle < 0) {
        spin64_unlock(&sched_lock, flags);
        return;
    }
    cpu->current = (u32)idle;
    cpu->cursor = (u32)idle;
    cpu->active_space = tasks[idle].space;
    tasks[idle].state = K64_TASK_RUNNING;
    if (tasks[idle].stack_top) {
        gdt64_set_rsp0_cpu(cpu_index, tasks[idle].stack_top);
        cpu->rsp0 = tasks[idle].stack_top;
    }
    /* Leave interrupts off across the unwind: a tick arriving between the
     * state above and the IRETQ would save a frame from the parking loop as
     * the idle task's context. The frame restores IF itself. */
    spin64_unlock(&sched_lock, 0);

    timer64_lapic_start_local();
    isr64_resume64(tasks[idle].frame);
}
