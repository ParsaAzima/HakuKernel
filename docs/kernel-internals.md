# Kernel internals

[Back to documentation index](README.md)

Locking primitives, the sector cache, ELF32 program loading, and the
mirror-image build strategy.

## No floating point, no SSE

The kernel never sets `CR4.OSFXSR`, so an SSE instruction faults with `#UD`
at ring 0 and ring 3, and a task switch does not save any XMM state. Both
i386 toolchains are therefore asked for integer-only code: `/arch:IA32` for
`cl.exe`, `-mno-sse -mno-sse2 -mno-mmx` for `clang-cl`. Without that,
clang-cl inherits MSVC's `/arch:SSE2` default and may use SSE2 for
constant-size memory operations such as the `memset` calls at the start of
`paging_init` — which is exactly how a clang-cl CI build once panicked with
an invalid opcode right after the `INT3` self-test. The same flags apply to
the user C library, whose objects run at ring 3 under the same CPU state.
Crash reports print the two words at the faulting `EIP`, so a `#UD` can be
told apart from a `ud2` that the compiler emitted for an unreachable path.

## Locking

`kernel/kernel/sync.h` provides four primitives on top of `cpu_irq_save`/`restore`:

- `irq_lock`/`irq_unlock` — a plain IRQ-save pair for short critical sections
  that never block and never call into a blocking path. Nests correctly.
- `mutex_lock`/`mutex_unlock` — a task-context sleeping mutex. Interrupts stay
  enabled for the whole critical section, including the I/O inside it, and a
  waiter parks on the lock's wait queue (`TASK_BLOCKED`) instead of spinning,
  so a task queued behind a disk transfer costs no CPU. The test and the park
  happen in one IRQ-off section, which is what makes a missed wake-up
  impossible. Boot and idle context has no task to park, so the lock falls
  back to a bounded spin there; that path is single-threaded by construction.
  Mutexes are not recursive and are never acquired with interrupts disabled.
- `spin_lock`/`spin_unlock` — a task-context spinlock for very short sections.
  Waiters busy-wait, so nothing slow may happen while one is held.
- `spin_lock_irqsave`/`spin_unlock_irqrestore` — a spinlock whose holder also
  disables interrupts, making it non-preemptible and safe from IRQ context.

Acquisition is atomic across CPUs: the spinlocks exchange with `xchg` (a
locked bus operation) and release with a plain store, so an application
processor entering the critical section at the same instant as the bootstrap
processor cannot both succeed. Interrupts are still what serialises a task
against its own timer preemption on one CPU, so the interrupt-save variant
keeps `cli`.

