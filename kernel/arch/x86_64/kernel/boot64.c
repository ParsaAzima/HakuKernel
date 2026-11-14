/* Haku x86-64 bring-up: from long mode to a working memory subsystem and
 * an interrupt-driven clock.
 *
 * entry64.asm enters long mode, installs an identity map of the first 1 GiB
 * with 2 MiB pages, and calls kernel64_main. From here the 64-bit kernel
 * brings up, in order:
 *
 *   1. the E820 inventory stage2 left at 0x7000,
 *   2. a supervisor physmap of all RAM at K64_PHYS_MAP_BASE (2 MiB pages),
 *   3. a frame allocator over that map,
 *   4. a kernel heap,
 *   5. the GDT/TSS with its IST stacks,
 *   6. the IDT, the 8259 remap, and the 100 Hz PIT,
 *   7. per-process address spaces (two PML4s mapping one virtual address
 *      to different frames, switched by writing CR3),
 *   8. two kernel tasks proving the frame-pointer context switch, then a
 *      ring-3 task whose INT 0x80 calls prove the syscall gate,
 *   9. the ELF64 loader: malformed images refused, then an embedded static
 *      executable mapped into a fresh space and run in ring 3.
 *
 * Each step prints a marker the headless check asserts, so a regression
 * shows up as a missing line rather than a silent hang. The 32-bit kernel
 * is still the default image and this path has no userland yet; when the
 * sequence finishes the CPU halts with interrupts enabled.
 */
#include "haku64.h"
#include "haku/abi.h"
#include "haku_module.h"

typedef struct {
    u32 base_low;
    u32 base_high;
    u32 length_low;
    u32 length_high;
    u32 type;
    u32 attributes;
} e820_entry64_t;

static const volatile e820_entry64_t *e820_entries(void) {
    return (const volatile e820_entry64_t *)K64_E820_MAP;
}

/* Highest usable physical address, and how much RAM is usable in total. */
static u64 e820_top(void) {
    u64 top = 0;
    u32 count = K64_E820_COUNT;
    const volatile e820_entry64_t *entries = e820_entries();

    for (u32 i = 0; i < count && i < K64_E820_MAX; i++) {
        u64 base = ((u64)entries[i].base_high << 32) | entries[i].base_low;
        u64 length = ((u64)entries[i].length_high << 32) |
                     entries[i].length_low;
        if (entries[i].type != K64_E820_RAM) continue;
        if (!(entries[i].attributes & 1u) || length == 0) continue;
        if (base + length > top) top = base + length;
    }
    return top;
}

static u64 e820_usable_bytes(void) {
    u64 total = 0;
    u32 count = K64_E820_COUNT;
    const volatile e820_entry64_t *entries = e820_entries();

    for (u32 i = 0; i < count && i < K64_E820_MAX; i++) {
        u64 length = ((u64)entries[i].length_high << 32) |
                     entries[i].length_low;
        if (entries[i].type != K64_E820_RAM) continue;
        if (!(entries[i].attributes & 1u)) continue;
        total += length;
    }
    return total;
}

static void report_e820(void) {
    k64_puts("HAKU64-E820-OK: entries=");
    k64_dec(K64_E820_COUNT);
    k64_puts(" usable=");
    k64_dec(e820_usable_bytes() >> 20);
    k64_puts("MiB top=");
    k64_hex(e820_top());
    k64_puts("\r\n");
}

/* The physmap must agree with the identity map: reading the E820 count
 * through both must give the same value. */
static int test_physmap(void) {
    volatile u32 *identity = (volatile u32 *)0x6FFC;
    volatile u32 *mapped = (volatile u32 *)(K64_PHYS_MAP_BASE + 0x6FFC);

    if (*identity != *mapped) return 0;
    /* And a frame the allocator actually hands out must be writable
     * through the map, not just readable. */
    {
        u64 phys = frame64_alloc();
        volatile u64 *cell;
        if (!phys) return 0;
        cell = (volatile u64 *)phys_to_virt64(phys);
        *cell = 0x0123456789ABCDEFull;
        if (*cell != 0x0123456789ABCDEFull) return 0;
        frame64_free(phys);
    }
    return 1;
}

static int test_frames(void) {
    u64 before = frame64_free_count();
    u64 a = frame64_alloc();
    u64 b = frame64_alloc();
    volatile u64 *pa;
    volatile u64 *pb;

    if (!a || !b || a == b) return 0;
    pa = (volatile u64 *)phys_to_virt64(a);
    pb = (volatile u64 *)phys_to_virt64(b);
    *pa = 0x1111111111111111ull;
    *pb = 0x2222222222222222ull;
    if (*pa != 0x1111111111111111ull) return 0;
    if (*pb != 0x2222222222222222ull) return 0;
    frame64_free(a);
    frame64_free(b);
    if (frame64_free_count() != before) return 0;

    /* Reserved and misaligned addresses must never become allocatable. */
    frame64_free(0);
    frame64_free(K64_HEAP_START);
    frame64_free(K64_HEAP_START + 1);
    if (frame64_free_count() != before) return 0;

    /* The physmap has to reach the top of installed RAM: the highest free
     * frame is poked through the map. It is free, so nothing is corrupted. */
    {
        u64 top = frame64_highest_free();
        volatile u64 *cell;
        if (!top) return 0;
        cell = (volatile u64 *)phys_to_virt64(top);
        *cell = 0x4444444444444444ull;
        if (*cell != 0x4444444444444444ull) return 0;
    }
    return 1;
}

static int test_paging(void) {
    /* A private virtual address outside both the identity map and the
     * physmap slot: PML4 index 498. */
    const u64 virt = 0xFFFF900000000000ull;
    u64 phys = frame64_alloc();
    volatile u64 *cell;

    if (!phys) return 0;
    if (!paging64_map_page(virt, phys, 1)) {
        frame64_free(phys);
        return 0;
    }
    if (paging64_translate(virt) != phys) {
        paging64_unmap_page(virt);
        frame64_free(phys);
        return 0;
    }
    cell = (volatile u64 *)virt;
    *cell = 0xCAFEBABEDEADBEEFull;
    if (*cell != 0xCAFEBABEDEADBEEFull) {
        paging64_unmap_page(virt);
        frame64_free(phys);
        return 0;
    }
    paging64_unmap_page(virt);
    frame64_free(phys);
    return 1;
}

