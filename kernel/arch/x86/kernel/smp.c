/* Haku SMP bring-up, IPIs and TLB shootdown (32-bit).
 *
 * The bring-up is the same shape as the 64-bit port: find the processors the
 * firmware describes, enable the local APIC, copy the trampoline to 0x8000
 * and start every other processor with INIT-SIPI-SIPI. Each AP walks the
 * trampoline through real mode and protected mode, then calls
 * smp_ap_entry().
 *
 * What the AP does next is the point of the per-CPU stage. It installs its
 * own state block (percpu.c), loads a private GDT/TSS whose GS entry is that
 * block, adopts the shared IDT, enables its own local APIC, takes its share
 * of the cross-CPU lock test, and then *parks*: the addresses interrupts and
 * the scheduler use are shared statics, so an AP must not schedule until the
 * bootstrap processor has finished building its idle task. Parked APs spin
 * with interrupts off; smp_release_aps() finishes the job and
 * scheduler_start_ap() enters the idle task.
 *
 * The two IPIs are what the scheduler and the MMU need from each other:
 * SMP_VEC_RESCHED makes an AP look at the task table now, and SMP_VEC_TLB
 * makes it drop stale translations after this CPU changed a page table.
 */
#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "cpu.h"
#include "frame.h"
#include "gdt.h"
#include "idt.h"
#include "klog.h"
#include "paging.h"
#include "pit.h"
#include "scheduler.h"
#include "sync.h"

#define SMP_TRAMPOLINE_BASE 0x8000u
#define SMP_TRAMPOLINE_VECTOR (SMP_TRAMPOLINE_BASE >> 12)
/* Parameter block inside the trampoline page. Must match ap_trampoline.asm:
 * four words at 0x8F00, above the real-mode stack that grows down from it. */
#define SMP_PARAM_BASE 0x8F00u

#define SMP_STACK_PAGES 1u
#define SMP_START_ATTEMPTS 3u
#define SMP_ONLINE_SPINS 200000u
/* Increments each CPU contributes to the cross-CPU lock test. Large enough
 * that a SIPI's startup latency overlaps the BSP's share, small enough to
 * stay well under a second even under TCG. */
#define SMP_LOCK_ITERATIONS 50000u
/* Bound on the "wait for every target to acknowledge" loop. An AP is in a
 * short critical section at worst, so a round trip is microseconds; the cap
 * only exists so a wedged CPU cannot hang the machine. */
#define SMP_SHOOTDOWN_SPINS 2000000u
/* Ten PIT ticks: 100 ms of counting. Long enough that a one-tick edge error
 * is 10 %, short enough not to matter at boot. */
#define SMP_CALIBRATE_TICKS 10u
#define SMP_LAPIC_FALLBACK_COUNTS 5000000u
#define SMP_LAPIC_DIVIDE 0x3u          /* TDCR encoding: divide by 16 */

typedef struct {
    uint32_t apic_id;
    uint32_t stack_phys;
    uint32_t stack_top;
    volatile uint32_t online;   /* AP reached the C entry point */
    volatile uint32_t done;     /* AP finished its share of the lock test */
    volatile uint32_t gdt_ok;   /* AP loaded its private GDT/TSS and IDT */
} smp_cpu_t;

static smp_cpu_t cpu_meta[SMP_MAX_CPUS];
static uint32_t cpu_count;

/* Shared by every CPU during bring-up. The counter is incremented with a
 * plain load/add/store on purpose: without a working atomic lock, two CPUs
 * running this at once lose updates and the total comes out short. */
static spinlock_t lock_test = SPINLOCK_INIT;
static volatile uint32_t lock_counter;

/* Set by the bootstrap processor once every AP's idle task exists. An AP
 * spins on it, so it must be volatile and the stores it releases must be
 * ordered before it. */
static volatile uint32_t released;

/* One shootdown at a time. Two concurrent rounds would overwrite each
 * other's `tlb_target`, so the lock is what makes the sequence numbers
 * meaningful. Only the bootstrap processor runs user tasks that edit page
 * tables, so in practice it is the only initiator.
 */
static spinlock_t tlb_lock = SPINLOCK_INIT;
static volatile uint32_t tlb_round;
static volatile uint32_t tlb_timeouts;

/* Local-APIC timer frequency measured against the PIT, and the boot test's
 * per-CPU hit counters. */
static uint32_t lapic_counts_per_sec;
static volatile uint32_t smp_hits[SMP_MAX_CPUS];
static volatile uint32_t smp_stop;

