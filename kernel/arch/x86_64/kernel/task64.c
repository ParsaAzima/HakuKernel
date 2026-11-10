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
    u64 space;      /* PML4 physical address this task runs under */
    u64 stack_top;  /* kernel stack, i.e. TSS.RSP0 while the task runs */
    regs64_t *frame;
    u64 exit_status; /* exit/exit_group argument, readable after DONE */
    u64 fault_vector; /* non-zero when the task stopped on a ring-3 fault */
    u64 fault_address;
    u64 mmap_next;   /* per-task bump pointer for anonymous mappings */
} task64_t;

static task64_t tasks[K64_TASK_MAX];
static int current_index = -1;
static u32 enabled;
static u64 switch_count;

/* One FXSAVE area per task. The area is 512 bytes and must be 16-byte
 * aligned, which a __declspec(align(16)) array guarantees. */
static __declspec(align(16)) u8 fxsave_areas[K64_TASK_MAX][512];
static __declspec(align(16)) u8 fxsave_default[512];

static int valid_id(int id) {
    return id >= 0 && id < (int)K64_TASK_MAX;
}

static int find_free(void) {
    for (int i = 1; i < (int)K64_TASK_MAX; i++)
        if (tasks[i].state == K64_TASK_UNUSED) return i;
    return -1;
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

void sched64_init(void) {
    memset(tasks, 0, sizeof(tasks));
    current_index = 0;
    switch_count = 0;

    enable_sse64();
    fxsave64(fxsave_default);

    tasks[0].id = 0;
    tasks[0].state = K64_TASK_RUNNING;
    tasks[0].space = read_cr3_64() & K64_PAGE_MASK;
    tasks[0].stack_top = 0;        /* ring 0 only, so RSP0 is not used */
    tasks[0].frame = 0;
    tasks[0].exit_status = 0;
    tasks[0].fault_vector = 0;
    tasks[0].fault_address = 0;
    tasks[0].mmap_next = 0;
    memcpy(fxsave_areas[0], fxsave_default, sizeof(fxsave_default));

    enabled = 1;
}

int task64_create_kernel(void (*entry)(void)) {
    int id;
    u8 *stack;
    u64 top;

    if (!enabled) return -1;
    id = find_free();
    if (id < 0) return -1;

    stack = (u8 *)kmalloc64(K64_TASK_STACK_SIZE);
    if (!stack) return -1;
    top = ((u64)stack + K64_TASK_STACK_SIZE) & ~15ull;

    tasks[id].id = (u32)id;
    tasks[id].space = read_cr3_64() & K64_PAGE_MASK;
    tasks[id].stack_top = top;
    tasks[id].frame = frame_on_stack(top, (u64)entry);
    tasks[id].exit_status = 0;
    tasks[id].fault_vector = 0;
    tasks[id].fault_address = 0;
    tasks[id].mmap_next = 0;
    prepare_sse(id);
    tasks[id].state = K64_TASK_READY;
    return id;
}

int task64_create_user(u64 entry, u64 user_stack_top, u64 space) {
    int id;
    u8 *stack;
    u64 top;
    regs64_t *frame;

    if (!enabled || !space) return -1;
    id = find_free();
    if (id < 0) return -1;

    stack = (u8 *)kmalloc64(K64_TASK_STACK_SIZE);
    if (!stack) return -1;
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
    tasks[id].space = space;
    tasks[id].stack_top = top;
    tasks[id].frame = frame;
    tasks[id].exit_status = 0;
    tasks[id].fault_vector = 0;
    tasks[id].fault_address = 0;
    tasks[id].mmap_next = K64_USER_MMAP_BASE;
    prepare_sse(id);
    tasks[id].state = K64_TASK_READY;
    return id;
}

/* Round-robin from the current task. The caller is the task that just ran
 * (or is running), so the scan starts one slot past it. */
static int pick_ready(void) {
    for (u32 step = 1; step < K64_TASK_MAX; step++) {
        int idx = (current_index + (int)step) % (int)K64_TASK_MAX;
        if (tasks[idx].state == K64_TASK_READY) return idx;
    }
    return -1;
}

static regs64_t *switch_to(int next, int save_current) {
    int old = current_index;

    if (save_current && old >= 0 && tasks[old].state == K64_TASK_RUNNING)
        fxsave64(fxsave_areas[old]);

    if (old >= 0 && tasks[old].state == K64_TASK_RUNNING)
        tasks[old].state = K64_TASK_READY;
    tasks[next].state = K64_TASK_RUNNING;
    current_index = next;
    switch_count++;

    /* The incoming area belongs to the kernel heap, which is identity
     * mapped in every space, so this is valid before the CR3 write too. */
    fxrstor64(fxsave_areas[next]);
    write_cr3_64(tasks[next].space);
    gdt64_set_rsp0(tasks[next].stack_top);
    return tasks[next].frame;
}

regs64_t *sched64_tick(regs64_t *frame) {
    int next;

    if (!enabled) return frame;
    if (current_index >= 0 && tasks[current_index].state == K64_TASK_RUNNING)
        tasks[current_index].frame = frame;

    next = pick_ready();
    if (next < 0) return frame;     /* nothing else to run: stay here */
    return switch_to(next, 1);
}

/* Called from the system call handler for SYS_EXIT. The exit frame is not
 * saved because the task never resumes; a process that wants to continue
 * after a syscall just returns through the normal path. */
regs64_t *sched64_exit(regs64_t *frame) {
    int next;

    if (!enabled || current_index < 0) return frame;
    tasks[current_index].state = K64_TASK_DONE;
    /* exit()/exit_group() pass the status in RDI; the frame is not saved
     * because the task never resumes, but the value is what a parent would
     * read back. */
    tasks[current_index].exit_status = frame->rdi;

    next = pick_ready();
    if (next < 0) {
        k64_puts("HAKU64-TASK-HALT: no runnable task after exit\r\n");
        cli64();
        for (;;) cpu_halt64();
    }
    return switch_to(next, 0);
}

/* A ring-3 exception ends the task: there is no signal model on the 64-bit
 * port, so the closest honest behaviour is a terminated process. The fault
 * reason is kept for the boot test (and a future parent). */
regs64_t *sched64_fault(regs64_t *frame, u32 vector, u64 address) {
    int next;

    if (!enabled || current_index < 0) return frame;
    tasks[current_index].state = K64_TASK_DONE;
    tasks[current_index].fault_vector = vector;
    tasks[current_index].fault_address = address;

    next = pick_ready();
    if (next < 0) {
        k64_puts("HAKU64-TASK-HALT: no runnable task after a user fault\r\n");
        cli64();
        for (;;) cpu_halt64();
    }
    return switch_to(next, 0);
}

/* Mark a task finished from outside (the boot self-test does this to the
 * CPU-burner tasks once they have proven they ran). The current task is
 * never killed this way, because it would then have no frame to return
 * from. */
void task64_kill(int id) {
    if (!valid_id(id) || id == current_index) return;
    tasks[id].state = K64_TASK_DONE;
}

int task64_state(int id) {
    return valid_id(id) ? (int)tasks[id].state : (int)K64_TASK_UNUSED;
}

u64 task64_kernel_stack(int id) {
    return valid_id(id) ? tasks[id].stack_top : 0;
}

u64 sched64_space(int id) {
    return valid_id(id) ? tasks[id].space : 0;
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
    return valid_id(id) ? tasks[id].mmap_next : 0;
}

/* Reserve `pages` at the task's bump pointer. The reservation is recorded
 * before the caller maps anything, so a failed mapping does not have to
 * rewind the pointer: the address range is simply never reused. */
u64 task64_mmap_reserve(int id, u64 pages) {
    u64 base, next;

    if (!valid_id(id) || pages == 0 || pages > K64_USER_MMAP_PAGES_MAX)
        return 0;
    base = tasks[id].mmap_next;
    if (base < K64_USER_MMAP_BASE) base = K64_USER_MMAP_BASE;
    next = base + pages * K64_PAGE_SIZE;
    if (next < base || next > K64_USER_STACK_BOTTOM) return 0;
    tasks[id].mmap_next = next;
    return base;
}

u64 sched64_switches(void) {
    return switch_count;
}

int sched64_current(void) {
    return current_index;
}