static int test_heap(void) {
    u64 before = heap64_free_bytes();
    char *a = (char *)kmalloc64(64);
    char *b;
    char *c;

    if (!a) return 0;
    memset(a, 'A', 64);
    if (a[0] != 'A' || a[63] != 'A') return 0;

    b = (char *)kmalloc64(4096);
    if (!b || b == a) return 0;
    memset(b, 'B', 4096);

    kfree64(a);
    c = (char *)kmalloc64(32);          /* must reuse the freed block */
    if (!c) return 0;
    kfree64(c);
    kfree64(b);
    if (heap64_free_bytes() != before) return 0;

    /* A request larger than the window must fail, not wrap. */
    if (kmalloc64(K64_HEAP_SIZE * 2) != 0) return 0;
    return 1;
}

/* The per-CPU block is what makes gs:[off] mean "this CPU", so the test has
 * to prove three separate things: the compiler's field offsets still match
 * the two the assembly hardcodes, GS.base really points at this CPU's block,
 * and the interrupt-owning spin lock restores the caller's flag state instead
 * of leaving interrupts on. It runs before anything uses the block. */
static int test_percpu(void) {
    k64_percpu_t *cpu = k64_cpu();
    u64 gs_base;
    u64 flags;
    spinlock64_t probe = SPINLOCK64_INIT;

    if (!percpu64_offsets_ok()) return 0;
    if (!cpu) return 0;
    if (cpu->self != (u64)cpu) return 0;
    if (cpu->index != 0 || k64_cpu_index() != 0) return 0;
    if (!cpu->online) return 0;
    /* Adjacent CPUs must not share a cache line. */
    if (((u64)cpu & 63ull) != 0) return 0;

    gs_base = read_msr64(K64_MSR_GS_BASE);
    if (gs_base != (u64)cpu) return 0;
    /* Ring 0 keeps the user half empty; the SWAPGS invariant depends on it. */
    if (read_msr64(K64_MSR_KERNEL_GS_BASE) != 0) return 0;

    /* Locking owns interrupts: the flags come back exactly as they were. */
    sti64();
    flags = spin64_lock(&probe);
    if ((flags & 0x200ull) == 0) return 0;      /* IF was on when taken */
    if (!probe.locked) return 0;
    spin64_unlock(&probe, flags);
    if (probe.locked) return 0;
    if ((read_rflags64() & 0x200ull) == 0) return 0;
    /* ...and taking it with IF already clear leaves IF clear. */
    cli64();
    flags = spin64_lock(&probe);
    spin64_unlock(&probe, flags);
    if (read_rflags64() & 0x200ull) return 0;
    sti64();

    k64_puts("HAKU64-PERCPU-OK: cpu=0 self=");
    k64_hex(cpu->self);
    k64_puts(" rsp0_off=");
    k64_dec((u64)((u8 *)&cpu->rsp0 - (u8 *)cpu));
    k64_puts(" user_rsp_off=");
    k64_dec((u64)((u8 *)&cpu->user_rsp - (u8 *)cpu));
    k64_puts("\r\n");
    return 1;
}

/* The table layout, as opposed to "an interrupt happened": every gate is a
 * DPL-0 interrupt gate except the system call vector, #DF and NMI get their
 * own IST stacks, the rest share the interrupted stack, each vector has its
 * own stub, and the unused vectors all fall through to the catch-all. */static int test_idt_table(void) {
    u32 v;

    for (v = 0; v < K64_IDT_ENTRIES; v++) {
        u8 expected = (v == K64_SYSCALL_VECTOR) ? K64_GATE_USER : K64_GATE_INT;
        if (idt64_gate_type(v) != expected) return 0;
    }
    if (idt64_gate_ist(8) != 1) return 0;     /* #DF -> IST1 */
    if (idt64_gate_ist(2) != 2) return 0;     /* NMI -> IST2 */
    if (idt64_gate_ist(14) != 0) return 0;    /* #PF -> interrupted stack */
    if (idt64_gate_ist(K64_SYSCALL_VECTOR) != 0) return 0;
    if (idt64_gate_ist(K64_IRQ_BASE) != 0) return 0;

    /* Distinct stubs where the vector is part of the pushed frame... */
    if (idt64_gate_handler(0) == idt64_gate_handler(1)) return 0;
    if (idt64_gate_handler(K64_IRQ_BASE) ==
        idt64_gate_handler(K64_IRQ_BASE + 1)) return 0;
    if (idt64_gate_handler(0) == idt64_gate_handler(K64_IRQ_BASE)) return 0;
    if (idt64_gate_handler(K64_SYSCALL_VECTOR) ==
        idt64_gate_handler(255)) return 0;
    /* ...and one shared catch-all above the IRQ range. */
    if (idt64_gate_handler(48) != idt64_gate_handler(255)) return 0;
    if (idt64_gate_handler(255) == idt64_gate_handler(31)) return 0;
    return 1;
}

/* An address in PML4 slot 416: canonical (bits 63..48 sign-extend bit 47)
 * and never populated. The neighbouring slots are taken — 256 is the
 * physmap, 384 the ACPI window and 448 the local-APIC window — so the probe
 * must stay clear of those three. */
#define K64_UNMAPPED_PROBE 0xFFFFD00000000000ull

static int test_exceptions(void) {
    /* INT3 is a trap, not a fault: the handler reports it and returns, and
     * the probe continues at the instruction after INT3. */
    int3_probe64();
    if (exc64_count(3) != 1) return 0;

    /* The divide probe has to work before it is made to fault, otherwise a
     * "recovered" #DE would not prove anything. */
    if (probe_div64(10, 2) != 5) return 0;

    /* A load from an unmapped kernel-half address. The handler resumes the
     * probe at an exported label, so this returns with a stale RAX. */
    probe_load64(K64_UNMAPPED_PROBE);
    if (exc64_count(14) != 1) return 0;
    if (exc64_last_cr2() != K64_UNMAPPED_PROBE) return 0;

    /* Divide by zero, recovered the same way. */
    probe_div64(1, 0);
    if (exc64_count(0) != 1) return 0;
    return 1;
}

