#include "sync.h"

irqflags_t irq_lock(void) {
    return cpu_irq_save();
}

void irq_unlock(irqflags_t flags) {
    cpu_irq_restore(flags);
}

void spin_init(spinlock_t *lock) {
    lock->locked = 0;
}

/* Test-and-set with an atomic exchange. The exchange is one locked bus
 * operation, so two CPUs entering together cannot both see a free lock. The
 * polling load is a plain volatile read: it only decides when to retry, and
 * the retry re-exchanges, so a stale value costs a spin, never the lock.
 * Interrupts stay enabled for the critical section; if the holder is
 * preempted, the timer will schedule it again while this task busy-waits. */
void spin_lock(spinlock_t *lock) {
    while (cpu_atomic_exchange(&lock->locked, 1u)) {
        while (lock->locked) cpu_pause();
    }
}

/* A plain store is the release: x86 does not reorder earlier stores past a
 * later one, so the critical section is visible before the lock is. */
void spin_unlock(spinlock_t *lock) {
    lock->locked = 0;
}

/* All holders keep interrupts disabled, so no other task can be stopped
 * while holding one of these locks; the busy-wait only runs if another CPU
 * holds the lock or the same context re-acquires it, and the latter is
 * forbidden by the locking rules. */
irqflags_t spin_lock_irqsave(spinlock_t *lock) {
    irqflags_t flags = irq_lock();
    while (cpu_atomic_exchange(&lock->locked, 1u)) cpu_pause();
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, irqflags_t flags) {
    lock->locked = 0;
    irq_unlock(flags);
}

void mutex_init(mutex_t *mutex) {
    mutex->locked = 0;
    mutex->owner = 0;
    mutex->waiters.waiters = 0;
}

/* Park the current task on the mutex until it can take the lock. The
 * decision and the park happen in one IRQ-off section, so an unlock cannot
 * slip in between the test and the sleep and leave the waiter parked with
 * nobody left to wake it. The park re-enables interrupts before halting:
 * the unlock (or the timer, which simply re-selects us) resumes execution
 * right here. */
void mutex_lock(mutex_t *mutex) {
    for (;;) {
        irqflags_t flags = irq_lock();
        if (!mutex->locked) {
            mutex->locked = 1;
            mutex->owner = scheduler_current_pid();
            irq_unlock(flags);
            return;
        }
        if (wait_queue_sleep(&mutex->waiters)) {
            __asm {
                sti
                hlt
            }
            continue;
        }
        /* Boot or idle context: there is no task to switch to, so the
         * holder can only be this context, which the rules forbid. Spin
         * with interrupts on so the timer can still make progress. */
        irq_unlock(flags);
        while (mutex->locked) {
        }
    }
}

int mutex_trylock(mutex_t *mutex) {
    irqflags_t flags = irq_lock();
    if (mutex->locked) {
        irq_unlock(flags);
        return 0;
    }
    mutex->locked = 1;
    mutex->owner = scheduler_current_pid();
    irq_unlock(flags);
    return 1;
}

void mutex_unlock(mutex_t *mutex) {
    irqflags_t flags = irq_lock();
    mutex->locked = 0;
    mutex->owner = 0;
    /* Every waiter re-tests the lock, so waking all of them costs at most
     * one extra pass through the loop per waiter and keeps the fast path
     * free of a waiter count. */
    wait_queue_wake_all(&mutex->waiters);
    irq_unlock(flags);
}

int mutex_held_by_current(const mutex_t *mutex) {
    uint32_t pid = scheduler_current_pid();
    return mutex->locked && mutex->owner == pid;
}
