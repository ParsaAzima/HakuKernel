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

/* The table layout, as opposed to "an interrupt happened": every gate is a
 * DPL-0 interrupt gate except the system call vector, #DF and NMI get their
 * own IST stacks, the rest share the interrupted stack, each vector has its
 * own stub, and the unused vectors all fall through to the catch-all. */
static int test_idt_table(void) {
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

/* An address in PML4 slot 384: canonical (bits 63..48 sign-extend bit 47)
 * and, unlike the physmap's slot 256, never populated. */
#define K64_UNMAPPED_PROBE 0xFFFFC00000000000ull

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

/* The store must return exactly the bytes each file advertises, because the
 * mmap path copies and aliases those same frames. */
static int test_fs(void) {
    static const char marker[16] = "HAKU64-FILE-OK\r\n";
    char hello[16];
    u8 pattern[32];
    int file;

    file = fs64_open("/hello.txt");
    if (file < 0 || fs64_size(file) != sizeof(hello)) return 0;
    if (fs64_read(file, 0, hello, sizeof(hello)) != sizeof(hello)) return 0;
    if (memcmp(hello, marker, sizeof(hello)) != 0) return 0;
    if (fs64_open("/missing") >= 0) return 0;

    file = fs64_open("/pattern.bin");
    if (file < 0 || fs64_pages(file) != 3) return 0;
    if (fs64_read(file, 0x1000, pattern, sizeof(pattern)) !=
        sizeof(pattern))
        return 0;
    for (u32 i = 0; i < sizeof(pattern); i++)
        if (pattern[i] != fs64_pattern_byte(0x1000 + i)) return 0;
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
     * write, writev, getpid, clock_gettime, open/read/close/lseek, mmap and
     * munmap, plus the error matrix. The bump pointer advanced by the two
     * initial pages, the one-page MAP_SHARED anonymous mapping and the three
     * file mappings (MAP_FIXED does not bump), which proves the mapping
     * tests ran even though munmap released the pages again. */
    if (sys64_count() < before + 10 ||
        task64_mmap_base(id) != K64_USER_MMAP_BASE + 6 * K64_PAGE_SIZE) {
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
    k64_puts("HAKU64-HEAP-OK: window=");
    k64_dec(K64_HEAP_SIZE >> 20);
    k64_puts("MiB free=");
    k64_dec(heap64_free_bytes());
    k64_puts("\r\n");

    if (!test_physmap()) k64_fail("physmap does not alias physical memory");
    if (!test_frames()) k64_fail("frame allocator self-test");
    if (!test_paging()) k64_fail("4 KiB mapping self-test");
    if (!test_heap()) k64_fail("heap self-test");

    if (!fs64_init()) k64_fail("in-memory file store");
    if (!test_fs()) k64_fail("file store self-test");
    k64_puts("HAKU64-FS-OK: files=");
    k64_dec(fs64_file_count());
    k64_puts(" hello=");
    k64_dec(fs64_size(fs64_open("/hello.txt")));
    k64_puts(" pattern=");
    k64_dec(fs64_size(fs64_open("/pattern.bin")));
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
    pic64_clear_mask(0);        /* only the timer is unmasked so far */
    timer64_init(100);
    k64_puts("HAKU64-IDT-OK: gates=256 ist=df/nmi irq0=");
    k64_dec(K64_IRQ_BASE);
    k64_puts(" hz=");
    k64_dec(timer64_hz());
    k64_puts("\r\n");
    sti64();

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

    if (!test_user()) k64_fail("ring-3 syscall self-test");
    k64_puts("HAKU64-USER-DONE: syscalls=");
    k64_dec(sys64_count());
    k64_puts(" state=done\r\n");

    if (!test_nx()) k64_fail("NX user-fetch self-test");

    if (!test_elf64_reject()) k64_fail("ELF64 loader accepted a malformed"
                                       " image");
    k64_puts("HAKU64-ELF-REJECT-OK: six malformed images refused\r\n");

    if (!test_elf64()) k64_fail("ELF64 loader self-test");

    k64_puts("HAKU64-SELFTEST-OK: physmap, frames, 4 KiB mapping, heap, "
             "files, exceptions, timer, RTC, spaces, tasks, ring 3, syscalls, "
             "NX, ELF64\r\n");
    k64_puts("HAKU64-READY: interrupts online, 32-bit kernel still boots "
             "the system\r\n");
    for (;;) cpu_halt64();
}