static int test_timer(void) {
    u64 start = timer64_ticks();
    u64 spins = 0;

    /* Wait for five ticks: 50 ms at 100 Hz. Bounded, so a timer that never
     * fires fails the check instead of hanging the boot. */
    while (timer64_ticks() < start + 5) {
        if (++spins > 200000000ull) return 0;
        cpu_pause64();
    }

    /* HLT must wake on an interrupt. With IRQ0 running this returns within
     * 10 ms; with interrupts off or the PIC masked it would never return. */
    start = timer64_ticks();
    cpu_halt64();
    if (timer64_ticks() == start) return 0;
    return 1;
}

/* The RTC anchor must be a plausible calendar time and must advance with
 * the tick counter. Nothing here depends on the host's current year, so
 * the check stays stable on any machine QEMU emulates. */
static int test_rtc(void) {
    u64 epoch = rtc64_epoch();
    u64 hz = timer64_hz();
    u64 now = timer64_ticks();

    /* 2000-01-01 .. 2200-01-01 in Unix seconds. */
    if (epoch < 946684800ull || epoch > 7258118400ull) return 0;
    if (hz == 0) return 0;
    if (rtc64_realtime(now, hz) < epoch) return 0;
    if (rtc64_realtime(now + hz, hz) != rtc64_realtime(now, hz) + 1) return 0;
    return 1;
}

/* ------------------------------------------------------- user address space
 * Three pages in PML4 slot 1. The code page is read-execute, the other two
 * are writable; nothing in slot 0 or the physmap slot is touched. */
#define USER_CODE_ADDR  (K64_USER_BASE)
#define USER_DATA_ADDR  (K64_USER_BASE + 0x1000)
#define USER_STACK_PAGE (K64_USER_BASE + 0x2000)
#define USER_STACK_TOP  (K64_USER_BASE + 0x3000)

/* Two PML4s, one virtual address, two different frames. Switching CR3 must
 * make the same pointer read different memory, and the kernel physmap and
 * identity map must keep working under either space. */
static int test_spaces(void) {
    const u64 virt = K64_USER_BASE + 0x100000;   /* 1 MiB into slot 1 */
    u64 space_a = paging64_create_space();
    u64 space_b = paging64_create_space();
    u64 frame_a = frame64_alloc();
    u64 frame_b = frame64_alloc();
    u64 saved;
    volatile u64 *cell;
    u32 ok = 1;

    if (!space_a || !space_b || !frame_a || !frame_b) return 0;
    if (!paging64_map_space(space_a, virt, frame_a, 1, 1, 0)) return 0;
    if (!paging64_map_space(space_b, virt, frame_b, 1, 1, 0)) return 0;
    if (paging64_translate_space(space_a, virt) != frame_a) return 0;
    if (paging64_translate_space(space_b, virt) != frame_b) return 0;

    saved = read_cr3_64();
    cell = (volatile u64 *)virt;

    paging64_switch_space(space_a);
    *cell = 0xAAAAAAAAAAAAAAAAull;
    /* The same byte through the physmap, and the identity map that every
     * space inherits, must still resolve. */
    if (*(volatile u64 *)phys_to_virt64(frame_a) != 0xAAAAAAAAAAAAAAAAull)
        ok = 0;
    if (*(volatile u32 *)0x6FFC != K64_E820_COUNT) ok = 0;

    paging64_switch_space(space_b);
    if (paging64_translate_space(space_b, virt) != frame_b) ok = 0;
    *cell = 0xBBBBBBBBBBBBBBBBull;
    if (*(volatile u64 *)phys_to_virt64(frame_a) != 0xAAAAAAAAAAAAAAAAull)
        ok = 0;
    if (*(volatile u64 *)phys_to_virt64(frame_b) != 0xBBBBBBBBBBBBBBBBull)
        ok = 0;

    paging64_switch_space(space_a);
    if (*cell != 0xAAAAAAAAAAAAAAAAull) ok = 0;
    paging64_switch_space(space_b);
    if (*cell != 0xBBBBBBBBBBBBBBBBull) ok = 0;

    paging64_switch_space(saved);
    if (!ok) return 0;

    paging64_destroy_space(space_a);
    paging64_destroy_space(space_b);
    frame64_free(frame_a);
    frame64_free(frame_b);
    return 1;
}

/* Two kernel tasks that never yield: the only thing that can make both
 * counters advance is the timer interrupt switching between their stacks.
 * Each records the address of a local so the test can show the stacks are
 * genuinely different. */
static volatile u32 task_hits[2];
static volatile u64 task_stack_mark[2];

static void burn_task_a(void) {
    volatile u64 local = 0;
    task_stack_mark[0] = (u64)&local;
    for (;;) {
        task_hits[0]++;
        for (u32 spin = 0; spin < 200000; spin++) cpu_pause64();
    }
}

static void burn_task_b(void) {
    volatile u64 local = 0;
    task_stack_mark[1] = (u64)&local;
    for (;;) {
        task_hits[1]++;
        for (u32 spin = 0; spin < 200000; spin++) cpu_pause64();
    }
}

static int test_tasks(void) {
    int a, b;
    u64 deadline;

    sched64_init();
    a = task64_create_kernel(burn_task_a);
    b = task64_create_kernel(burn_task_b);
    if (a < 0 || b < 0) return 0;

    deadline = timer64_ticks() + 200;   /* 2 s at 100 Hz, bounded */
    while ((task_hits[0] < 3 || task_hits[1] < 3) &&
           timer64_ticks() < deadline)
        cpu_pause64();
    if (task_hits[0] < 3 || task_hits[1] < 3) return 0;

    /* Separate 16 KiB stacks: the two locals cannot share a page. */
    if (task_stack_mark[0] == 0 || task_stack_mark[1] == 0) return 0;
    if ((task_stack_mark[0] >> 12) == (task_stack_mark[1] >> 12)) return 0;
    if (sched64_switches() < 4) return 0;

    task64_kill(a);
    task64_kill(b);
    return 1;
}

/* --------------------------------------------------- per-CPU scheduling
 * One worker per application processor. Each increments the counter for the
 * CPU it is running on, which is only possible if the AP dispatched a task of
 * its own: nothing migrates a task, so a hit on CPU n proves CPU n ran a
 * task from the shared table. The worker then spins until the bootstrap
 * processor releases it; it never returns, because a kernel task that
 * returned would run off the end of its stack. */
