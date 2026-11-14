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

The state *inside* the critical sections is per-CPU on both ports: each CPU
keeps its own current task, tick and switch counters, an idle context and a
local APIC timer, and adds a reschedule IPI plus a TLB shootdown round to the
cross-CPU vocabulary. On x86-64 the block is a `k64_percpu_t` reached through
`GS.base` and there is one spin lock (`spinlock64_t`) instead of the four
primitives above. It saves `RFLAGS`, disables interrupts, then claims the word
with a locked exchange, so it is both cross-CPU atomic and non-preemptible on
the CPU that holds it; the unlock restores the caller's flags. The 64-bit
scheduler table, frame allocator, heap, TLB round and disk mutex all use it,
and nothing blocking (no sleeping mutex, no I/O wait) happens while it is
held. The 32-bit scheduler adds one lock of its own, `sched_lock`, around the
transitions two CPUs really share — reserving a slot, releasing a reaped one,
publishing a kernel task — and keeps the per-CPU hot path under the
interrupt-flag rule. It is always taken before a task frame lock, never the
other way round. See
[stage D](architecture.md#stage-d-per-cpu-scheduling).

The locking rules per subsystem:

- **Scheduler**: a task only ever runs on the CPU that owns its slot, so the
  RUNNABLE/BLOCKED flips and the saved frame pointer are serialised by that
  CPU's interrupt flag alone (in an IRQ handler, or under `irq_lock`). The
  slot table itself is shared — an AP's kernel worker is reserved by one CPU
  and freed by the bootstrap processor — so reserving, reaping and publishing
  a kernel task take `sched_lock` (interrupt-save), which is always taken
  before a task frame lock. A task-context lock must never be taken with
  interrupts disabled — a preempted holder would never run again to release
  it. `scheduler_spawn` therefore reserves a task slot, then re-enables
  interrupts for `vfs_inherit` before publishing the task as runnable.
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

## Haku module loading

`tools/mkhakumod.py` is the only code that understands object files for
modules. It accepts the subset a NASM `-f elf32` object uses (no `RELA`, no
`COMMON`, no absolute symbols) and the COFF objects `cl.exe` or `clang-cl`
emit for i386 or amd64, rejects a module that does not define
`haku_module_init`, extracts the addend from each relocated word and writes
zero there, and emits the `.hkm` container described in
`kernel/module/haku_module.h`. One input object set must be a single
architecture; the header version records it (1 = i386, 2 = x86-64), and a
loader refuses the other version before touching the image.

With COFF input the tool is a small static linker, because a C module is
several objects (`hello.c` plus `libhakumod`). It resolves defined symbols to
section offsets, turns undefined ones into kernel imports (one leading C
decoration underscore is removed on i386 only), folds duplicate
`COMDAT SELECT_ANY` definitions — the same function emitted inline in two
objects — and rejects any other duplicate definition. The sections are then
laid out only if the entry or exit hook reaches them, walking the relocation
graph, so an unused library routine costs nothing. On i386,
`IMAGE_REL_I386_DIR32` becomes `R_386_32` and `IMAGE_REL_I386_REL32` becomes
`R_386_PC32` with its addend reduced by four, because the COFF field is
relative to the end of the four-byte word while ELF is relative to its start.
On amd64 the accepted forms are `ADDR64`/`DIR64` (absolute, `R_64_ABS`),
`ADDR32` (absolute) and `REL32` (`R_64_PC32`, also biased by four),
`.pdata`/`.xdata` are dropped because the kernel does no SEH unwinding, and no
underscore is stripped. Sections with `LNK_INFO`, `LNK_REMOVE`, or
`MEM_DISCARDABLE` are dropped, and uninitialized sections with no raw data
become BSS.

`build.py` builds three modules for the 32-bit image, packs
`build/MODULE.HKM`, `build/HELLO.HKM`, and `build/ROMFS.HKM`, and installs
them in both the initramfs and the FAT12 seed disk, so a user program can read
them and call `init_module`. `haku_demo.asm` keeps the NASM/ELF32 path alive
and proves relocation and logging, `hello.c` registers a character device and
checks the C runtime it linked, and `romfs.c` registers and mounts a read-only
filesystem. The x64 build packs one module, `build/X64MOD.HKM` from
`x64demo.c`, and seeds it on `build/haku64_fs.img`; the boot self-test reads it
back from the FAT12 volume, so the disk path is part of the check.

The loader does no symbol table lookup at run time. The host has already
reduced `haku_module_init`/`haku_module_exit` to offsets inside the image and
turned every undefined symbol into an offset into the import-string table, so
loading is: range-check the header, allocate `image + bss` from the heap, copy,
then walk at most 256 relocation records. Each import name is matched against
the eleven-entry export table with an exact, 48-byte-bounded comparison — a
deliberate linear scan over a tiny constant table, not a general hash lookup.

`kernel/arch/x86_64/kernel/module64.c` is the same algorithm on the 64-bit
side: it accepts version 2 only, applies `R_64_ABS`/`R_64_PC32`, and stores the
low 32 bits of every computed address because the image, the kernel text and
the exports all live below 4 GiB (an amd64 pointer is packed with a zero upper
half). Its export table has the same eleven names so `include/haku/module.h`
and `libhakumod` are shared; the six utilities work, while the capability
calls and the two struct fillers answer `-ENOSYS` until module registration
reaches the 64-bit mount table and devfs. User space reaches both loaders
through `init_module` (175) and
`delete_module` (176); `syscall64.c` bounds the image length by the container's
worst case, copies it page by page, and copies the name into a 25-byte stack
buffer.

Slot states make concurrent calls safe without holding a lock across a hook.
`module_load` reserves the slot (copying the name and the function pointers
while `LOADING`), releases the lock, claims the single `module_api_owner` token,
and only then calls `init`, so a module that logs, allocates, or takes a while
does not stall `delete_module` on another task, which in turn sees `-EBUSY` if
it asks for that same name. The token is what makes the capability calls safe:
`haku_module_register_device`, `haku_module_register_fs`, and
`haku_module_mount` bind the resource to the slot that is currently running its
init hook, and report `-EPERM` from anywhere else — including the exit hook.

Teardown is detached-before-free. `delete_module` marks the slot `UNLOADING`,
unmounts the module's prefix (`-EBUSY` while any open description points at
that mount), unregisters the filesystem (`-EBUSY` while a mount still uses the
table), and removes the device (`-EBUSY` while a descriptor is open). Only when
all three succeed does it run `haku_module_exit`, free the image, and clear the
slot; a failure restores `LIVE` and returns the error. A failed init gets the
same teardown, and if a concurrent open pins the new resource the module stays
loaded and the load reports `-EBUSY` instead of freeing live code. Mount slots
and devfs slots are reused after unload, so an existing open description's
mount index never shifts.

