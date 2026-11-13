/* Haku x86-64 SMP bring-up, IPIs and TLB shootdown.
 *
 * The bring-up itself is the same as stage B: find the CPUs through ACPI,
 * enable the local APIC, copy the trampoline to 0x8000 and start every other
 * processor with INIT-SIPI-SIPI. Each AP walks the trampoline through real
 * mode, protected mode with PAE and long mode, then calls smp64_ap_entry().
 *
 * What changed is what the AP does next. It installs its own per-CPU block
 * (percpu64.c) so gs:[off] means "this CPU", loads its own GDT/TSS and the
 * shared IDT, enables its own local APIC, and then *parks*: the addresses
 * that interrupts and the scheduler use are all shared statics, so an AP
 * must not start scheduling until the bootstrap processor has finished the
 * single-CPU boot tests and built that CPU's idle task. Parked APs spin with
 * interrupts off; smp64_release_aps() finishes the job.
 *
 * The two IPIs are the parts the scheduler and the MMU need from each other:
 * K64_VEC_RESCHED makes an AP look at the task table now instead of at its
 * next tick, and K64_VEC_TLB makes it drop stale translations after this CPU
 * changed a page table.
 */
#include "acpi.h"
#include "apic.h"
#include "haku64.h"

#define SMP64_TRAMPOLINE_BASE 0x8000ull
#define SMP64_TRAMPOLINE_VECTOR 0x08u
/* Parameter block at the top of the trampoline page; must match
 * ap_trampoline64.asm. Four 64-bit slots, above the real-mode stack. */
#define SMP64_PARAM_BASE 0x8F00ull

#define SMP64_START_ATTEMPTS 3u
#define SMP64_ONLINE_SPINS 200000u
/* Increments each CPU contributes to the cross-CPU lock test; see the 32-bit
 * smp.c for why the counter update itself is deliberately not atomic. */
#define SMP64_LOCK_ITERATIONS 50000u
/* Bound on the "wait for every target to acknowledge" loop. An AP is in a
 * short critical section at worst (system calls run with interrupts off), so
 * a round trip is microseconds; the cap only exists so a wedged CPU cannot
 * hang the machine. */
#define SMP64_SHOOTDOWN_SPINS 2000000u

typedef struct {
    u32 apic_id;
    u64 stack_phys;
    u64 stack_top;
    volatile u32 online;
    volatile u32 done;
    volatile u32 gdt_ok;
} smp64_cpu_t;

static smp64_cpu_t cpus[K64_SMP_MAX_CPUS];
static u32 cpu_count;

/* The cross-CPU lock test. A count of increments under a lock must equal
 * iterations x CPUs; a plain increment would lose updates. spinlock64_t is
 * also what the scheduler and the allocators use, so the test exercises the
 * real primitive rather than a stand-in. */
static spinlock64_t lock_test = SPINLOCK64_INIT;
static volatile u32 lock_counter;

/* Set by the bootstrap processor once every AP's idle task exists. An AP
 * spins on it, so it must be volatile and stores to it must be ordered
 * before the release is observed. */
static volatile u32 released;

/* One shootdown at a time. Two concurrent rounds would overwrite each
 * other's `tlb_target`, so the lock is what makes the sequence numbers
 * meaningful. */
static spinlock64_t tlb_lock = SPINLOCK64_INIT;
static volatile u32 tlb_round;
static volatile u32 tlb_timeouts;

/* From ap_trampoline64.asm. The blob is copied, not executed in place. */
extern void ap_trampoline64_start(void);
extern void ap_trampoline64_end(void);
extern void smp_memory_barrier64(void);

/* Running on the AP's private stack with the trampoline's bootstrap GDT.
 * The per-CPU block comes first because everything after it may look at
 * "this CPU"; nothing here touches shared scheduler state until
 * sched64_start_ap(), which only runs after the release. */
void smp64_ap_entry(u64 cpu) {
    u32 index = (u32)cpu;
    u32 i;

    if (index < K64_SMP_MAX_CPUS) {
        percpu64_init_ap(index, cpus[index].apic_id);
        /* Switch from the trampoline's bootstrap table to this CPU's own
         * GDT/TSS and the shared IDT before doing anything else. */
        if (gdt64_load_ap(index)) {
            idt64_load_ap();
            cpus[index].gdt_ok = 1;
        }
        /* APICBASE is per CPU: the unit the bootstrap processor enabled is
         * not the one this CPU will receive its timer and IPIs on. */
        apic_enable_local();
        cpus[index].online = 1;
    }

    for (i = 0; i < SMP64_LOCK_ITERATIONS; i++) {
        u64 flags = spin64_lock(&lock_test);
        lock_counter++;
        spin64_unlock(&lock_test, flags);
    }

    if (index < K64_SMP_MAX_CPUS) cpus[index].done = 1;

    /* Park until the bootstrap processor has built this CPU's idle task.
     * Spinning (rather than HLT) keeps the release a plain flag: sleeping
     * here would need the flag test inside the interrupt path, and an AP has
     * nothing else to wake it for. The window is the boot tests, seconds at
     * most, and this core is otherwise idle. */
    for (;;) {
        if (released) sched64_start_ap(index);
        cpu_pause64();
    }
}