static volatile u32 smp_hits[K64_SMP_MAX_CPUS];
static volatile u32 smp_stop;

static void smp_worker(void) {
    u32 me = k64_cpu_index();

    if (me < K64_SMP_MAX_CPUS) smp_hits[me]++;
    while (!smp_stop) {
        for (u32 spin = 0; spin < 100000; spin++) cpu_pause64();
    }
    for (;;) cpu_halt64();
}

/* Hand every AP its idle task and start it scheduling, then prove that two
 * CPUs ran two tasks at the same time: the workers are created after the
 * release, and each only ever runs on the AP it was pinned to. */
static int test_smp_sched(void) {
    u32 cpus = smp64_cpu_count();
    u64 deadline;
    int created = 0;

    if (cpus < 2) {
        k64_puts("HAKU64-SMP-SCHED-SKIP: uniprocessor\r\n");
        return 1;
    }

    smp_stop = 0;
    for (u32 i = 0; i < K64_SMP_MAX_CPUS; i++) smp_hits[i] = 0;

    /* The AP tick is derived from the PIT, so the measurement has to happen
     * while IRQ0 is the only tick source in the system. */
    timer64_lapic_calibrate();
    if (smp64_release_aps() != (int)cpus - 1) return 0;

    for (u32 i = 1; i < cpus; i++) {
        if (task64_create_kernel_on(i, smp_worker) < 0) return 0;
        created++;
    }

    deadline = timer64_ticks() + 300;   /* 3 s at 100 Hz, bounded */
    for (;;) {
        u32 seen = 0;

        for (u32 i = 1; i < cpus; i++)
            if (smp_hits[i]) seen++;
        if (seen == cpus - 1) break;
        if (timer64_ticks() >= deadline) return 0;
        cpu_pause64();
    }

    /* Per-CPU, not a shared counter: only the APs have workers. */
    for (u32 i = 0; i < K64_SMP_MAX_CPUS; i++) {
        if (i >= 1 && i < cpus) {
            if (!smp_hits[i]) return 0;
        } else if (smp_hits[i]) {
            return 0;
        }
    }
    /* The bootstrap processor kept switching between its own tasks too. */
    if (sched64_switches() < 4) return 0;

    k64_puts("HAKU64-SMP-SCHED-OK: cpus=");
    k64_dec(cpus);
    k64_puts(" workers=");
    k64_dec(created);
    k64_puts(" lapic_hz=");
    k64_dec(timer64_lapic_counts_per_sec());
    k64_puts("\r\n");

    /* Leave the workers running: killing a task another CPU is executing is
     * exactly the case the reschedule IPI exists for, and the boot tests do
     * not need that slot back. */
    smp_stop = 1;
    return 1;
}

/* No address space is ever mapped at this physical address: frames come from
 * RAM below the physmap limit, so a shootdown naming it must reach nobody. */
#define SMP64_BOGUS_SPACE 0x0000FFFF00000000ull

/* A full-space shootdown has to reach every other CPU that could have the
 * space cached, be acknowledged, and not be a silent no-op. The counters are
 * per CPU for the same reason the hits above are. */
static int test_smp_tlb(void) {
    u32 cpus = smp64_cpu_count();
    u32 targets;
    u32 timeouts;
    u64 before = 0;
    u64 after = 0;

    if (cpus < 2) {
        k64_puts("HAKU64-SMP-TLB-SKIP: uniprocessor\r\n");
        return 1;
    }
    for (u32 i = 0; i < cpus; i++) before += cpus64[i].flushes;
    timeouts = smp64_tlb_timeouts();

    /* A space no CPU runs costs one sequence number and no IPI. */
    if (smp64_tlb_shootdown(SMP64_BOGUS_SPACE) != 0) return 0;
    if (smp64_tlb_timeouts() != timeouts) return 0;

    targets = smp64_tlb_shootdown(0);          /* every other CPU */
    if (targets != cpus - 1) return 0;
    if (smp64_tlb_timeouts() != timeouts) return 0;

    for (u32 i = 0; i < cpus; i++) after += cpus64[i].flushes;
    if (after - before != (u64)targets) return 0;

    k64_puts("HAKU64-SMP-TLB-OK: targets=");
    k64_dec(targets);
    k64_puts(" flushes=");
    k64_dec(after - before);
    k64_puts(" timeouts=0\r\n");
    return 1;
}

/* Hand-assembled ring-3 program:
 *
 *   mov eax, SYS_WRITE ; mov edi, 1 ; mov rsi, USER_DATA_ADDR
 *   mov edx, 5         ; int 0x80
 *   mov eax, SYS_EXIT  ; xor edi, edi ; int 0x80
 *   jmp $
 *
 * Writing it out byte by byte keeps this test free of a second toolchain:
 * the point is ring-3 entry and the raw INT 0x80 path with three arguments.
 * The ELF64 test below covers the six-argument ABI and the wider syscall
 * table. */
static u32 build_user_program(u8 *code) {
    u32 n = 0;
    u32 i;

    code[n++] = 0xB8;                 /* mov eax, imm32 */
    code[n++] = (u8)K64_SYS_WRITE;
    code[n++] = 0; code[n++] = 0; code[n++] = 0;
    code[n++] = 0xBF;                 /* mov edi, imm32 */
    code[n++] = 1;
    code[n++] = 0; code[n++] = 0; code[n++] = 0;
    code[n++] = 0x48; code[n++] = 0xBE;   /* mov rsi, imm64 */
    for (i = 0; i < 8; i++) code[n++] = (u8)(USER_DATA_ADDR >> (8 * i));
    code[n++] = 0xBA;                 /* mov edx, imm32 */
    code[n++] = 5;
    code[n++] = 0; code[n++] = 0; code[n++] = 0;
    code[n++] = 0xCD; code[n++] = 0x80;   /* int 0x80 */

    code[n++] = 0xB8;                 /* mov eax, imm32 */
    code[n++] = (u8)K64_SYS_EXIT;
    code[n++] = 0; code[n++] = 0; code[n++] = 0;
    code[n++] = 0x31; code[n++] = 0xFF;   /* xor edi, edi */
    code[n++] = 0xCD; code[n++] = 0x80;   /* int 0x80 */
    code[n++] = 0xEB; code[n++] = 0xFE;   /* jmp $ */
    return n;
}