/* A directory address no CPU can have loaded: the frame allocator hands out
 * RAM below PAGING_PHYS_LIMIT, well under this. */
#define SMP_BOGUS_SPACE 0xFFFFF000u

/* Provided by ap_trampoline.asm. The blob is copied, not executed in place,
 * so its link address only has to be readable. */
extern void ap_trampoline_start(void);
extern void ap_trampoline_end(void);
/* Full barrier from the trampoline object; C inline asm here cannot use
 * cpuid because MSVC does not model that it clobbers EBX/ECX/EDX. */
extern void smp_memory_barrier(void);

/* --------------------------------------------------------------- AP entry */
/* Running on the AP's own stack, with the flat bootstrap GDT the trampoline
 * installed. The per-CPU block comes first because everything after it may
 * look at "this CPU"; nothing here touches shared scheduler state until
 * scheduler_start_ap(), which only runs after the release. */
void smp_ap_entry(uint32_t cpu) {
    uint32_t index = cpu;
    uint32_t i;

    if (index < SMP_MAX_CPUS) {
        /* Fill the block, then switch to this CPU's GDT/TSS: the load also
         * caches the block's base in GS, so gs:[0] works from here on. */
        percpu_init_ap(index, cpu_meta[index].apic_id);
        if (gdt_init_ap(index, cpu_meta[index].stack_top)) {
            idt_load_ap();
            cpu_meta[index].gdt_ok = 1;
        }
        /* APICBASE is per CPU: the unit the bootstrap processor enabled is
         * not the one this CPU will receive its timer and IPIs on. */
        apic_enable_local();
        cpu_meta[index].online = 1;
    }

    for (i = 0; i < SMP_LOCK_ITERATIONS; i++) {
        spin_lock(&lock_test);
        lock_counter++;
        spin_unlock(&lock_test);
    }

    if (index < SMP_MAX_CPUS) cpu_meta[index].done = 1;

    /* Park until the bootstrap processor has built this CPU's idle task.
     * Spinning (rather than HLT) keeps the release a plain flag: sleeping
     * here would need the flag test inside the interrupt path, and an AP has
     * nothing else to wake it for. */
    for (;;) {
        if (released) scheduler_start_ap(index);
        cpu_pause();
    }
}

static int start_ap(uint32_t index, uint32_t apic_id) {
    volatile uint32_t *params = (volatile uint32_t *)SMP_PARAM_BASE;
    uint32_t blob_size = (uint32_t)ap_trampoline_end -
                         (uint32_t)ap_trampoline_start;
    uint32_t stack_top;

    cpu_meta[index].stack_phys = (uint32_t)frame_alloc_pages(SMP_STACK_PAGES);
    if (!cpu_meta[index].stack_phys) return 0;
    stack_top = (uint32_t)PHYS_TO_VIRT(cpu_meta[index].stack_phys) +
                SMP_STACK_PAGES * FRAME_SIZE;
    cpu_meta[index].stack_top = stack_top;
    cpu_meta[index].apic_id = apic_id;

    memcpy((void *)SMP_TRAMPOLINE_BASE, (void *)ap_trampoline_start,
           blob_size);
    params[0] = PAGING_DIR_PHYS;      /* CR3 the AP will run on */
    params[1] = stack_top;            /* stack, via the direct map  */
    params[2] = index;                /* per-CPU slot               */
    params[3] = (uint32_t)smp_ap_entry;

    for (uint32_t attempt = 0; attempt < SMP_START_ATTEMPTS; attempt++) {
        /* A store buffer may still hold the trampoline and parameter words
         * when the SIPI lands; a full barrier before the IPI makes them
         * visible to the AP. */
        smp_memory_barrier();
        apic_send_init(apic_id);
        apic_delay(2000);
        apic_send_sipi(apic_id, SMP_TRAMPOLINE_VECTOR);
        apic_delay(2000);
        apic_send_sipi(apic_id, SMP_TRAMPOLINE_VECTOR);
        for (uint32_t spin = 0; spin < SMP_ONLINE_SPINS; spin++) {
            if (cpu_meta[index].online) return 1;
            apic_delay(10);
        }
    }
    return 0;
}