The state *inside* the critical sections is still per-machine, not per-CPU:
the scheduler keeps one current task, one run-state bitmap and one tick
counter, and only the bootstrap processor takes timer interrupts. Running
tasks on a second CPU needs per-CPU blocks and a per-CPU timer; that is the
remaining SMP work in the
[roadmap](../ROADMAP.md#4-concurrency-and-memory-management).

The locking rules per subsystem:

- **Scheduler**: every task-array mutation runs with interrupts disabled (in an
  IRQ handler or under `irq_lock`), so no lock is needed. A task-context lock
  must never be taken with interrupts disabled — a preempted holder would
  never run again to release it. `scheduler_spawn` therefore reserves a task
  slot, then re-enables interrupts for `vfs_inherit` before publishing the
  task as runnable.
- **Allocators** (`kernel/arch/x86/mm/frame.c`, `kernel/kernel/heap.c`): IRQ-save
  spinlocks. The free-list/bitmap updates are short and the holder cannot be
  preempted, so the allocators are safe from both task and interrupt context.
- **VFS** (`kernel/fs/vfs/vfs.c`): one sleeping mutex, `vfs_lock`, around all
  descriptor/OFD table changes and file-backend calls. Because the lock sleeps
  instead of spinning, a second process that opens or reads a file while the
  first is inside a disk transfer simply blocks and is woken by the unlock;
  console and pipe I/O drop the lock first because they block for input or
  buffer space. `ofd_release` does not lock — callers hold `vfs_lock`.
- **Drivers**: `kernel/drivers/ata.c` takes the same kind of mutex (`ata_lock`)
  around a polling transfer, so the VFS lock and the disk lock are distinct and
  a task waiting for either one sleeps. The sector cache below `ata_lock` is
  the only reader of the drive outside FAT12; `kernel/arch/x86/kernel/kbd.c`
  only translates scancodes and forwards them to `terminal.c`, which owns the
  input ring (single-producer IRQ1, single-consumer task) and the IRQ-safe
  wait queue; `terminal.c` serializes output through the display and UART
  backends, which use IRQ-save spinlocks because `klog` may run from interrupt
  context.

Locks are non-recursive; the only ordering rule is VFS → disk, never the
reverse.

## Sector cache

`kernel/drivers/ata.c` keeps a 16-slot direct-mapped read cache (8 KiB) in
front of the drive. FAT12 re-reads the same metadata constantly — both FAT
copies and the directory chain of every path component — so the cache removes
most of the polling latency without changing the filesystem's write-through
model: writes go straight to the platter, a single-sector write refreshes the
cached copy, and a multi-sector write or a failed write invalidates every slot
it may have touched. Selecting another device flushes the cache, because the
sectors belong to whichever drive is active. The boot self-test reads one
sector twice and requires the second read to hit (`ATA-CACHE-OK`), so a cache
that silently serves stale data fails the headless run.

## ELF32 program loading

`kernel/arch/x86/elf/elf.c` loads static ELF32 little-endian i386 `ET_EXEC` images read
from FAT12. It validates the ELF header and program-header bounds, accepts
non-overlapping readable `PT_LOAD` segments, checks address arithmetic and
file ranges, and rejects dynamic/interpreted images and writable-executable
segments. Each segment is mapped according to its ELF permissions; the loader
zeroes `p_memsz - p_filesz` for BSS and creates a two-page user stack. A
rejected image records why in `elf_last_error()`, which the boot path prints
after `ELF-LOAD-FAIL` — the reason distinguishes a wrong-architecture binary
from overlapping segments, which look identical in a generic message.

Each loaded image receives a fresh page directory containing supervisor-only
kernel identity/direct mappings plus only that image's user pages. The scheduler
activates each process's CR3 on dispatch and returns to the kernel page
directory for its idle context. Exited process mappings and page tables are
released after switching away from their kernel stacks. Dynamic linking,
relocations, shared libraries, and images larger than the loader's limits
(1280 KiB for a program read by the boot and exec paths, 384 KiB for the
initramfs archive) are not supported.

## Why `link.exe` + `tools/pe2bin.py`?

MS `link.exe` cannot emit flat binaries and raw COFF objects carry
MSVC-specific relocations, so we let the **real linker** do its job:
`entry.obj + arch/x86/kernel/isr + arch/x86/user objects + kernel C objects` → fixed-base PE
(`/BASE:0x10000 /ENTRY:entry`),
then the Python script only copies section bytes to their RVA — zero
relocation math on our side, zero MSVC quirks to chase. The reported
`ENTRY=` address and exact padded kernel sector count are passed to stage 2.
The stage-2 CHS loop reads track-bounded chunks into the 448 KiB load window,
leaving 64 KiB below it for the bootstrap stack; `mkimg.py` enforces both
the memory and floppy-image limits. A chunk is also clamped so an INT 13h
transfer never crosses a 64 KiB DMA boundary; when that clamp shortens a
read, the loop advances the sector within the current track instead of
skipping to the next head, which keeps a large kernel image contiguous.

The A20 routine mirrors Linux `arch/x86/boot/a20.c` (test → BIOS →
test → keyboard controller → long test → fast gate, with the KBC drain
loop), and the COM1 init mirrors Linux `early_serial_init`.