static int test_user(void) {
    u64 space = paging64_create_space();
    u64 code = frame64_alloc();
    u64 data = frame64_alloc();
    u64 stack = frame64_alloc();
    u8 program[64];
    u32 length;
    int id;
    u64 deadline;

    if (!space || !code || !data || !stack) return 0;
    if (!paging64_map_space(space, USER_CODE_ADDR, code, 0, 1, 1)) return 0;
    if (!paging64_map_space(space, USER_DATA_ADDR, data, 1, 1, 0)) return 0;
    if (!paging64_map_space(space, USER_STACK_PAGE, stack, 1, 1, 0)) return 0;

    length = build_user_program(program);
    memcpy(phys_to_virt64(code), program, length);
    memcpy(phys_to_virt64(data), "RING3", 5);

    id = task64_create_user(USER_CODE_ADDR, USER_STACK_TOP, space);
    if (id < 0) return 0;

    deadline = timer64_ticks() + 200;
    while (task64_state(id) != K64_TASK_DONE && timer64_ticks() < deadline)
        cpu_pause64();
    if (task64_state(id) != K64_TASK_DONE) return 0;
    if (task64_exit_status(id) != 0) return 0;
    if (sys64_count() < 2) return 0;

    /* The task is done and the scheduler is back on the boot context, so
     * its space and pages can be released; the kernel stack stays with the
     * dead task slot for now. */
    paging64_destroy_space(space);
    frame64_free(code);
    frame64_free(data);
    frame64_free(stack);
    return 1;
}

/* --------------------------------------------------------- NX user fetch
 * EFER.NXE is on, data and stack pages are mapped with the NX bit, and a
 * ring-3 fetch from one must fault. The fault handler now terminates the
 * task instead of halting, so the boot context resumes and can inspect why
 * the task stopped. */
#define NX_CODE_ADDR  (K64_USER_BASE + 0x4000)
#define NX_DATA_ADDR  (K64_USER_BASE + 0x5000)
#define NX_STACK_PAGE (K64_USER_BASE + 0x6000)
#define NX_STACK_TOP  (K64_USER_BASE + 0x7000)

static int test_nx(void) {
    u64 space = paging64_create_space();
    u64 code = frame64_alloc();
    u64 data = frame64_alloc();
    u64 stack = frame64_alloc();
    u8 program[16];
    u32 length = 0;
    u32 faults_before = exc64_count(14);
    int id;
    u64 deadline;

    if (!space || !code || !data || !stack) return 0;
    if (!paging64_map_space(space, NX_CODE_ADDR, code, 0, 1, 1)) return 0;
    if (!paging64_map_space(space, NX_DATA_ADDR, data, 1, 1, 0)) return 0;
    if (!paging64_map_space(space, NX_STACK_PAGE, stack, 1, 1, 0)) return 0;

    program[length++] = 0x48;                 /* mov rax, imm64 */
    program[length++] = 0xB8;
    for (u32 i = 0; i < 8; i++)
        program[length++] = (u8)(NX_DATA_ADDR >> (8 * i));
    program[length++] = 0xFF;                 /* jmp rax */
    program[length++] = 0xE0;
    program[length++] = 0xEB;                 /* jmp $ */
    program[length++] = 0xFE;
    memcpy(phys_to_virt64(code), program, length);

    id = task64_create_user(NX_CODE_ADDR, NX_STACK_TOP, space);
    if (id < 0) return 0;

    deadline = timer64_ticks() + 200;
    while (task64_state(id) != K64_TASK_DONE && timer64_ticks() < deadline)
        cpu_pause64();
    if (task64_state(id) != K64_TASK_DONE) return 0;
    if (task64_fault_vector(id) != 14) return 0;
    if ((task64_fault_address(id) & K64_PAGE_MASK) != NX_DATA_ADDR) return 0;
    if (exc64_count(14) <= faults_before) return 0;

    k64_puts("HAKU64-NX-OK: user fetch faulted vector=14 addr=");
    k64_hex(task64_fault_address(id));
    k64_puts("\r\n");

    paging64_destroy_space(space);
    frame64_free(code);
    frame64_free(data);
    frame64_free(stack);
    return 1;
}

/* --------------------------------------------------------- ELF64 user image
 * The embedded executable comes from kernel/arch/x86_64/user/ring3_elf64.asm
 * through tools/mkelf64.py. Every rejection case starts from a fresh copy of
 * the valid image and changes one field. */
static int expect_reject64(u8 *image, u64 size, const char *what) {
    loaded_elf64_t loaded;

    if (elf64_load(image, size, &loaded)) {
        k64_puts("HAKU64-ELF-REJECT-FAIL: accepted ");
        k64_puts(what);
        k64_puts("\r\n");
        elf64_unload(&loaded);
        return 0;
    }
    if (memcmp(elf64_last_error(), "no error", 8) == 0) {
        k64_puts("HAKU64-ELF-REJECT-FAIL: no reason for ");
        k64_puts(what);
        k64_puts("\r\n");
        return 0;
    }
    return 1;
}

static int test_elf64_reject(void) {
    u64 size = (u64)(user64_image_end - user64_image);
    u8 *copy = (u8 *)kmalloc64(size);
    elf64_header_t good_header;
    elf64_phdr_t good_phdrs[2];
    elf64_header_t *header;
    elf64_phdr_t *phdrs;
    int ok = 1;

    if (!copy) return 0;
    memcpy(copy, user64_image, size);
    header = (elf64_header_t *)copy;
    if (header->phnum < 2) {
        kfree64(copy);
        return 0;
    }
    memcpy(&good_header, header, sizeof(good_header));
    memcpy(good_phdrs, copy + header->phoff, sizeof(good_phdrs));
    phdrs = (elf64_phdr_t *)(copy + header->phoff);

    header->ident[4] = 1;                  /* ELFCLASS32 */
    if (!expect_reject64(copy, size, "a 32-bit class")) ok = 0;
    memcpy(header, &good_header, sizeof(good_header));

    header->machine = 3;                   /* EM_386 */
    if (!expect_reject64(copy, size, "the wrong machine")) ok = 0;
    memcpy(header, &good_header, sizeof(good_header));

    phdrs[0].flags = 7;                    /* R+W+X */
    if (!expect_reject64(copy, size, "a writable executable segment")) ok = 0;
    memcpy(phdrs, good_phdrs, sizeof(good_phdrs));

    header->entry = K64_USER_STACK_TOP;    /* inside no segment */
    if (!expect_reject64(copy, size, "an entry outside the code")) ok = 0;
    memcpy(header, &good_header, sizeof(good_header));

    phdrs[0].offset = size;                /* past the end of the file */
    if (!expect_reject64(copy, size, "a segment past end of file")) ok = 0;
    memcpy(phdrs, good_phdrs, sizeof(good_phdrs));

    if (!expect_reject64(copy, 32, "a truncated header")) ok = 0;

    kfree64(copy);
    return ok;
}