static int start_ap(u32 index, u32 apic_id) {
    volatile u64 *params = (volatile u64 *)SMP64_PARAM_BASE;
    u64 blob_size = (u64)ap_trampoline64_end - (u64)ap_trampoline64_start;
    u64 stack_phys = frame64_alloc();
    u64 stack_top;
    u32 attempt;
    u32 spin;

    if (!stack_phys) return 0;
    cpus[index].apic_id = apic_id;
    cpus[index].stack_phys = stack_phys;
    stack_top = (u64)phys_to_virt64(stack_phys) + K64_PAGE_SIZE;
    cpus[index].stack_top = stack_top;
    /* Build the descriptor table here, while only the BSP uses the frame
     * allocator; the AP only loads it. */
    if (!gdt64_prepare_ap(index, stack_top)) {
        frame64_free(stack_phys);
        cpus[index].stack_phys = 0;
        return 0;
    }

    memcpy((void *)SMP64_TRAMPOLINE_BASE, (void *)ap_trampoline64_start,
           blob_size);
    params[0] = read_cr3_64() & K64_PAGE_MASK;  /* BSP's PML4 */
    params[1] = stack_top;
    params[2] = index;
    params[3] = (u64)smp64_ap_entry;

    for (attempt = 0; attempt < SMP64_START_ATTEMPTS; attempt++) {
        /* Stores to the trampoline page and the parameter block must be
         * globally visible before the INIT/SIPI sequence; mfence is the
         * x64 equivalent of the 32-bit build's barrier. */
        smp_memory_barrier64();
        apic_send_init(apic_id);
        apic_delay(2000);
        apic_send_sipi(apic_id, SMP64_TRAMPOLINE_VECTOR);
        apic_delay(2000);
        apic_send_sipi(apic_id, SMP64_TRAMPOLINE_VECTOR);
        for (spin = 0; spin < SMP64_ONLINE_SPINS; spin++) {
            if (cpus[index].online) return 1;
            apic_delay(10);
        }
    }
    return 0;
}

int smp64_init(void) {
    const acpi_info_t *info;
    u32 bsp_id;
    u32 i;
    u32 spin;
    u32 expected;

    memset(cpus, 0, sizeof(cpus));
    cpu_count = 0;
    lock_counter = 0;
    lock_test.locked = 0;
    released = 0;
    tlb_round = 0;
    tlb_timeouts = 0;

    if (!acpi_init() || !acpi_info()->have_madt) {
        k64_puts("HAKU64-SMP-SKIP: no ACPI MADT\r\n");
        return 0;
    }
    info = acpi_info();
    if (!apic_init()) {
        k64_puts("HAKU64-SMP-SKIP: local APIC did not map\r\n");
        return 0;
    }

    bsp_id = apic_id();
    percpu64_set_apic_id(0, bsp_id);
    cpus[0].apic_id = bsp_id;
    cpus[0].online = 1;
    cpu_count = 1;

    /* One pass over the MADT entries; the table is tiny and this runs once. */
    for (i = 0; i < info->cpu_count && cpu_count < K64_SMP_MAX_CPUS; i++) {
        u32 id = info->cpus[i].apic_id;
        if (!info->cpus[i].enabled || id == bsp_id) continue;
        if (start_ap(cpu_count, id)) {
            k64_puts("HAKU64-SMP-AP-OK cpu=");
            k64_dec(cpu_count);
            k64_puts(" apic=");
            k64_dec(id);
            k64_puts("\r\n");
            cpu_count++;
        } else {
            k64_puts("HAKU64-SMP-AP-FAIL apic=");
            k64_dec(id);
            k64_puts("\r\n");
        }
    }

    k64_puts("HAKU64-SMP-OK: cpus=");
    k64_dec(cpu_count);
    k64_puts(" lapic=");
    k64_hex(info->lapic_phys);
    k64_puts(" madt=");
    k64_dec(info->cpu_count);
    k64_puts(" parked=1\r\n");

    /* The BSP takes the same share as each AP while the APs run theirs, so
     * the counter update has to be protected by a lock that is atomic across
     * CPUs, not by a local interrupt mask. */
    for (i = 0; i < SMP64_LOCK_ITERATIONS; i++) {
        u64 flags = spin64_lock(&lock_test);
        lock_counter++;
        spin64_unlock(&lock_test, flags);
    }
    for (i = 1; i < cpu_count; i++) {
        for (spin = 0; spin < SMP64_ONLINE_SPINS; spin++) {
            if (cpus[i].done) break;
            apic_delay(10);
        }
    }

    /* Each AP switched to its own GDT/TSS: prove the TSS carries that CPU's
     * stack, which is what a ring-3 entry would use. */
    for (i = 1; i < cpu_count; i++) {
        if (cpus[i].gdt_ok && gdt64_ap_rsp0(i) == cpus[i].stack_top) {
            k64_puts("HAKU64-SMP-AP-GDT-OK cpu=");
        } else {
            k64_puts("HAKU64-SMP-AP-GDT-FAIL cpu=");
        }
        k64_dec(i);
        k64_puts(" rsp0=");
        k64_hex(gdt64_ap_rsp0(i));
        k64_puts("\r\n");
    }

    expected = SMP64_LOCK_ITERATIONS * cpu_count;
    if (lock_counter == expected) {
        k64_puts("HAKU64-SMP-LOCK-OK counter=");
    } else {
        k64_puts("HAKU64-SMP-LOCK-FAIL counter=");
    }
    k64_dec(lock_counter);
    k64_puts(" expected=");
    k64_dec(expected);
    k64_puts("\r\n");
    return (int)cpu_count;
}

