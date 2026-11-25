/* A tiny ring-3 program whose whole job is to prove that a user task can
 * live on an application processor. The kernel creates a copy of this image
 * pinned to CPU 1; every check therefore has to pass while the task's
 * current descriptor, FS-relative TLS base, syscall bookkeeping and pending
 * signals are all resolved on that CPU rather than on the bootstrap one.
 *
 * It reports its CPU with getcpu(), installs an FS base, walks to a handler
 * through raise(), then makes enough syscall/nanosleep round trips for the
 * AP's local APIC timer to preempt it at least once. Markers are prefixed
 * so the x86-64 and i386 headless checks can assert them separately.
 */
#ifdef __x86_64__
#include <haku/abi64.h>
#define AP_PREFIX "HAKU64-AP"
#define AP_SIGNAL HAKU64_SIGUSR1
typedef haku64_sighandler_t ap_handler_t;
typedef haku64_timespec_t ap_timespec_t;
#else
#include <haku/haku.h>
#define AP_PREFIX "AP"
#define AP_SIGNAL SIGUSR1
typedef sighandler_t ap_handler_t;
typedef haku_timespec_t ap_timespec_t;
#endif

#ifndef AP_CPU
#define AP_CPU 1u
#endif

static volatile int handler_runs;
static volatile unsigned handler_cpu;

static void on_signal(int sig) {
    unsigned cpu = 99u;

    (void)sig;
    (void)getcpu(&cpu, 0, 0);
    handler_cpu = cpu;
    handler_runs++;
}

int main(void) {
    unsigned cpu = 99u;
    unsigned node = 99u;
    ap_timespec_t delay;
    ap_handler_t previous;
    /* The i386 libc finds errno through [fs:0] and expects that word to be
     * the thread block's own address. A plain marker there makes the first
     * errno write dereference the marker as a pointer, so keep a two-word
     * block whose first word is its own address. */
    static volatile unsigned tls_block[2];
    volatile unsigned *tls_slot = tls_block;
    unsigned long long base = 0;

    tls_block[0] = (unsigned)tls_block;
    tls_block[1] = 0;
    int i;

    if (getcpu(&cpu, &node, 0) != 0 || cpu != AP_CPU || node != 0) {
        printf(AP_PREFIX "-FAIL: getcpu cpu=%u node=%u\n", cpu, node);
        return 1;
    }
    printf(AP_PREFIX "-CPU: running on %u\n", cpu);

#ifdef __x86_64__
    if (arch_prctl(HAKU64_ARCH_SET_FS,
                   (unsigned long long)(unsigned long)tls_slot) != 0 ||
        arch_prctl(HAKU64_ARCH_GET_FS, (unsigned long long)&base) != 0 ||
        base != (unsigned long long)(unsigned long)tls_slot) {
        printf(AP_PREFIX "-FAIL: FS base 0x%llx\n", base);
        return 2;
    }
#else
    {
        haku_thread_area_t area;

        area.entry_number = (unsigned)-1;
        area.base_addr = (unsigned)tls_slot;
        area.limit = 0xFFFFF;
        area.seg_32bit = 1;
        area.contents = 0;
        area.read_exec_only = 0;
        area.limit_in_pages = 1;
        area.seg_not_present = 0;
        area.useable = 1;
        if (set_thread_area(&area) != 0 ||
            get_thread_area() != (unsigned)tls_slot) {
            printf(AP_PREFIX "-FAIL: thread area 0x%x\n",
                   get_thread_area());
            return 2;
        }
    }
#endif
    printf(AP_PREFIX "-TLS-OK: fs-base follows the task\n");

    previous = signal(AP_SIGNAL, on_signal);
    if (previous == (ap_handler_t)-1 || raise(AP_SIGNAL) != 0 ||
        handler_runs != 1 || handler_cpu != cpu) {
        printf(AP_PREFIX "-FAIL: signal runs=%d cpu=%u\n",
               handler_runs, handler_cpu);
        return 3;
    }
    printf(AP_PREFIX "-SIGNAL-OK: handler ran on %u\n", handler_cpu);

    /* Every call goes through the AP's own current-task lookup, and each
     * nanosleep parks the task so the AP timer has a chance to preempt it
     * before the next syscall finds it again. */
    delay.tv_sec = 0;
    delay.tv_nsec = 1000000;
    for (i = 0; i < 24; i++) {
        unsigned now = 0;

        if (getcpu(&now, 0, 0) != 0 || now != cpu) {
            printf(AP_PREFIX "-FAIL: moved to %u\n", now);
            return 4;
        }
        if (nanosleep(&delay, 0) != 0) {
            printf(AP_PREFIX "-FAIL: nanosleep\n");
            return 5;
        }
    }
    printf(AP_PREFIX "-SCHED-OK: 24 syscalls and sleeps on %u\n", cpu);
    printf(AP_PREFIX "-EXIT-OK\n");
    return 0;
}