/* The disk volume seeds two files: HELLO.TXT with the 16-byte marker and
 * PATTERN.BIN with three pages of a byte pattern. Reading both through the
 * FAT12 page cache proves the mount, the path walk and a multi-page file at
 * once; the 37/11 pattern is the same formula tools/mkfat12.py seeds. */
static u8 disk_pattern(u64 offset) {
    return (u8)((offset * 37u + 11u) & 0xFFu);
}

static int test_fs(void) {
    static const char marker[16] = "HAKU64-FILE-OK\r\n";
    char hello[16];
    u8 pattern[32];
    int file;

    file = fs64_lookup("/HELLO.TXT");
    if (file < 0 || fs64_size(file) != sizeof(hello)) return 0;
    if (fs64_read(file, 0, hello, sizeof(hello)) != sizeof(hello)) return 0;
    if (memcmp(hello, marker, sizeof(hello)) != 0) return 0;
    if (fs64_lookup("/missing.txt") >= 0) return 0;

    file = fs64_lookup("/PATTERN.BIN");
    if (file < 0 || fs64_pages(file) != 3) return 0;
    if (fs64_read(file, 0x1000, pattern, sizeof(pattern)) !=
        sizeof(pattern))
        return 0;
    for (u32 i = 0; i < sizeof(pattern); i++)
        if (pattern[i] != disk_pattern(0x1000 + i)) return 0;
    return 1;
}

/* The ring-3 program created /TMP and wrote NOTE.TXT, then closed it. A new
 * lookup therefore has to read the bytes back from the volume, not from a
 * descriptor that is still open: this is the disk-write proof. */
static int test_disk_note(void) {
    static const char expected[16] = "HAKU64-DISK-OK\r\n";
    char note[16];
    int file = fs64_lookup("/TMP/NOTE.TXT");

    if (file < 0 || fs64_size(file) != sizeof(note)) return 0;
    if (fs64_read(file, 0, note, sizeof(note)) != sizeof(note)) return 0;
    if (memcmp(note, expected, sizeof(note)) != 0) return 0;

    k64_puts("HAKU64-DISK-OK: /TMP/NOTE.TXT read back after close\r\n");
    return 1;
}

/* X64MOD.HKM is the packed x86-64 demo. Reading it from the volume and
 * loading it exercises the whole host-to-ring-0 path: CONTAINER header,
 * relocation of amd64 data and call sites, export lookup, then a call into
 * module code that logs, reads ticks and uses the heap. The duplicate-name,
 * bad-magic and truncated-size loads prove the loader rejects what it should
 * instead of trusting the file. */
static int test_module(void) {
    u8 *image;
    u64 size;
    int file;
    int ok = 1;

    file = fs64_lookup("/X64MOD.HKM");
    if (file < 0) return 0;
    size = fs64_size(file);
    if (size < HAKU_MODULE_HEADER_SIZE || size > HAKU_MODULE_MAX_FILE)
        return 0;
    image = (u8 *)kmalloc64(size);
    if (!image) return 0;
    if (fs64_read(file, 0, image, size) != size) {
        kfree64(image);
        return 0;
    }

    if (module64_load(image, (u32)size) != 0 || module64_count() != 1)
        ok = 0;
    else if (module64_load(image, (u32)size) != -HAKU_EEXIST)
        ok = 0;
    else if (module64_load(image, HAKU_MODULE_HEADER_SIZE - 1u) != -HAKU_EINVAL)
        ok = 0;
    else {
        image[0] ^= 0xFFu;
        if (module64_load(image, (u32)size) != -HAKU_EINVAL)
            ok = 0;
        image[0] ^= 0xFFu;
    }
    if (ok && module64_unload("HAKU-X64") != 0) ok = 0;
    if (ok && module64_count() != 0) ok = 0;
    if (ok && module64_unload("HAKU-X64") != -HAKU_ENOENT) ok = 0;

    kfree64(image);
    if (!ok) return 0;

    k64_puts("HAKU64-MODULE-REJECT-OK: duplicate, truncated and bad magic"
             " refused\r\n");
    return 1;
}

/* Copy bytes into a mapped user address space. Every page the caller names
 * must already be mapped; paging64_translate_space returns the physical
 * address including the in-page offset, so a chunk ends at the page edge. */
static int write_user64(u64 space, u64 address, const void *source, u64 size) {
    const u8 *bytes = (const u8 *)source;

    while (size) {
        u64 phys = paging64_translate_space(space, address);
        u64 chunk = K64_PAGE_SIZE - (address & (K64_PAGE_SIZE - 1));

        if (!phys) return 0;
        if (chunk > size) chunk = size;
        memcpy(phys_to_virt64(phys), bytes, chunk);
        bytes += chunk;
        address += chunk;
        size -= chunk;
    }
    return 1;
}

/* /INIT.ELF is the C program built against user/lib64. The kernel reads it
 * from the FAT12 volume, loads it through the ELF64 loader, puts argc/argv on
 * the stack elf64_load allocated and starts it at ring 3. Everything the
 * program prints after that comes from the user library: printf, malloc,
 * open/read/write, fork/wait4, mmap and clock_gettime. The marker
 * HAKU64-LIBC-OK is written by the program itself, not by this test.
 *
 * The stack words are the kernel-launched form of the execve layout:
 * argc, argv[0], NULL (argv terminator), NULL (empty envp). execve adds the
 * auxiliary vector; a program started by the kernel is not expected to need
 * one, and crt64.asm reads only argc/argv/envp.
 */