u32 smp64_cpu_count(void) {
    return cpu_count;
}

/* Give every AP its idle task and let it go. Runs on the bootstrap processor
 * after sched64_init(), which is what makes the wait worth it: the task table
 * and the heap are fully set up, so the idle task can be built here and the
 * AP only has to adopt it. Returns how many APs were prepared. */
int smp64_release_aps(void) {
    int started = 0;

    for (u32 i = 1; i < cpu_count; i++)
        if (sched64_prepare_ap(i) >= 0) started++;

    /* The flag must not become visible before the task records it refers
     * to. */
    smp_memory_barrier64();
    released = 1;
    smp_memory_barrier64();
    return started;
}

void smp64_resched_cpu(u32 cpu) {
    if (cpu >= K64_SMP_MAX_CPUS) return;
    if (!cpus64[cpu].online) return;
    if (cpu == k64_cpu_index()) return;     /* the caller is about to return */
    apic_send_ipi(cpus[cpu].apic_id, K64_VEC_RESCHED);
}

/* The TLB half of cross-CPU coherence. Without PCIDs a CR3 write flushes the
 * whole TLB, so a CPU only holds stale entries for a space it has actually
 * had loaded: `active_space` is that set, and a shootdown for a space no
 * other CPU ran costs nothing. `space == 0` means "whoever you are", which is
 * what a full teardown or the self-test wants.
 *
 * Called with the paging lock held by nobody in particular, but the caller
 * may have interrupts off; targets are in short critical sections at worst.
 * The wait is bounded and a timeout is counted rather than ignored. */
u32 smp64_tlb_shootdown(u64 space) {
    u32 me = k64_cpu_index();
    u32 targets = 0;
    u32 seq;
    u32 spin;
    u64 flags;

    flags = spin64_lock(&tlb_lock);
    seq = ++tlb_round;

    for (u32 i = 0; i < cpu_count; i++) {
        if (i == me) continue;
        if (!cpus64[i].online) continue;
        if (space && cpus64[i].active_space != space) continue;
        cpus64[i].tlb_ack = 0;
        cpus64[i].tlb_target = seq;
        targets++;
    }
    /* Arm every target before the first IPI: an AP that acks immediately
     * would otherwise race the next target's sequence number. */
    for (u32 i = 0; i < cpu_count; i++) {
        if (i == me || cpus64[i].tlb_target != seq) continue;
        apic_send_ipi(cpus64[i].apic_id, K64_VEC_TLB);
    }

    for (spin = 0; spin < SMP64_SHOOTDOWN_SPINS; spin++) {
        u32 pending = 0;

        for (u32 i = 0; i < cpu_count; i++) {
            if (i == me) continue;
            if (cpus64[i].tlb_target != seq) continue;
            if (cpus64[i].tlb_ack != seq) pending++;
        }
        if (!pending) break;
        cpu_pause64();
    }
    for (u32 i = 0; i < cpu_count; i++) {
        if (i == me) continue;
        if (cpus64[i].tlb_target != seq) continue;
        if (cpus64[i].tlb_ack != seq) tlb_timeouts++;
    }

    spin64_unlock(&tlb_lock, flags);
    return targets;
}

/* Runs on the CPU the IPI landed on: drop every translation, then report
 * that the round is done. A CR3 reload is the whole flush because this port
 * does not use PCIDs. */
void smp64_tlb_ipi(void) {
    k64_percpu_t *cpu = k64_cpu();

    write_cr3_64(read_cr3_64());
    if (!cpu) return;
    cpu->flushes++;
    cpu->tlb_ack = cpu->tlb_target;
}

u32 smp64_tlb_timeouts(void) {
    return tlb_timeouts;
}