int smp_init(void) {
    const acpi_info_t *info;
    uint32_t bsp_id;
    uint32_t i;
    uint32_t spin;
    uint32_t expected;

    memset(cpu_meta, 0, sizeof(cpu_meta));
    cpu_count = 0;
    lock_counter = 0;
    lock_test.locked = 0;
    released = 0;
    tlb_round = 0;
    tlb_timeouts = 0;

    if (!acpi_init() || !acpi_info()->have_madt) {
        klog_log(KLOG_WARN, "SMP-SKIP: no ACPI MADT, staying uniprocessor\n");
        return 0;
    }
    info = acpi_info();
    if (!apic_init()) {
        klog_log(KLOG_WARN, "SMP-SKIP: local APIC did not map\n");
        return 0;
    }

    bsp_id = apic_id();
    percpu_set_apic_id(0, bsp_id);
    cpu_meta[0].apic_id = bsp_id;
    cpu_meta[0].online = 1;
    cpu_count = 1;

    /* A linear scan over at most ACPI_MAX_CPUS entries is the whole point
     * of the table: this runs once, at boot, and needs no index. */
    for (i = 0; i < info->cpu_count && cpu_count < SMP_MAX_CPUS; i++) {
        uint32_t id = info->cpus[i].apic_id;
        if (!info->cpus[i].enabled || id == bsp_id) continue;
        if (start_ap(cpu_count, id)) {
            klog_prefix();
            klog_write("SMP-AP-OK cpu=");
            klog_dec(cpu_count);
            klog_write(" apic=");
            klog_dec(id);
            klog_write("\n");
            cpu_count++;
        } else {
            klog_prefix();
            klog_write("SMP-AP-FAIL apic=");
            klog_dec(id);
            klog_write("\n");
        }
    }

    klog_prefix();
    klog_write("SMP-OK cpus=");
    klog_dec(cpu_count);
    klog_write(" lapic=");
    klog_hex(info->lapic_phys);
    klog_write(" madt=cpus/");
    klog_dec(info->cpu_count);
    klog_write(" parked=1\n");

    /* The BSP takes the same share as each AP. The APs are already running
     * theirs, so the two loops overlap and the lock has to be atomic across
     * CPUs, not just across an interrupt. */
    for (i = 0; i < SMP_LOCK_ITERATIONS; i++) {
        spin_lock(&lock_test);
        lock_counter++;
        spin_unlock(&lock_test);
    }
    for (i = 1; i < cpu_count; i++) {
        for (spin = 0; spin < SMP_ONLINE_SPINS; spin++) {
            if (cpu_meta[i].done) break;
            apic_delay(10);
        }
    }

    /* Each AP rebuilt its descriptor table: prove the TSS it loaded carries
     * that CPU's stack, which is what a ring-3 entry would use. */
    for (i = 1; i < cpu_count; i++) {
        klog_prefix();
        if (cpu_meta[i].gdt_ok &&
            gdt_ap_tss_esp0(i) == cpu_meta[i].stack_top) {
            klog_write("SMP-AP-GDT-OK cpu=");
        } else {
            klog_write("SMP-AP-GDT-FAIL cpu=");
        }
        klog_dec(i);
        klog_write(" esp0=");
        klog_hex(gdt_ap_tss_esp0(i));
        klog_write("\n");
    }

    expected = SMP_LOCK_ITERATIONS * cpu_count;
    klog_prefix();
    if (lock_counter == expected) {
        klog_write("SMP-LOCK-OK counter=");
    } else {
        klog_write("SMP-LOCK-FAIL counter=");
    }
    klog_dec(lock_counter);
    klog_write(" expected=");
    klog_dec(expected);
    klog_write("\n");
    return (int)cpu_count;
}

uint32_t smp_cpu_count(void) {
    return cpu_count;
}

/* --------------------------------------------------------------- release */
int smp_release_aps(void) {
    int started = 0;

    for (uint32_t i = 1; i < cpu_count; i++)
        if (scheduler_prepare_ap(i) >= 0) started++;

    /* The flag must not become visible before the task records it refers
     * to. */
    smp_memory_barrier();
    released = 1;
    smp_memory_barrier();
    return started;
}

void smp_resched_cpu(uint32_t cpu) {
    if (cpu >= SMP_MAX_CPUS) return;
    if (!cpu_meta[cpu].online) return;
    if (cpu == percpu_self_index()) return;   /* the caller is about to return */
    apic_send_ipi(cpu_meta[cpu].apic_id, SMP_VEC_RESCHED);
}

/* ------------------------------------------------------------- TLB flush */
/* Without PCIDs a CR3 write flushes the whole TLB, so a CPU only holds stale
 * entries for a space it has actually had loaded: `active_space` is that
 * set, and a shootdown for a space no other CPU ran costs nothing.
 * `space == 0` means "whoever you are", which is what a full teardown or the
 * self-test wants. */
