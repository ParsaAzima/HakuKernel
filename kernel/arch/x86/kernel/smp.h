/* Haku SMP bring-up interface (32-bit).
 *
 * Only the bootstrap processor runs the scheduler at this stage; the
 * application processors are online and parked. See smp.c for why the split
 * is deliberate and for the marker names the headless check asserts.
 */
#ifndef HAKU_SMP_H
#define HAKU_SMP_H

#include "cpu.h"

#define SMP_MAX_CPUS 8u

/* Discover, start and park the application processors. Returns the number
 * of online CPUs including the bootstrap processor; 1 means the machine
 * stayed uniprocessor (and is still fully usable). */
int smp_init(void);

uint32_t smp_cpu_count(void);

#endif
