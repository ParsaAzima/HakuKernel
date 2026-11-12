/* Haku SMP bring-up.
 *
 * Stage A of the SMP plan: find the processors the firmware describes, start
 * the application processors through the real-mode trampoline, and park each
 * one on a private stack with interrupts off. The bootstrap processor stays
 * the only CPU that runs the scheduler. That is deliberate: task scheduling
 * needs per-CPU current-task state, an atomic scheduler lock and a per-CPU
 * TSS first, and landing those before the APs are even known to start would
 * make a bring-up failure impossible to localize.
 *
 * The trampoline is copied to physical 0x8000 at boot. The page is dead by
 * then (stage 2 lived at 0x7E00 and the kernel starts at 0x10000), it is
 * inside the identity map every CR3 keeps, and the SIPI vector that starts
 * an AP is simply 0x8000 >> 12.
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

typedef struct {
    uint32_t apic_id;
    uint32_t stack_phys;
    uint32_t stack_top;
    volatile uint32_t online;   /* AP reached the C entry point */
    volatile uint32_t done;     /* AP finished its share of the lock test */
    volatile uint32_t gdt_ok;   /* AP loaded its private GDT/TSS and IDT */
} smp_cpu_t;

static smp_cpu_t cpus[SMP_MAX_CPUS];
static uint32_t cpu_count;

/* Shared by every CPU during bring-up. The counter is incremented with a
 * plain load/add/store on purpose: without a working atomic lock, two CPUs
 * running this at once lose updates and the total comes out short. */
static spinlock_t lock_test = SPINLOCK_INIT;
static volatile uint32_t lock_counter;

/* Provided by ap_trampoline.asm. The blob is copied, not executed in place,
 * so its link address only has to be readable. */
extern void ap_trampoline_start(void);
extern void ap_trampoline_end(void);
/* Full barrier from the trampoline object; C inline asm here cannot use
 * cpuid because MSVC does not model that it clobbers EBX/ECX/EDX. */
extern void smp_memory_barrier(void);

/* Running on the AP's own stack, with the flat bootstrap GDT the trampoline
 * installed. Everything beyond this point must stay per-CPU: the BSP owns
 * the scheduler until the next stage. */
void smp_ap_entry(uint32_t cpu) {
    uint32_t i;

    if (cpu < SMP_MAX_CPUS) {
        /* Give this CPU its own descriptor table, TSS and the shared IDT
         * before running any work: from here an exception on the AP is
         * reportable instead of a triple fault. */
        if (gdt_init_ap(cpu, cpus[cpu].stack_top)) {
            idt_load_ap();
            cpus[cpu].gdt_ok = 1;
        }
        /* Announce arrival before the loop so the BSP can move on and start
         * its own share; that overlap is what makes the lock test
         * meaningful. */
        cpus[cpu].online = 1;
    }

    for (i = 0; i < SMP_LOCK_ITERATIONS; i++) {
        spin_lock(&lock_test);
        lock_counter++;
        spin_unlock(&lock_test);
    }

    if (cpu < SMP_MAX_CPUS) cpus[cpu].done = 1;

    /* The AP has no work and must not touch shared scheduler state yet. */
    for (;;) {
        __asm {
            cli
            hlt
        }
    }
}

static int start_ap(uint32_t index, uint32_t apic_id) {
    volatile uint32_t *params = (volatile uint32_t *)SMP_PARAM_BASE;
    uint32_t blob_size = (uint32_t)ap_trampoline_end -
                         (uint32_t)ap_trampoline_start;
    uint32_t stack_top;

    cpus[index].stack_phys = (uint32_t)frame_alloc_pages(SMP_STACK_PAGES);
    if (!cpus[index].stack_phys) return 0;
    stack_top = (uint32_t)PHYS_TO_VIRT(cpus[index].stack_phys) +
                SMP_STACK_PAGES * FRAME_SIZE;
    cpus[index].stack_top = stack_top;

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
            if (cpus[index].online) return 1;
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

    memset(cpus, 0, sizeof(cpus));
    cpu_count = 0;
    lock_counter = 0;
    lock_test.locked = 0;

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
    cpus[0].apic_id = bsp_id;
    cpus[0].online = 1;
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
            if (cpus[i].done) break;
            apic_delay(10);
        }
    }

    /* Each AP rebuilt its descriptor table: prove the TSS it loaded carries
     * that CPU's stack, which is what a ring-3 entry would use. */
    for (i = 1; i < cpu_count; i++) {
        klog_prefix();
        if (cpus[i].gdt_ok && gdt_ap_tss_esp0(i) == cpus[i].stack_top) {
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
