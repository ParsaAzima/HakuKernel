/* sync.h for the shared 32-bit drivers and console sources.
 *
 * The 64-bit port runs every system call with interrupts off, so a critical
 * section only has to exclude another task or another CPU, not an interrupt.
 * The ATA driver's mutex is a bounded spin for that reason. The framebuffer
 * console and the VGA text backend are also reachable from the timer
 * interrupt, so they need a lock that owns interrupts: that is the same lock
 * the scheduler uses (spinlock64_t in haku64.h), exposed here under the names
 * the shared sources use. There is one implementation, not two.
 */
#ifndef HAKU64_COMPAT_SYNC_H
#define HAKU64_COMPAT_SYNC_H

#include "cpu.h"
#include "haku64.h"

typedef struct {
    volatile uint32_t locked;
} mutex_t;

#define MUTEX_INIT { 0u }

void mutex_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);

/* The 32-bit console sources name the interrupt-safe lock this way. `flags`
 * is the caller's RFLAGS from before the acquisition, so unlocking restores
 * the caller's interrupt state instead of blindly enabling them. */
typedef u64 irqflags_t;
typedef spinlock64_t spinlock_t;

#define SPINLOCK_INIT SPINLOCK64_INIT

irqflags_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags);

#endif