A C module is compiled with the kernel's switches plus `/Gy`, then packed with
`libhakumod` (`kernel/module/lib/module_lib.c` and `runtime.asm`): the string
routines and 64-bit multiply/divide helpers in C, the stack probes and 64-bit
shift helpers in assembly, since their calling convention lives in registers C
cannot name. The x86-64 build uses the same C file plus `runtime64.asm`, which
supplies `__chkstk`/`___chkstk_ms` (MSVC x64 passes the frame size in RAX).
Nothing is linked eagerly, so the reachability pass decides the final image.
Module code runs in ring 0 on the calling task's kernel stack — 8 KiB on i386,
16 KiB on x86-64 — so frames stay small. A 32-bit module must not use
floating point or SSE: the kernel neither saves x87/SSE state across a task
switch nor sets `CR4.OSFXSR`. On x86-64 SSE2 is part of the ABI and the
scheduler `FXSAVE`s each task, so compiler-emitted SSE2 is safe, but there is
still no math library. `module.c` also checks at build time that the public
`haku_module_fs_ops_t` and the kernel's `vfs_fs_ops_t` have the same size; that
assert is still part of the 32-bit build only, because the 64-bit filesystem
layer does not use `vfs_fs_ops_t` yet.

Error mapping: bad container or relocation `-EINVAL`, unknown import
`-ENOENT`, duplicate name `-EEXIST`, full table `-ENOSPC`, heap exhaustion
`-ENOMEM`, overlapping load/unload or a capability still in use `-EBUSY`.

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