static int test_libc64(void) {
    static const char path[] = "/INIT.ELF";
    u64 words[4];
    u64 sp;
    u64 size;
    u64 deadline;
    u8 *image;
    int file, id;
    loaded_elf64_t loaded;

    file = fs64_lookup(path);
    if (file < 0) return 0;
    size = fs64_size(file);
    if (size == 0 || size > 0x20000u) return 0;
    image = (u8 *)kmalloc64(size);
    if (!image) return 0;
    if (fs64_read(file, 0, image, size) != size) {
        kfree64(image);
        return 0;
    }
    if (!elf64_load(image, size, &loaded)) {
        k64_puts("HAKU64-LIBC-FAIL: ");
        k64_puts(elf64_last_error());
        k64_puts("\r\n");
        kfree64(image);
        return 0;
    }
    kfree64(image);

    sp = loaded.stack_top - 64;
    words[0] = 1;                    /* argc */
    words[1] = sp + 32;              /* argv[0] points at the string above */
    words[2] = 0;                    /* argv terminator */
    words[3] = 0;                    /* envp terminator */
    if (!write_user64(loaded.space, sp, words, sizeof(words)) ||
        !write_user64(loaded.space, sp + 32, path, sizeof(path))) {
        elf64_unload(&loaded);
        return 0;
    }

    id = task64_create_user(loaded.entry, sp, loaded.space);
    if (id < 0) {
        elf64_unload(&loaded);
        return 0;
    }

    deadline = timer64_ticks() + 400;
    while (task64_state(id) != K64_TASK_DONE && timer64_ticks() < deadline)
        cpu_pause64();
    if (task64_state(id) != K64_TASK_DONE) {
        k64_puts("HAKU64-LIBC-FAIL: /INIT.ELF did not finish\r\n");
        elf64_unload(&loaded);
        return 0;
    }
    if (task64_exit_status(id) != 0) {
        k64_puts("HAKU64-LIBC-FAIL: /INIT.ELF exit=");
        k64_dec(task64_exit_status(id));
        k64_puts("\r\n");
        elf64_unload(&loaded);
        return 0;
    }

    k64_puts("HAKU64-LIBC-RUN-OK: /INIT.ELF exit=0\r\n");
    elf64_unload(&loaded);
    return 1;
}

static int test_elf64(void) {
    loaded_elf64_t loaded;
    u64 size = (u64)(user64_image_end - user64_image);
    u64 before = sys64_count();
    u64 deadline;
    u64 phys;
    int id;

    if (!elf64_load(user64_image, size, &loaded)) {
        k64_puts("HAKU64-ELF-FAIL: ");
        k64_puts(elf64_last_error());
        k64_puts("\r\n");
        return 0;
    }
    if (loaded.entry == 0 || loaded.writable == 0 ||
        loaded.image_high <= loaded.image_low) {
        elf64_unload(&loaded);
        return 0;
    }

    /* The read-write segment's BSS tail must be zero before ring 3 runs. */
    phys = paging64_translate_space(loaded.space, loaded.writable);
    if (!phys || *(const volatile u32 *)phys_to_virt64(phys) != 0) {
        elf64_unload(&loaded);
        return 0;
    }

    k64_puts("HAKU64-ELF-OK: entry=");
    k64_hex(loaded.entry);
    k64_puts(" pages=");
    k64_dec(loaded.pages);
    k64_puts(" bss=");
    k64_hex(loaded.writable);
    k64_puts("\r\n");

    id = task64_create_user(loaded.entry, loaded.stack_top, loaded.space);
    if (id < 0) {
        elf64_unload(&loaded);
        return 0;
    }

    deadline = timer64_ticks() + 200;
    while (task64_state(id) != K64_TASK_DONE && timer64_ticks() < deadline)
        cpu_pause64();
    if (task64_state(id) != K64_TASK_DONE) return 0;
    if (task64_exit_status(id) != 0) {
        k64_puts("HAKU64-ELF-RUN-FAIL: exit=");
        k64_dec(task64_exit_status(id));
        k64_puts("\r\n");
        elf64_unload(&loaded);
        return 0;
    }

    /* The program makes more than a dozen calls: the success paths for
     * write, writev, getpid, clock_gettime, open/read/close/lseek, mmap,
     * munmap, mkdir, fork, wait4, clone and futex, plus the error matrix.
     * The bump pointer advanced by the two initial pages, the one-page
     * MAP_SHARED anonymous mapping, the three file mappings, the page the
     * fork test shares, the copy-on-write page and the two-page thread
     * stack (MAP_FIXED does not bump), which proves the mapping tests ran
     * even though munmap released the pages again. */
    if (sys64_count() < before + 10 ||
        task64_mmap_base(id) != K64_USER_MMAP_BASE + 10 * K64_PAGE_SIZE) {
        elf64_unload(&loaded);
        return 0;
    }

    /* The magic word is only in the image's BSS page if the program
     * actually reached the store, so it proves ring-3 execution rather
     * than a successful mapping. */
    phys = paging64_translate_space(loaded.space, loaded.writable);
    if (!phys || *(const volatile u32 *)phys_to_virt64(phys) != 0x48414B55u) {
        elf64_unload(&loaded);
        return 0;
    }

    k64_puts("HAKU64-ELF-RUN-OK: syscalls=");
    k64_dec(sys64_count() - before);
    k64_puts(" exit=0 bss=0x48414B55\r\n");

    /* The task is done and the scheduler is on the boot context again, so
     * its frames and address space can be released. Its kernel stack stays
     * with the dead task slot, like the ring-3 test above. */
    elf64_unload(&loaded);
    return 1;
}

