/* sync.h for the shared 32-bit ATA sources.
 *
 * The 64-bit port is single-CPU and every syscall runs with interrupts
 * off, so the disk mutex only has to keep two paths from interleaving, not
 * to park a waiter. A bounded spin lock does that: the only competing
 * context is another task, and the scheduler will switch back to the
 * holder.
 */
#ifndef HAKU64_COMPAT_SYNC_H
#define HAKU64_COMPAT_SYNC_H

#include "cpu.h"

typedef struct {
    volatile uint32_t locked;
} mutex_t;

#define MUTEX_INIT { 0u }

void mutex_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);

#endif