uint32_t smp_tlb_shootdown(uint32_t space) {
    uint32_t me = percpu_self_index();
    uint32_t targets = 0;
    uint32_t seq;
    uint32_t spin;
    irqflags_t flags;

    flags = spin_lock_irqsave(&tlb_lock);
    seq = ++tlb_round;

    for (uint32_t i = 0; i < cpu_count; i++) {
        if (i == me) continue;
        if (!cpus[i].online) continue;
        if (space && cpus[i].active_space != space) continue;
        cpus[i].tlb_ack = 0;
        cpus[i].tlb_target = seq;
        targets++;
    }
    /* Arm every target before the first IPI: an AP that acks immediately
     * would otherwise race the next target's sequence number. */
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (i == me || cpus[i].tlb_target != seq) continue;
        apic_send_ipi(cpus[i].apic_id, SMP_VEC_TLB);
    }

    for (spin = 0; spin < SMP_SHOOTDOWN_SPINS; spin++) {
        uint32_t pending = 0;

        for (uint32_t i = 0; i < cpu_count; i++) {
            if (i == me) continue;
            if (cpus[i].tlb_target != seq) continue;
            if (cpus[i].tlb_ack != seq) pending++;
        }
        if (!pending) break;
        cpu_pause();
    }
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (i == me) continue;
        if (cpus[i].tlb_target != seq) continue;
        if (cpus[i].tlb_ack != seq) tlb_timeouts++;
    }

    spin_unlock_irqrestore(&tlb_lock, flags);
    return targets;
}

/* Runs on the CPU the IPI landed on: drop every translation, then report
 * that the round is done. A CR3 reload is the whole flush because this port
 * does not use PCIDs. */
void smp_tlb_ipi(void) {
    percpu_t *cpu = percpu_self();

    paging_flush_all();
    if (!cpu) return;
    cpu->flushes++;
    cpu->tlb_ack = cpu->tlb_target;
}

uint32_t smp_tlb_timeouts(void) {
    return tlb_timeouts;
}

/* --------------------------------------------------------- LAPIC timers */
/* Measure the local-APIC timer once, against the PIT. The counter is
 * free-running downwards from `initial`, so the elapsed count is the
 * difference; the LVT stays masked, which suppresses the interrupt but not
 * the counting, so the measurement cannot disturb the tick counter it
 * depends on. */
uint32_t smp_lapic_calibrate(void) {
    uint32_t start, end, elapsed, hz, t0;

    if (lapic_counts_per_sec) return lapic_counts_per_sec;
    hz = pit_hz() ? pit_hz() : 100u;

    apic_timer_program(0, 0xFFFFFFFFu, SMP_LAPIC_DIVIDE, APIC_LVT_MASKED);
    t0 = pit_ticks();
    start = apic_timer_current();
    while ((uint32_t)(pit_ticks() - t0) < SMP_CALIBRATE_TICKS)
        cpu_pause();
    end = apic_timer_current();

    elapsed = start - end;                  /* the counter counts down */
    if (!elapsed) {
        lapic_counts_per_sec = SMP_LAPIC_FALLBACK_COUNTS;
        return lapic_counts_per_sec;
    }
    /* hz/10 keeps the multiply inside 32 bits for any plausible bus rate. */
    lapic_counts_per_sec = elapsed * (hz / SMP_CALIBRATE_TICKS);
    if (!lapic_counts_per_sec) lapic_counts_per_sec = SMP_LAPIC_FALLBACK_COUNTS;
    return lapic_counts_per_sec;
}

uint32_t smp_lapic_counts_per_sec(void) {
    return lapic_counts_per_sec;
}

void smp_start_timer(void) {
    uint32_t period;

    if (!lapic_counts_per_sec) return;
    period = lapic_counts_per_sec / (pit_hz() ? pit_hz() : 100u);
    if (!period) period = 1;
    apic_timer_program(SMP_VEC_TIMER, period, SMP_LAPIC_DIVIDE,
                       APIC_LVT_PERIODIC);
}

/* ------------------------------------------------------------- self-test */
static void smp_worker(void) {
    uint32_t me = percpu_self_index();

    if (me < SMP_MAX_CPUS) smp_hits[me]++;
    while (!smp_stop) {
        for (uint32_t spin = 0; spin < 100000u; spin++) cpu_pause();
    }
    /* Leave the run queue; this CPU's next tick switches to its idle task
     * and the bootstrap processor reaps the slot. */
    scheduler_kernel_exit();
}