void kernel64_main(void) {
    u64 top;
    u64 highest;

    k64_puts("HAKU64-BOOT-OK: long mode, 64-bit C\r\n");
    report_e820();

    top = e820_top();
    if (top == 0) k64_fail("no usable RAM in the E820 map");
    if (top < K64_RESERVED_END) k64_fail("less than 32 MiB of RAM");

    bootmem64_init();
    if (!paging64_init(top)) k64_fail("no boot memory for the physmap");
    /* The per-CPU block only needs a CR3 (it records the active space), and
     * everything from the frame allocator onwards takes locks, so it is
     * installed before those. Its self-test waits for the IDT: it turns
     * interrupts on to check that a lock restores them. */
    percpu64_init_bsp(0);
    k64_puts("HAKU64-PAGING-OK: physmap=");
    k64_dec(paging64_phys_map_bytes() >> 20);
    k64_puts("MiB at ");
    k64_hex(K64_PHYS_MAP_BASE);
    k64_puts("\r\n");

    if (!frame64_init(top)) k64_fail("no boot memory for the frame bitmaps");
    highest = frame64_highest_free();
    k64_puts("HAKU64-FRAME-OK: limit=");
    k64_dec(frame64_limit() >> 20);
    k64_puts("MiB usable=");
    k64_dec(frame64_usable_count());
    k64_puts(" free=");
    k64_dec(frame64_free_count());
    k64_puts(" top=");
    k64_hex(highest);
    k64_puts("\r\n");

    if (!heap64_init()) k64_fail("heap window rejected");
    /* The screen only becomes usable now: mapping the VBE aperture needs the
     * page tables, and reserving it needs the frame allocator. Every byte
     * printed from here on is teed to COM1 and to the display. */
    console64_init();
    k64_puts("HAKU64-HEAP-OK: window=");
    k64_dec(K64_HEAP_SIZE >> 20);
    k64_puts("MiB free=");
    k64_dec(heap64_free_bytes());
    k64_puts("\r\n");

    if (!test_physmap()) k64_fail("physmap does not alias physical memory");
    if (!test_frames()) k64_fail("frame allocator self-test");
    if (!test_paging()) k64_fail("4 KiB mapping self-test");
    if (!test_heap()) k64_fail("heap self-test");

    if (!fs64_init()) k64_fail("FAT12 disk mount");
    if (!test_fs()) k64_fail("FAT12 file self-test");
    k64_puts("HAKU64-FS-OK: hello=");
    k64_dec(fs64_size(fs64_lookup("/HELLO.TXT")));
    k64_puts(" pattern=");
    k64_dec(fs64_size(fs64_lookup("/PATTERN.BIN")));
    k64_puts("\r\n");

    gdt64_init();
    k64_puts("HAKU64-GDT-OK: tss=");
    k64_hex(gdt64_tss_base());
    k64_puts(" ist1=");
    k64_hex(gdt64_ist_top(1));
    k64_puts(" ist2=");
    k64_hex(gdt64_ist_top(2));
    k64_puts("\r\n");

    syscall64_init();

    idt64_init();
    if (!test_idt_table()) k64_fail("IDT layout self-test");
    pic64_remap(K64_IRQ_BASE, K64_IRQ_BASE + 8);
    pic64_mask_all();
    pic64_clear_mask(0);        /* timer first: it is the boot timebase */
    timer64_init(100);
    /* PS/2 keyboard: drains the controller, unmasks IRQ1 and proves the
     * set-1 decoder on synthesised scancodes. Interrupts are still off, so
     * the self-test's queue cannot race a real key. */
    kbd64_init();
    if (!kbd64_self_test()) k64_fail("PS/2 keyboard decode self-test");
    k64_puts("HAKU64-IDT-OK: gates=256 ist=df/nmi irq0=");
    k64_dec(K64_IRQ_BASE);
    k64_puts(" hz=");
    k64_dec(timer64_hz());
    k64_puts("\r\n");

    /* ACPI, the local APIC and the application processors. Interrupts are
     * still off here: the APs come online parked and the bootstrap
     * processor keeps the 8259 timer it just programmed. */
    smp64_init();
    sti64();

    if (!test_percpu()) k64_fail("per-CPU block self-test");
    if (!test_exceptions()) k64_fail("exception dispatch self-test");
    k64_puts("HAKU64-EXC-OK: int3, page fault and divide error resumed\r\n");

    if (!test_timer()) k64_fail("timer self-test");
    k64_puts("HAKU64-TIMER-OK: ticks=");
    k64_dec(timer64_ticks());
    k64_puts("\r\n");

    if (!rtc64_init()) k64_fail("CMOS real-time clock unreadable");
    if (!test_rtc()) k64_fail("real-time clock self-test");
    k64_puts("HAKU64-RTC-OK: epoch=");
    k64_dec(rtc64_epoch());
    k64_puts(" realtime=");
    k64_dec(rtc64_realtime(timer64_ticks(), timer64_hz()));
    k64_puts("\r\n");

    if (!test_spaces()) k64_fail("per-process page table self-test");
    k64_puts("HAKU64-VM-OK: virt=");
    k64_hex(K64_USER_BASE + 0x100000);
    k64_puts(" -> two frames across CR3 switches\r\n");

    if (!test_tasks()) k64_fail("64-bit context switch self-test");
    k64_puts("HAKU64-PROC-OK: hits=");
    k64_dec(task_hits[0]);
    k64_putc('/');
    k64_dec(task_hits[1]);
    k64_puts(" switches=");
    k64_dec(sched64_switches());
    k64_puts(" stacks=distinct\r\n");

    if (!test_smp_sched()) k64_fail("per-CPU scheduler self-test");
    if (!test_smp_tlb()) k64_fail("TLB shootdown self-test");

    if (!test_user()) k64_fail("ring-3 syscall self-test");
    k64_puts("HAKU64-USER-DONE: syscalls=");
    k64_dec(sys64_count());
    k64_puts(" state=done\r\n");

    if (!test_nx()) k64_fail("NX user-fetch self-test");

    if (!test_elf64_reject()) k64_fail("ELF64 loader accepted a malformed"
                                       " image");
    k64_puts("HAKU64-ELF-REJECT-OK: six malformed images refused\r\n");

    if (!test_elf64()) k64_fail("ELF64 loader self-test");
    if (!test_libc64()) k64_fail("x86-64 user library self-test");
    if (!test_disk_note()) k64_fail("FAT12 disk write self-test");
    if (!test_module()) k64_fail("x86-64 module loader self-test");

    k64_puts("HAKU64-SELFTEST-OK: physmap, frames, 4 KiB mapping, heap, "
             "FAT12 disk, exceptions, timer, RTC, spaces, tasks, ring 3, "
             "syscalls, fork/wait, NX, ELF64, modules\r\n");
    k64_puts("HAKU64-READY: interrupts online, 32-bit kernel still boots "
             "the system\r\n");
    for (;;) cpu_halt64();
}
