/* Haku x86-64 per-CPU state.
 *
 * Before this file the 64-bit port had exactly one of everything: one
 * scheduler cursor, one TSS.RSP0 mirror, one `current`. That is why only the
 * bootstrap processor ran tasks while the application processors parked. The
 * fix is the same one every x86 kernel makes: a block per CPU, found through
 * GS.base, so a plain gs:[offset] load is "this CPU" and no lookup is needed.
 *
 * GS is not simply loaded with the block's address once. Kernel memory is
 * mapped in *every* address space (the identity map in PML4 slot 0 and the
 * physmap above it), so a ring-3 program that kept the kernel GS base could
 * write through gs:[off] into kernel memory. The invariant therefore is:
 *
 *   ring 0: GS.base = this block,  IA32_KERNEL_GS_BASE = user value (0)
 *   ring 3: GS.base = 0,           IA32_KERNEL_GS_BASE = this block
 *
 * and every privilege transition swaps the two. isr64.asm does that for
 * traps (SWAPGS only when the interrupted frame says ring 3) and for SYSCALL;
 * this file only establishes the invariant for a CPU and hands out the block
 * pointer.
 *
 * A task frame that moves between CPUs would carry a stale GS base, but
 * tasks are pinned: `task64_t.cpu` is set when the task is created and never
 * changed, so a frame always resumes on the CPU that saved it.
 */
#include "haku64.h"

/* Storage for the blocks. They must not share a cache line with each other,
 * so each block starts at a 64-byte boundary, and the struct is padded to 128
 * bytes so consecutive blocks stay aligned. MSVC gives a 64-byte-aligned
 * static its own section alignment, which /ALIGN:16 (the flat image the boot
 * loader expects) rejects, so the storage is only 16-byte aligned here and
 * percpu_align() rounds the first block up once, on the bootstrap processor,
 * before any block is used. */
__declspec(align(16)) static u8 percpu_storage[K64_SMP_MAX_CPUS *
                                             sizeof(k64_percpu_t) + 64];
k64_percpu_t *cpus64;

static void percpu_align(void) {
    if (!cpus64)
        cpus64 = (k64_percpu_t *)(((u64)percpu_storage + 63ull) & ~63ull);
}

/* Install `index`'s block and point GS at it. Runs on the CPU being
 * described, so the MSR write takes effect on the correct unit. */
static void percpu_install(u32 index, u32 apic_id) {
    k64_percpu_t *cpu;

    percpu_align();
    cpu = &cpus64[index];

    memset(cpu, 0, sizeof(*cpu));
    cpu->self = (u64)cpu;
    cpu->index = index;
    cpu->apic_id = apic_id;
    cpu->current = (u32)-1;      /* nothing scheduled yet */
    cpu->idle = (u32)-1;         /* the idle task does not exist yet */
    cpu->online = 1;
    cpu->active_space = read_cr3_64() & K64_PAGE_MASK;

    write_msr64(K64_MSR_GS_BASE, (u64)cpu);
    write_msr64(K64_MSR_KERNEL_GS_BASE, 0);
}

void percpu64_init_bsp(u32 apic_id) {
    percpu_install(0, apic_id);
}

void percpu64_init_ap(u32 index, u32 apic_id) {
    if (index == 0 || index >= K64_SMP_MAX_CPUS) return;
    percpu_install(index, apic_id);
}

/* The bootstrap processor's block is installed before the local APIC is
 * discovered, so its APIC id arrives here once smp64_init() knows it. */
void percpu64_set_apic_id(u32 index, u32 apic_id) {
    if (index < K64_SMP_MAX_CPUS) cpus64[index].apic_id = apic_id;
}

u32 k64_cpu_index(void) {
    k64_percpu_t *cpu = k64_cpu();
    return cpu ? cpu->index : 0;
}

/* The assembly hardcodes 8 and 16 for rsp0 and user_rsp. If a field is ever
 * inserted in front of them this returns 0 and the boot test fails instead of
 * switching a system call onto the wrong stack. */
int percpu64_offsets_ok(void) {
    k64_percpu_t probe;
    u64 rsp0 = (u64)((u8 *)&probe.rsp0 - (u8 *)&probe);
    u64 user_rsp = (u64)((u8 *)&probe.user_rsp - (u8 *)&probe);
    u64 asm_offsets = isr64_percpu_offsets();

    /* The stride has to stay a multiple of the cache-line alignment or
     * consecutive CPUs would share lines; 128 is also what the layout comment
     * in haku64.h claims. */
    if (sizeof(k64_percpu_t) % 64u) return 0;
    return rsp0 == (asm_offsets >> 32) &&
           user_rsp == (asm_offsets & 0xFFFFFFFFu);
}

/* --------------------------------------------------------------- spin locks
 * Interrupts are part of the lock, not a separate concern: a lock held across
 * a timer interrupt would let the same CPU re-enter the critical section
 * while the holder is descheduled, and the spinner would never let it run.
 * Saving RFLAGS instead of blindly re-enabling IF keeps the lock usable from
 * a system call (IF already off) and from the boot tests (IF on). */
u64 spin64_lock(spinlock64_t *lock) {
    u64 flags = read_rflags64();

    cli64();
    while (cpu_atomic_exchange64(&lock->locked, 1u)) {
        /* Read-only spin: the exchange already claimed the line, and a plain
         * load does not keep dirtying it between retries. */
        while (lock->locked) cpu_pause64();
    }
    return flags;
}

void spin64_unlock(spinlock64_t *lock, u64 flags) {
    /* A locked exchange, not a plain store: it is a full barrier, so every
     * store made inside the critical section is globally visible before the
     * next CPU can see the lock free. */
    cpu_atomic_exchange64(&lock->locked, 0u);
    write_rflags64(flags);
}