static int smp_test_percpu(void) {
    percpu_t *cpu = percpu_self();

    if (!percpu_offsets_ok() || !cpu) return 0;
    if (cpu->self != (uint32_t)cpu) return 0;
    if (cpu->index != 0 || percpu_self_index() != 0) return 0;
    if (!cpu->online) return 0;
    /* Adjacent CPUs must not share a cache line. */
    if (((uint32_t)cpu & 63u) != 0) return 0;

    klog_prefix();
    klog_write("SMP-PERCPU-OK: cpu=0 self=");
    klog_hex(cpu->self);
    klog_write(" index=");
    klog_dec(cpu->index);
    klog_write("\n");
    return 1;
}

/* Hand every AP its idle task and start it scheduling, then prove that two
 * CPUs ran two tasks at the same time: the workers are created after the
 * release, and each only ever runs on the AP it was pinned to. The caller
 * must have IRQ0 unmasked and interrupts on, because the measurement uses
 * the PIT. */
int smp_self_test(void) {
    uint32_t online = cpu_count;
    int32_t slots[SMP_MAX_CPUS];
    uint32_t i;
    int created = 0;

    if (!smp_test_percpu()) return 0;
    if (online < 2) {
        klog_prefix();
        klog_write("SMP-SCHED-SKIP: uniprocessor\n");
        klog_prefix();
        klog_write("SMP-TLB-SKIP: uniprocessor\n");
        return 1;
    }

    smp_stop = 0;
    for (i = 0; i < SMP_MAX_CPUS; i++) smp_hits[i] = 0;

    /* The AP tick is derived from the PIT, so the measurement has to happen
     * while IRQ0 is the only tick source in the system. */
    smp_lapic_calibrate();
    if (smp_release_aps() != (int)online - 1) return 0;

    for (i = 1; i < online; i++) {
        slots[i] = scheduler_create_kernel_on(i, smp_worker);
        if (slots[i] < 0) return 0;
        created++;
    }

    {
        uint32_t deadline = pit_ticks() + 300;   /* 3 s at 100 Hz, bounded */
        for (;;) {
            uint32_t seen = 0;

            for (i = 1; i < online; i++)
                if (smp_hits[i]) seen++;
            if (seen == online - 1) break;
            if ((int32_t)(pit_ticks() - deadline) >= 0) return 0;
            cpu_pause();
        }
    }

    /* Per-CPU, not a shared counter: only the APs have workers. */
    for (i = 0; i < SMP_MAX_CPUS; i++) {
        if (i >= 1 && i < online) {
            if (!smp_hits[i]) return 0;
        } else if (smp_hits[i]) {
            return 0;
        }
    }

    klog_prefix();
    klog_write("SMP-SCHED-OK: cpus=");
    klog_dec(online);
    klog_write(" workers=");
    klog_dec((uint32_t)created);
    klog_write(" lapic_hz=");
    klog_dec(lapic_counts_per_sec);
    klog_write("\n");

    smp_memory_barrier();
    smp_stop = 1;

    /* The workers park as soon as they see the flag; this CPU then frees
     * their slots. Userland runs on this processor and needs the four-entry
     * process table back. An AP that fails to leave its worker is a real
     * failure, so the joining is part of the test rather than best effort. */
    for (i = 1; i < online; i++)
        if (!scheduler_join_kernel(slots[i])) return 0;

    /* A space no CPU runs costs one sequence number and no IPI. */
    {
        uint32_t before = 0;
        uint32_t after = 0;
        uint32_t targets;
        uint32_t timeouts = smp_tlb_timeouts();

        for (i = 0; i < online; i++) before += cpus[i].flushes;
        if (smp_tlb_shootdown(SMP_BOGUS_SPACE) != 0) return 0;
        if (smp_tlb_timeouts() != timeouts) return 0;

        targets = smp_tlb_shootdown(0);          /* every other CPU */
        if (targets != online - 1) return 0;
        if (smp_tlb_timeouts() != timeouts) return 0;

        for (i = 0; i < online; i++) after += cpus[i].flushes;
        if (after - before != targets) return 0;

        klog_prefix();
        klog_write("SMP-TLB-OK: targets=");
        klog_dec(targets);
        klog_write(" flushes=");
        klog_dec(after - before);
        klog_write(" timeouts=0\n");
    }
    return 1;
}
