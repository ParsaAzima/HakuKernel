# Architecture

[Back to documentation index](README.md)

Boot chain, memory map, the x86-64 port, the IDT, and the
physical frame allocator.

## How it works

```
BIOS (0x7C00)
  └─ kernel/arch/x86/boot/{stage1,stage2}.asm (16-bit): load kernel at 0x10000
      → A20 (Linux-style: test after BIOS/KBC/fast methods) → GDT → CR0.PE=1
         └─ far jump to KERNEL_ENTRY (= PE entry point from link.exe)
               └─ kernel/arch/x86/kernel/entry.asm (_entry stub) → call kernel_main
                     └─ kernel/kernel/kernel_main.c: paging/IDT → GDT+TSS → scheduler
                          └─ timer-preempted ring-3 processes → int 0x80
```

Memory map:

| Address | Use |
|---|---|
| `0x6FC0` | VBE handover record from stage2 (`fb_handover_t`, magic `'HFB1'`) read by both arches |
| `0x7C00` | boot sector (512 B) |
| `0x10000–0x80000` | kernel load window (up to 448 KiB, linked `/BASE:0x10000 /ALIGN:16`) |
| `0x7000–0x7240` | E820 map from stage2 (INT 15h, count at `0x6FFC`) |
| `0x9000–0xC000` | page directory + initial tables (8 MiB supervisor identity map) |
| `0x1000–0x4000` | x86-64 boot: PML4, PDPT and PD (1 GiB identity map, 2 MiB pages) |
| `0x400000–0x500000` | direct-map page tables (one per 4 MiB of mapped RAM) |
| `0xC0000000–0xC0400000` | supervisor framebuffer window (one page-directory entry) |
| `0xC0400000–0xFFFFFFFF` | supervisor direct map of physical `0–1000 MiB`, sized from E820 at boot (the last two PDEs are the local-APIC page and the ACPI window) |
| `0x100000–0x400000` | kernel heap (kmalloc/kfree, 16 B aligned) |
| `0x90000` | bootstrap kernel stack top (grows down) |
| low E820 frames (often 4–8 MiB) | per-process 8 KiB kernel stacks, accessed through the direct map |
| `0x40000000` | user program code/data virtual base (per-process mapping) |
| `0x50000000–0x54000000` | user `brk` heap region (lazily mapped) |
| `0x60000000–0x70000000` | anonymous `mmap` region (lazily mapped) |
| `0xBFFFE000–0xC0000000` | user stack (two pages initially, grows downward on fault up to 1 MiB) |
| `0xB8000` | VGA text buffer |
| `0xFFFF800000000000` | x86-64 physmap of all RAM (PML4 slot 256, 2 MiB pages) |
| `0xFFFFF80000000000` | x86-64 framebuffer window (PML4 slot 496, 4 MiB, uncached) |

### x86-64: boot path, memory and interrupts (primary)

x86-64 is the primary architecture: `python build.py`, `run` and `headless`
produce and boot `haku64.img` from the same stage1/stage2 floppy, with a
64-bit kernel entry. The original 32-bit image is still built with
`--arch=i386`, and it remains the one that carries busybox, the shell and the
signals/pipes/job-control subset; each architecture has its own user C library
(`user/lib` on 32 bits, `user/lib64` on x86-64, see
[the x86-64 C library](libc64.md)). Stage 2 leaves the CPU in 32-bit protected
mode;
`kernel/arch/x86_64/boot/entry64.asm` takes over from there, enables PAE and
`EFER.LME`, installs a PML4/PDPT/PD identity map of the first 1 GiB with 2 MiB
pages, and far-jumps to a 64-bit code segment.

From `kernel64_main` the 64-bit kernel brings up, in order:

| Step | What it builds | Marker |
|---|---|---|
| E820 | physical inventory from the map stage2 left at `0x7000` | `HAKU64-E820-OK` |
| paging | supervisor physmap of all RAM at `0xFFFF800000000000`, 2 MiB pages | `HAKU64-PAGING-OK` |
| frames | E820-backed frame allocator, bitmaps sized from the detected limit | `HAKU64-FRAME-OK` |
| heap | 8 MiB free-list `kmalloc64`/`kfree64` window | `HAKU64-HEAP-OK` |
| console | VBE aperture mapped into `K64_FB_VIRT` (uncached, 4 MiB window), the shared renderer starts with Terminus 8x16, `COM1` keeps every byte | `HAKU64-FB-OK`, `HAKU64-TERMINAL-OK` |
| GDT/TSS | kernel and user segments, a 64-bit TSS, IST1/IST2 stacks | `HAKU64-GDT-OK` |
| IDT/PIC | 256 gates, 8259 remapped to `0x20`/`0x28`, IRQ0 unmasked | `HAKU64-IDT-OK` |
| exceptions | `INT3`, `#PF` and `#DE` provoked and resumed | `HAKU64-EXC-OK` |
| timer | 100 Hz PIT, plus a bounded wait and an `HLT` wake-up | `HAKU64-TIMER-OK` |
| keyboard | PC/8042 in set-1 mode: IRQ1 unmasked after a decode-only self-test | `HAKU64-KBD-OK` |
| RTC | CMOS wall clock sampled at boot, Unix epoch for `CLOCK_REALTIME` | `HAKU64-RTC-OK` |
| files | FAT12 on an ATA disk, behind an eight-slot page cache; `open`, `read`, `write`, `lseek`, `close`, `mkdir`, plus `stat`/`statx`, `getdents64`, `rename`, `unlink`, `truncate`, `chdir`/`getcwd`, `fsync` | `HAKU64-FS-OK`, `HAKU64-FILE-OK`, `HAKU64-DISK-OK` |
| spaces | two PML4s map one address to different frames; CR3 switches them | `HAKU64-VM-OK` |
| tasks | two 16 KiB kernel stacks preempted by IRQ0 | `HAKU64-PROC-OK` |
| fork | a child gets a copied address space, two observers of one `MAP_SHARED` page | `HAKU64-FORK-OK` |
| threads | `clone(CLONE_VM)` shares one space and, with `CLONE_FILES`, one descriptor table; a futex hands the flag over | `HAKU64-THREAD-OK` |
| ring 3 | an `IRETQ` frame with `CS=0x23`/`SS=0x1B` enters user mode | `HAKU64-USER-OK` |
| syscall | `SYSCALL`/`SYSRET` plus the DPL-3 `INT 0x80` fallback: the ported i386 table on the x86-64 ABI — I/O and metadata, paths and directories, `mmap`/`brk`/`mprotect`, process, time, limits, `poll`/`select` (pipes are still missing) | `HAKU64-SYSCALL-OK`, `HAKU64-SYSCALL64-OK`, `HAKU64-ABI-OK`, `HAKU64-PORT-OK` |
| NX | a ring-3 fetch from a mapped non-executable page faults and ends the task | `HAKU64-NX-OK` |
| ELF64 | malformed images refused, then an embedded static executable runs | `HAKU64-ELF-REJECT-OK`, `HAKU64-ELF-OK`, `HAKU64-ELF-RUN-OK` |
| modules | `X64MOD.HKM` is read from the disk, relocated and run; duplicate, truncated and bad-magic images refused | `HAKU64-MODULE-LOAD-OK`, `HAKU64-MODULE-UNLOAD-OK`, `HAKU64-MODULE-REJECT-OK` |
| self-test | physmap alias, alloc/free, a 4 KiB mapping, heap coalescing | `HAKU64-SELFTEST-OK` |

The physmap is why the 64-bit build has no equivalent of the 32-bit
1000 MiB ceiling: one page directory covers a GiB with 2 MiB pages, so
mapping 5 GiB costs five directories and no page tables. A 4 GiB guest
reports `physmap=5120MiB` and a 5120 MiB frame limit; the 32-bit image on
the same machine stops at 1000 MiB.

Interrupts follow the 32-bit model with 64-bit frames: `isr64.asm` has one
stub per vector, pushes a zero in the error-code slot for the vectors the CPU
does not supply one for, and hands a `regs64_t` to `isr64_dispatch`. Two
things are new in long mode. Gates are 16 bytes and carry an IST index, so
`#DF` runs on IST1 and NMI on IST2 — a double fault is usually the *result*
of a bad kernel stack, and without a switch it could not be reported. And the
TSS exists purely to describe `RSP0` and those stacks, since long mode has no
hardware task switching.

Because MSVC x64 has no inline assembly, `CLI`/`STI`/`LIDT`/`LGDT`/`LTR`,
port I/O, `CR2`/`CR3`, `PAUSE`, `FXSAVE`/`FXRSTOR` and the CR4/CR0 bits that
enable SSE all live in `isr64.asm` as small callable functions. The same file
holds three exception probes (`int3_probe64`, `probe_load64`, `probe_div64`).
Recovering from a fault means skipping the faulting instruction, so instead of
decoding it each probe exports a label for the instruction *after* the one
that faults and the handler sets `RIP` to it — no hardcoded instruction
lengths, and the test fails if the handler's frame is wrong.

`gdt64_load` replaces the boot GDT with `LGDT` and reloads only the data
segments. CS is deliberately not reloaded: the new table keeps the boot code
descriptor byte-identical at selector `0x08`, so the cached descriptor stays
valid. Index 1 must therefore remain the kernel code descriptor, and indices 3
and 4 hold the user *data* then user *code* descriptors — the order `SYSRET`
derives from `STAR[63:48]` (`SS = base + 8`, `CS = base + 16`).

Processes reuse that frame rather than adding a second switch mechanism.
`kernel/arch/x86_64/kernel/task64.c` saves the `regs64_t` pointer and, when
the round-robin scan picks another task, returns a frame that lives on the
incoming task's kernel stack; the stub moves `RSP` to it, pops and `IRETQ`s.
New tasks start from a frame assembled on top of a private 16 KiB kernel
stack, placed so the entry point sees the same stack alignment a `CALL`
would produce. IRQ0 drives the switch, so nothing has to yield. Because the
x86-64 ABI puts SSE2 in the baseline, `sched64_init` sets `CR4.OSFXSR` and
`CR4.OSXMMEXCPT` and every switch `FXSAVE`s the outgoing task and `FXRSTOR`s
the incoming one; unlike the 32-bit build, this path cannot pretend the
vector registers are unused.

An address space is a PML4 that copies the two kernel slots — the 1 GiB
identity map and the physmap — and owns PML4 slot 1, starting at
`0x0000008000000000` in the lower canonical half. `paging64_create_space`
takes the root from the frame allocator, and `paging64_map_space` allocates
the three intermediate levels under a caller-supplied root, so the kernel's
own tables are never edited by a user mapping. `paging64_switch_space` writes
`CR3`; the self-test maps one virtual address to two frames, writes through
both and checks the physmap and identity map still resolve. `RSP0` is
repointed at the incoming task's kernel stack on every switch, because that
is what the CPU loads when a ring-3 trap enters the kernel.

`entry64.asm` also sets `EFER.NXE`, and every leaf `paging64_map_space`
installs carries the matching bit: executable segments clear bit 63 while
data, BSS, stack and non-`PROT_EXEC` mappings set it, so code pages cannot be
written and writable pages cannot be fetched. A ring-3 `#PF` used to halt the
machine; `idt64.c` now ends the task instead, because there is no signal
model yet: `sched64_fault` records the vector and `CR2` and switches to the
next runnable task, while a kernel fault stays fatal. The NX self-test jumps
into a writable page and asserts vector 14 with `CR2` at that page. Frame
extraction in the paging code uses `K64_PHYS_MASK` rather than
`K64_PAGE_MASK`, so the NX bit can never leak into a physical address.

Ring-3 entry is an ordinary `IRETQ` from a frame whose `CS` is `0x23` and
whose `SS` is `0x1B`; the same instruction that returns from an interrupt
performs the privilege change. System calls use `SYSCALL`: `syscall64_init`
programs `STAR` (kernel segments from `0x08`, `SYSRET` base `0x10`), `LSTAR`
and `FMASK`, and sets `EFER.SCE`. `syscall64_entry` saves the user `RSP`,
switches to the task's kernel stack (the `TSS.RSP0` mirror
`gdt64_rsp0_value`), builds the same `regs64_t` frame the interrupt stubs
build, and calls `sys64_dispatch`. It returns with `SYSRET` when the
dispatcher hands back a ring-3 frame and through the shared `IRETQ` tail when
the scheduler hands back a kernel frame. `INT 0x80` at vector `0x80` remains
as a DPL-3 fallback, and the test program exercises both entry points.

`sys64_dispatch` implements the x86-64 Linux convention — number in `RAX`,
arguments in `RDI`/`RSI`/`RDX`/`R10`/`R8`/`R9`, result in `RAX` or a negative
errno — with the Linux x86-64 numbers for the ported table; an unimplemented
number returns `-ENOSYS`. The I/O core is `read`, `write`, `writev`, `open`,
`openat`, `close`, `lseek`, `pread64`, `pwrite64`, `dup`, `dup2`, `dup3`,
`fcntl`, `ioctl`, `fsync`/`fdatasync`/`sync` and the descriptor-free
metadata calls: `stat`/`lstat`/`fstat`/`newfstatat`/`statx`,
`access`/`faccessat`, `chmod`/`chown` (accepted), `utimensat`, `truncate`,
`ftruncate`, `unlink`/`unlinkat`, `rmdir`, `rename`/`renameat`/`renameat2`,
`mkdir`, `mkdirat`, `getdents64` and the `chdir`/`fchdir`/`getcwd` trio.
Memory is `mmap`, `munmap`, `mprotect` (leaf `W`/`NX` bits),
`madvise` and `brk`; the program break lives in its own window
(`K64_USER_BRK_BASE` up to the anonymous base, 4096 pages), so it never
grows into an `mmap` region, and a shrinking `brk` unmaps and frees.
Process and time calls are `fork`, `clone`, `execve`, `wait4`, `exit`/
`exit_group`, `kill`, `tgkill`, `futex`, `nanosleep`, `sched_yield`,
`getpid`/`getppid`/`gettid`, the uid/gid identity pair, `getpgrp`/`getpgid`/
`getsid`/`setsid`, `clock_gettime`/`clock_getres`, `time`/`gettimeofday`/
`times`/`getrusage`, `getrlimit`/`setrlimit`/`prlimit64`, `umask`,
`prctl`, `getrandom`, `uname`, `set_tid_address`,
`poll`/`select`/`pselect6`/`ppoll`. Every
user pointer is resolved through the current space with
`paging64_query_space` before it is dereferenced, and the copies move through
the physmap: a ring-0 page fault halts the kernel, so the CPU cannot be
relied on to reject a bad address.

Paths follow `open`'s rule: `copy_user_path64` copies one NUL-terminated
name into a `K64_PATH_MAX` buffer (one read per page, so a short string at
the end of a page is safe), `path64_normalize` collapses `.`, `..` and
repeated separators into a canonical absolute path, and `resolve64_path`
joins a relative name onto `AT_FDCWD` (the task's `cwd`),
a directory descriptor or an explicit `cwd`, so `chdir` and `openat` see
one consistent view. The task's `cwd` is canonical and absolute, inherited
by `fork` and shared by a thread group through its owner. `fs64.c` keeps
the same page cache and answers metadata from the FAT12 directory entry;
`O_DIRECTORY` descriptors use `FS64_DIR` with a parallel path table and the
descriptor position as the `getdents64` cursor.

`mmap` accepts anonymous ranges (`fd` must be `-1`) and file-backed ones.
`MAP_FIXED` places a mapping at a page-aligned address inside the anonymous
window and refuses an occupied range instead of replacing it; without it the
per-task bump region from `K64_USER_MMAP_BASE` supplies the address.
`MAP_SHARED` on a file maps the file's own frames, so a store through the
mapping is visible to `read`; `MAP_PRIVATE` copies the pages at map time, so
a later file update does not change the mapping. Mapping past the end of a file
is `-EINVAL` rather than the `SIGBUS` a Linux process would take, and a
page-aligned `offset` selects where in the file the mapping starts. A
writable shared file mapping needs a descriptor opened for writing and is
`-EACCES` otherwise. Because `munmap` may not free a frame the file still
owns, each task carries a small table of mapping ranges recording whether
their frames are owned and whether another space aliases them; a task's
table is consulted page by page, and `fork` copies it.

Files live on a FAT12 volume attached as the primary-master ATA disk. The
64-bit image compiles the 32-bit `kernel/drivers/ata.c` and
`kernel/fs/fat12/fat12.c` unchanged and supplies the seams they expect
(`kernel/arch/x86_64/compat`): port I/O, the `klog_*` surface over `k64_*`,
and a single-CPU spin mutex. `fs64.c` is a page cache in front of that
filesystem: each path gets a slot holding up to 32 frames (128 KiB) and eight
paths are cached at once, which is what lets `mmap` alias a file's frames and
keeps every descriptor of one path looking at the same bytes. `open` accepts
`O_RDONLY`, `O_WRONLY`, `O_RDWR` plus `O_CREAT`, `O_EXCL`, `O_TRUNC` and
`O_APPEND`; `mkdir` creates one 8.3 directory, and paths may contain `.` and
`..` because FAT12 resolves them. Dirty pages are written back when the last
descriptor on a path closes or the owning task exits, so the cache is
write-back, not write-through: a crash can lose the last pages. The limits
are explicit: only 8.3 names, no `unlink`, `rename` or `getdents`, no
symbolic links, at most 32 cached pages per file, and a writable
`MAP_SHARED` mapping requires a writable descriptor. A shared mapping also
relies on its descriptor staying open, because only descriptors pin a cache
slot. `python build.py headless` builds
`build/haku64_fs.img` with
`tools/mkfat12.py --empty` on every run,
seeds `HELLO.TXT`, `PATTERN.BIN`, `USER.ELF` and `X64MOD.HKM`, attaches it to
QEMU and
checks the resulting image with `tools/lsfat12.py --check`, so a disk write is
proven from the host's view of the volume and not only from the kernel's
cache.

`fork` clones the address space page by page: `paging64_clone_space` walks
the parent's user leaves and aliases the frames a mapping record marks shared.
Every private page becomes a read-only copy-on-write mapping in both spaces:
the leaf carries `PAGE_COW` (bit 9) and, when the mapping was writable,
`PAGE_COW_RW` (bit 10), and a per-frame reference count records how many
spaces map it. The first write in either space takes a private copy (or, when
the count has already dropped to one, simply restores the write bit); the
kernel's direct-map copy path breaks the sharing before it writes, and
`mprotect` keeps a COW page write-protected while remembering the logical
permission. Reference counts live one byte per frame in 4 KiB chunks claimed
from the frame allocator on first use, so a machine that never forks pays only
the 32 KiB chunk-pointer array. The child gets a copy of
the parent's syscall frame on its own 16 KiB kernel stack with `RAX=0`, a copy of
the descriptor table and the mapping table, with shared ranges marked
borrow-only. A descriptor is only an index into a 64-entry description table
holding the cache slot, the access mode and the file offset, so `fork` and
`dup` leave both processes pointing at one description: they share the offset
and, through the cache, the bytes. `dup2` moves a description onto a chosen
descriptor — including 0..2, which is how stdout is redirected onto a file —
and `dup` always answers 3 or above because 0..2 are the console. Closing a
console descriptor that was never duplicated is a no-op; there is no
`O_CLOEXEC` and no `dup3`. `wait4` blocks until a child is `DONE`, writes
`(status & 0xFF) << 8` or the terminating signal, and reaps it; a task's
space, frames and kernel stack are released only at that point, which is also
what lets the boot test inspect a task after it stopped. Signals have no
handlers: `kill` and a ring-3 fault perform the default action, terminate,
and `wait4` reports the signal number. `SIGCHLD` is not delivered, there are
no signal masks, and a task cannot signal itself through `kill` — that path
goes through `exit` so the dispatcher never resumes the frame.

`execve` reads the whole file into a heap buffer (32 pages is the cache limit),
runs `elf64_load`, and `task64_exec` releases the caller's old space — aliased
shared frames survive, as they do at exit — clears the mapping table and
resets the anonymous window before switching CR3. `argv` and `envp` are
captured first, while the old space is still current: each string is copied
out in page-sized chunks (one page-table walk per page) into an 8-entry,
128-byte-per-string, 768-byte-total kernel buffer, and a longer vector is
`-E2BIG` rather than truncated. Once the new space exists, its stack gets
`argc`, `argv[]`, `NULL`, `envp[]`, `NULL` and an auxiliary vector with
`AT_PAGESZ` and `AT_ENTRY` (there is no `AT_PHDR` yet), and the task is
resumed at the new entry with that stack and zeroed registers. Descriptors
survive execve because there is no `O_CLOEXEC`. A failed call returns
`-ENOENT`, `-ENOEXEC`, `-EIO`, `-E2BIG`, `-EFAULT` or `-ENOMEM` and leaves the
caller running unchanged. A task whose space is shared with another thread
cannot exec at all: `task64_exec` refuses, because there is no way to stop the
other threads, and the call reports `-EINVAL`.

Threads are tasks that share one address space. `task64.c` gives each slot a
`space_owner`, so exactly one slot owns the PML4, the mapping records and the
anonymous bump pointer while `space_refs[owner]` counts the live users. A
thread's `sched64_space` is the owner's, `mmap` reserves from the owner's bump
pointer (so two threads never get the same range), and `munmap` edits the
owner's records. The space is released once, when the last user is reaped; if
the owner dies while a thread still runs, the records and the bump pointer
move to that thread so a later `munmap` or the final exit still sees them.

`clone` requires `CLONE_VM` and a 16-byte aligned child stack inside the user
half; without `CLONE_VM` it falls back to `sched64_fork`, so a caller that
asks for a process still gets one. `CLONE_FS`, `CLONE_FILES`, `CLONE_SIGHAND`,
`CLONE_THREAD`, `CLONE_SYSVSEM` and `CLONE_PARENT_SETTID` are accepted and
everything else — TLS, child-tid stores, `CLONE_CLEARTID` — is `-EINVAL`
rather than ignored, so a caller never believes it got a feature the kernel
does not implement. The child resumes right after the `SYSCALL` with `RAX=0`
and the requested stack, keeps the creator as its parent (so `wait4(tid)`
reaps it), and with `CLONE_THREAD` keeps the creator's `tgid`: `getpid`
returns the thread group's id while `gettid` returns the task's own, and
`getppid` answers with the group leader's parent. `CLONE_FILES` makes the
descriptor table shared: `fs64.c` marks one slot as the table's owner with a
user count, every descriptor call resolves through it, and only the last user
closes the descriptions. Without `CLONE_FILES` the child gets a private copy
of the table, with the same descriptions and therefore the same offsets.

`futex` implements `FUTEX_WAIT` and `FUTEX_WAKE`, with `FUTEX_PRIVATE_FLAG`
accepted because there is one address space per check, and the op is the low
seven bits. `WAIT` reads the word through the page-table validator and
compares it with the expected value: a mismatch is `-EAGAIN`, not a sleep
that misses the wake. A match moves the task to `K64_TASK_WAIT` with its
syscall frame saved and `RAX=0`, so a wake resumes it as if the call had
returned; the round-robin scan skips `WAIT`. `WAKE` makes at most `count`
waiters on the same address runnable again and answers how many it woke. There
is no timeout and no priority inheritance, and a wait that leaves nothing
runnable prints `HAKU64-TASK-HALT: every task is blocked on a futex` and halts
instead of spinning. Two thread limitations are deliberate: `exit` and
`exit_group` end only the calling task (there is no group exit that stops the
other threads), and `kill` addresses one task id, so a signal aimed at a
process id reaches the thread in that slot rather than the whole group.

`clock_gettime` reports `CLOCK_MONOTONIC` as time since boot and
`CLOCK_REALTIME` as the CMOS RTC epoch sampled once at boot plus the ticks
elapsed since that sample; when the RTC cannot be read it falls back to
monotonic time. `nanosleep` rounds the request up to whole timer ticks and
then waits with interrupts enabled, the same way `wait4` does, so other
READY tasks run during the sleep; there is no sleep queue, the waiter stays
the current task and is simply resumed, and `remaining` is never written
because nothing interrupts the sleep. `poll` and `select` answer from the
descriptor table instead of wait queues: the console is always readable and
writable, a file is readable while its position is below the size, and a
positive timeout is spent in tick slices; an infinite wait with nothing
ready returns 0 rather than blocking. Pipes, symlinks and real signal
handlers are still missing; the syscall port that is there covers what the
tests and a static libc probe need.

The 64-bit path now has an ELF64 loader, a FAT12 data disk, the syscall table,
a module loader and a user C library (`user/lib64`) with `/INIT.ELF` as the
first real ring-3 program, but no VFS/devfs, no console device and no bootable
root filesystem image. A shell therefore still requires the 32-bit side, which
is where the full userland lives. [The roadmap](../ROADMAP.md) (milestones 1
to 4) lists the remaining stages: a TLS base and the musl probe for milestone
1, then a VFS/devfs with module capabilities, pipes and signals, and the
interactive shell. The 64-bit sources are excluded from the 32-bit link line,
so both targets build from one tree.

`kernel/arch/x86_64/elf/elf64.c` follows the 32-bit loader's rules — validate
the whole image before mapping, accept only static `ET_EXEC`, require
readable `PT_LOAD` segments that are not both writable and executable, refuse
segments whose offset is not congruent with their address, and name the first
reason a file was rejected. It maps each segment into a fresh
`paging64_create_space` space, zero-filling the pages the file does not cover,
then allocates a four-page stack ending one page below the top of PML4 slot
1; the gap between the stack and the image is unmapped, so a runaway stack
faults instead of overwriting code. Images must load above
`0x0000008000010000`, which keeps the first 64 KiB of the slot unmapped.
There is no `AT_PHDR` because nothing walks it — only the `execve` hand-off
builds an auxiliary vector, with `AT_PAGESZ`, `AT_ENTRY` and `AT_NULL` — and
the loader records the page-aligned start of the first writable segment so the
self-test can read it back.

The test executable is `kernel/arch/x86_64/user/ring3_elf64.asm`: a flat
`[BITS 64]` program that writes `ELF64` through `SYSCALL`, proves the
`SYSRET` return by reaching a second write (`HAKU64-SYSCALL64-OK`), exercises
the six-argument syscall ABI (`writev`, `getpid`, `CLOCK_MONOTONIC` and
`CLOCK_REALTIME`, anonymous `mmap`/`munmap`, `MAP_FIXED` placement,
`MAP_SHARED|MAP_ANONYMOUS`, and the `EBADF`/`EFAULT`/`ENOSYS`/`EINVAL` error
paths over `INT 0x80`), reads `/HELLO.TXT` and prints the bytes it read
(`HAKU64-FILE-OK`), maps that file privately and checks a store does not
reach the file, maps it shared and checks the store does, maps
`/PATTERN.BIN` at a page offset and compares it with a `read` of the same
range, creates `/TMP`, writes `/TMP/NOTE.TXT`, reads it back before and after
`close` (`HAKU64-DISK-OK`), resolves `/TMP/../HELLO.TXT` and `/./HELLO.TXT`,
then maps a shared anonymous page, forks, and requires the child to see the
parent's word and the parent to see the child's store after `wait4`
(`HAKU64-FORK-OK`). It then reads `/HELLO.TXT` through one descriptor and its
`dup` to prove both share an offset, `dup2`s the descriptor onto 9 and reads
past a `close`, and writes `HAKU64-DUP-OK` through a `dup` of the console
(`HAKU64-DUP-OK`); `nanosleep` must move `CLOCK_MONOTONIC` forward by at
least the 10 ms requested, and a nanosecond field of 1e9 is `-EINVAL`. It then
forks again and the child first `execve`s a missing path, which must return
`-ENOENT` with the child still running, and then `/USER.ELF` with
`argv = {"/USER.ELF", "alpha", "beta"}` and `envp = {"A=1"}`. The exec'd
program, `kernel/arch/x86_64/user/exec_elf64.asm`, walks that stack image
([rsp] argc, then argv, NULL, envp, NULL, auxv), prints
`HAKU64-EXEC-PROG` and `HAKU64-EXEC-ARGS`, and exits 42; the parent requires
status 42 and then prints `HAKU64-EXEC-OK`. `USER.ELF` is seeded on the FAT12
volume by `build.py` and is not part of the kernel image, so the three markers
prove the program came off the disk, that `execve` replaced the child's
address space, and that the argument vector landed where a startup stub reads
it. It next maps a two-page thread stack, `clone`s a thread with
`CLONE_VM|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD`, sleeps in
`futex(FUTEX_WAIT)` on a word in the shared BSS, and requires the thread to
have bumped that word with `lock inc`, stored its `gettid` and `getpid`
(equal to the parent's), and `dup2`d the console onto 9 in the shared
descriptor table — the parent proves the last one by writing through 9 and
printing `HAKU64-THREAD-OK` — before `wait4(tid)` reaps it with status 7.
Then it walks the rest of the ported table, forty-seven steps numbered 71-117:
`stat`/`fstat`/`statx` against `/HELLO.TXT` (size and `S_IFREG`),
`chdir("/TMP")` with `getcwd` reporting the canonical path and a relative
`NOTE.TXT` open, `openat(AT_FDCWD, ...)`, `mkdirat`/`O_DIRECTORY`/
`getdents64` on an empty directory followed by `unlinkat(AT_REMOVEDIR)`,
create/write/`ftruncate`/`truncate`/`renameat`/`stat`-gone/`unlinkat` on a
relative name, `getdents64` on the non-empty `/TMP` (a 32-byte record typed
`DT_REG`), `chdir("/")`, `uname`, `gettimeofday`, `times`, `getrlimit` and
`clock_getres`, `prctl(PR_SET_NAME)`/`PR_GET_NAME`, two differing
`getrandom` words, `brk(0)` then a two-page grow with a store, `mprotect`,
and a shrink back, `poll(POLLOUT)` on the console, `select` on fd 0, a
`sched_yield`, and `dup3(1, 10)` plus `fcntl(F_GETFD)`/`F_GETFL`, printing
`HAKU64-PORT-OK`. It then prints `HAKU64-ABI-OK`, stores a magic word in
its BSS page and exits with status 0; every failure exits with the number of
the step that failed, which is what `HAKU64-ELF-RUN-FAIL: exit=` reports.
`tools/mkelf64.py` wraps the flat binary in an
`ET_EXEC`/`EM_X86_64` image with an RX `PT_LOAD` at `0x0000008000400000` and a
zero-filesz RW segment one page above it; `user64_image.asm` embeds the result
in `.rdata`, which `tools/pe2bin.py` copies into the kernel image. The boot
test first feeds `elf64_load` six malformed copies (wrong class, wrong
machine, W+X, bad entry, offset past the file, truncated header) and requires
a reason for each, then loads the valid image, checks that the BSS page is
zero, and runs it as a user task. The magic word (`0x48414B55`) can only be
there if ring 3 reached the store, and the boot test also requires exit
status 0 and the mmap bump pointer to have advanced, so `HAKU64-ELF-RUN-OK`
proves execution rather than a successful mapping.

Physical layout on the 64-bit path: `0–2 MiB` firmware, boot stages, kernel
image and the boot page tables; `2–16 MiB` boot allocations (page tables,
frame bitmaps); `16–24 MiB` kernel heap. A machine needs at least 32 MiB.

### Interrupts (IDT)

`kernel/arch/x86/kernel/isr.asm` builds a uniform stack frame for all 256
vectors (dummy error code where the CPU pushes none), saves all data segments,
and calls into C. `kernel/arch/x86/kernel/idt.c`
installs 32 exception gates (0–31), 16 IRQ gates (32–47, used once the
8259 is remapped) and a catch-all for 48–255 that reports instead of
triple-faulting. The syscall gate at vector 128 is DPL 3; all other gates
remain ring-0-only. All are interrupt gates, so handlers run with IF clear.
The boot proves the exception path with an `INT3` self-test.

### SMP: ACPI, the local APIC and application processors

SMP bring-up is staged, and stages A through D are implemented on both
kernels: they discover the machine's processors, enable the local APIC, start
every application processor, give it its own descriptor table, TSS and the
shared IDT, and then let it schedule tasks of its own. Stages A and B are the
bring-up itself, stage C is the shared state a second CPU needs (atomic
spinlocks and per-CPU descriptor tables), and stage D gives every CPU a
private scheduler state block, its own tick and its own idle task, so two
CPUs run two tasks at the same time. Section "Per-CPU scheduling" below
describes stage D and how the two ports reach the same state.

Three pieces are shared by both builds:

- `kernel/drivers/acpi.c` finds the RSDP (extended BIOS data area, then the
  `0xE0000–0xFFFFF` firmware window), follows the RSDT or XSDT, and parses the
  MADT's local-APIC (type 0), x2APIC (type 9) and MMIO-override (type 5)
  entries. AML and the ACPI interpreter are not implemented, and neither is
  anything else the kernel does not need at boot. All walks are bounded by the
  tables' own lengths and small hard caps.
- `kernel/drivers/apic.c` drives the xAPIC register page: it enables the unit
  through `SVR`, sets `TPR` to zero, masks every LVT input except LINT0, and
  sends INIT/SIPI IPIs. LINT0 is programmed for ExtINT, which is what keeps
  the 8259 PIC usable after the APIC is enabled — with the unit on, the PIC's
  output reaches the CPU through LINT0, and leaving that input masked would
  silently lose every hardware interrupt.
- A per-architecture hook maps ACPI tables and the APIC page, because each
  build reaches physical memory differently.

The 32-bit build (`kernel/arch/x86/kernel/smp.c`) copies
`ap_trampoline.asm` to physical `0x8000` (dead stage-2 space, inside the
identity map and the vector the SIPI names), passes the kernel page directory,
a direct-mapped stack, the CPU index and the C entry point in a four-word
parameter block at `0x8F00`, then performs INIT-SIPI-SIPI. The trampoline
takes the AP from real mode to protected mode, loads CR3, enables paging and
calls `smp_ap_entry`. The local APIC is identity mapped at `0xFEE00000`
(page-directory entry 1019) and that entry is copied into every process
directory, so an interrupt or IPI never depends on which CR3 is loaded.
ACPI tables are read through a two-page scratch window at `0xFF000000`.

The x86-64 build (`kernel/arch/x86_64/kernel/smp64.c`) uses
`ap_trampoline64.asm`, which walks an AP through protected mode with PAE,
sets `EFER.LME|NXE`, enables paging and far-jumps into a 64-bit code segment.
It reuses the BSP's PML4, so no page tables are built on the AP. The APIC page
lives in its own uncached PML4 slot (`K64_LAPIC_VIRT`, PCD+PWT) and is copied
into every per-process PML4; ACPI tables are read through a separate window in
slot 384, and the VBE framebuffer at `K64_FB_VIRT` (slot 496, 4 MiB, uncached,
also copied into every PML4 because a `write()` paints on the caller's CR3).
The three windows are why the 64-bit identity-mapped probe used by the
exception self-test lives in slot 416.

`python build.py headless --arch=i386` and `python build.py headless` both boot
with two CPUs by default (both take `--smp=N`) and assert
`SMP-OK cpus=2`/`SMP-AP-OK cpu=1` and `HAKU64-SMP-OK: cpus=2`/
`HAKU64-SMP-AP-OK cpu=1` respectively.

#### Stage C: state a second CPU can share

Three things had to change before an AP could do anything beyond parking.

- **Atomic spinlocks.** `spin_lock` and `spin_lock_irqsave` now acquire with
  `xchg`, which is implicitly locked with a memory operand, so two CPUs
  entering together cannot both see a free lock. `spin_unlock` is a plain
  store: x86 does not move earlier stores past a later one, so the critical
  section is visible before the lock is released. The poll before each retry
  is an ordinary volatile read plus a `pause` hint; a stale value costs a
  spin, never the lock. The x86-64 build uses the same exchange through
  `cpu_atomic_exchange64`, which also backs its disk mutex. `cli` is still
  what protects the interrupt-save variant, but it is no longer what makes
  the acquisition atomic — on one CPU that was enough, on two it was not.
- **Per-CPU GDT/TSS.** A TSS is a single object, so each CPU needs its own, and
  the 32-bit build cannot put one behind the same table without a private
  copy. `gdt_init_ap(cpu, stack_top)` fills, loads and `LTR`s a private table
  with flat ring-0/ring-3 descriptors and the same selector values, then
  `tss.rsp0` (`esp0` on 32-bit) points at that CPU's kernel stack. Both
  tables describe identical segments, so the cached CS/SS stay valid and
  `gdt_load` does not reload them. The x86-64 build allocates the table, TSS
  and two IST stacks with the frame allocator *on the BSP*, before the AP is
  released, and the AP only executes `LGDT`/`LTR`, reached through the shared
  physmap. The allocator itself became cross-CPU safe in stage D; building
  the table early keeps the AP's first instructions to "load what already
  exists" and avoids the question of which CPU owns a half-built TSS.
- **AP IDT.** An AP starts with `IDTR` zero, so its first exception would be a
  triple fault. Both builds load the shared table on the AP right after the
  new descriptor table.

The headless runs then prove both halves. Each CPU increments a plain
(non-atomic) counter 50 000 times under the spinlock, so the expected total is
`50000 * cpu_count`; a missing lock loses updates and the sum comes out short.
The reported TSS stack is checked against the frame the BSP allocated for that
CPU. Markers: `SMP-LOCK-OK counter=100000 expected=100000` and
`SMP-AP-GDT-OK cpu=1 esp0=…` on 32-bit; `HAKU64-SMP-LOCK-OK` and
`HAKU64-SMP-AP-GDT-OK … rsp0=…` on x86-64. The AP sets `online` before its
loop, so the BSP runs its share while the AP is still working and the two
loops overlap.

What is *not* shared, even in stage C: the scheduler's current-task pointer,
its run-state bitmap and its tick counters were one set of globals, the timer
ran on the BSP, and nothing dispatched a task to an AP. Those are stage D.

#### Stage D: per-CPU scheduling

Stage D is what turns "an AP that runs C code" into "a CPU that runs tasks".
Both ports implement it: the state is the same shape, and how a CPU reaches it
is what differs.

- **The per-CPU block.** Each CPU owns one 128-byte block: a self pointer, the
  ring-0 stack pointer, the index and APIC id, the current task, an online
  flag, tick and switch counters, the address space this CPU has in CR3, and
  the shootdown sequence numbers. Blocks are cache-line aligned so `ticks` on
  one CPU never invalidates a neighbour's line. On x86-64
  (`kernel/arch/x86_64/kernel/percpu64.c`) `IA32_GS_BASE` points at it, so
  `k64_cpu()` is one instruction (`mov rax, [gs:0]`); the two offsets the
  assembly hardcodes (`rsp0` at 8, `user_rsp` at 16) are checked against the
  compiler's layout at boot. On 32-bit (`kernel/arch/x86/kernel/percpu.c`) the
  block is reached through GDT entry 7 (`CPU_GS`, selector `0x38`), so
  `percpu_self()` is `mov eax, gs:[0]` (`65 A1 00 00 00 00`) and FS keeps
  carrying the user thread pointer for `set_thread_area`. The 32-bit linker
  runs at `/ALIGN:16`, which rejects a 64-byte alignment in `.data`, so the
  blocks are declared 16-byte aligned and rounded up to a cache line when the
  first one is installed.
- **Keeping GS on the block.** On x86-64 the invariant is: in ring 0
  `GS.base` is the block and `IA32_KERNEL_GS_BASE` is zero, in ring 3 it is
  the other way round. The interrupt stub swaps iff the interrupted CS says
  ring 3, the resume path swaps iff the frame it is about to `IRETQ` says ring
  3, and the syscall entry swaps unconditionally because it is only ever
  entered from ring 3. `gdt64_load` deliberately does not reload FS or GS.
  32-bit has no SWAPGS: a segment's base is a GDT entry, so the per-CPU base
  is installed once by `gdt_init`/`gdt_init_ap` and never changes while the
  CPU is in ring 0. `gdt_load` does reload `GS=0x38` after the flat selectors,
  because `mov gs, ax` would reset the base to zero. `SAVE_SEGMENTS` reloads
  `0x38` only for a frame whose saved CS has ring-3 privilege, so an
  interrupt that arrived in ring 0 keeps the base, and a kernel task's
  synthetic frame stores `CPU_GS` rather than `KERNEL_DS` so resuming it does
  not clear the base either. Ring 3 loads `GS=USER_DS` (base 0), so a
  userland `gs:[off]` would fault instead of reaching kernel state.
- **Interrupt-owning spin locks.** `spin64_lock`/`spin_lock_irqsave` save the
  flags word, disable interrupts and then claim the word with a locked
  exchange; the unlock releases it and restores the caller's flags. Combined
  take-then-mask is the point: a lock taken with IF on and a tick arriving
  between the test and the store would let the interrupted critical section
  re-enter itself on the same CPU. The frame allocator, the heap, the TLB
  round and the disk mutex all use this lock, so a lock is never held across
  a blocking wait. The 32-bit scheduler adds one more, `sched_lock`, for the
  transitions that two CPUs really share — reserving a slot, releasing a
  reaped slot, publishing a kernel task — while the hot path (pick the next
  task, flip RUNNABLE/BLOCKED) stays on the per-CPU side, protected by that
  CPU's interrupt flag alone, because a task only ever runs on its own CPU.
- **Pinned tasks.** A task records the CPU it may run on at creation and is
  never migrated, which is why the per-CPU run state is a scan of the task
  table filtered by that field rather than a shared run queue. Nothing has to
  be consistent across CPUs to pick the next task: each CPU walks its own
  subset, and `current`, the round-robin cursor and the switch counter live in
  the block.
- **Every CPU gets an idle task.** The BSP keeps the original boot-context
  idle; each AP is handed one that halts in a loop, so a CPU with no ready
  task always has a valid frame to switch to and never needs a "nothing to
  run" special case. APs come up parked with interrupts off and spin on a
  release flag; on x86-64 the BSP builds the idle task *after* `sched64_init`,
  then sets the flag, and the AP adopts the task and `IRETQ`s into it. On
  32-bit the idle context is a per-CPU record (`idle_tasks[cpu]`, laid out like
  any other task) that `scheduler_prepare_ap` builds before the release; the
  AP's `scheduler_start_ap` sets its own `current` to the idle index, points
  the TSS at that stack, starts its LAPIC timer and `iret`s into the loop.
  Because the 32-bit process table has only four slots, the boot test's kernel
  workers do not stay resident: `scheduler_kernel_exit` marks the slot
  `TASK_REAP_PENDING`, the AP's next tick switches it to idle, and the BSP's
  `scheduler_join_kernel` frees the slot once no CPU is standing on it.
- **A timer per CPU.** IRQ0 on the BSP stays the wall clock, but an AP does
  not receive it, so each CPU programs its own local APIC timer. The divisor
  is calibrated once on the BSP against the PIT (mask the LAPIC timer, wait
  100 ms of PIT ticks, divide), with a fixed fallback if the measurement looks
  impossible. The APs' tick therefore has the same period as the BSP's without
  a second clock source. On 32-bit the BSP's scheduler tick is still IRQ0 from
  the 8259, and only the APs use the LAPIC.
- **Cross-CPU wake-ups and TLB coherence.** Two new IDT vectors carry the
  work on both ports: `0x40` is the per-CPU timer, `0x41` asks a CPU to
  reschedule (a kill, a signal or a futex wake that made a task runnable
  elsewhere) and `0x42` is a TLB shootdown. A page-table edit only
  invalidates the editing CPU, so `paging64_map_space`, `protect_space`,
  `unmap_space` and the copy-on-write break call
  `smp64_tlb_shootdown(space)` on x86-64, and the 32-bit `paging_map`/
  `paging_protect`/`paging_unmap` paths call `smp_tlb_shootdown` through
  `paging_space_changed`. The shootdown arms the target CPUs' slots before
  sending any IPI, waits a bounded number of spins for the acknowledgements,
  and counts timeouts rather than ignoring them. Because neither port uses
  PCIDs, a CPU only holds stale entries for a space it has had loaded, so the
  shootdown targets only CPUs whose `active_space` matches and a space nobody
  else runs costs no IPI at all.
- **Releasing an address space** (`paging64_destroy_space`) still does not
  shoot down: it runs after the last task using that space has been reaped, so
  no CR3 names it, and a shootdown could not help because the frames about to
  be freed are the page tables a stale entry would point at.
- **Housekeeping stays on one CPU.** On 32-bit, reaping a slot, waking
  sleepers and re-scanning wait queues all run from CPU 0's tick, and the
  post-user FS check (`fs_error_self_test`) only runs on CPU 0's idle loop:
  the console writer has no lock of its own and only the BSP edits user page
  tables, so doing that work in one place keeps the diagnostics and the
  address-space teardown single-threaded.

Both builds are checked with the same three shapes. `python build.py headless`
boots two CPUs by default and asserts `HAKU64-PERCPU-OK`,
`HAKU64-SMP-SCHED-OK: cpus=2 workers=1` and
`HAKU64-SMP-TLB-OK: targets=1 flushes=1 timeouts=0`;
`python build.py headless --arch=i386` asserts the same three moments as
`PERCPU-OK`, `SMP-PERCPU-OK`, `SMP-SCHED-OK: cpus=2 workers=1` and
`SMP-TLB-OK: targets=1 flushes=1 timeouts=0`. `--smp=4` moves the markers to
`cpus=4 workers=3 targets=3` on both (the 32-bit table is why its ceiling is
four CPUs), and `--smp=1` skips the two scheduling tests with an explicit
`…-SKIP: uniprocessor`. The scheduling test pins one worker per AP and waits
for every worker's counter, which can only advance if that AP dispatched from
the shared task table.

What stage D does *not* do: there is no load balancing or migration (affinity
is fixed at creation), no PCID/ASID tagging, and user-space concurrency is
still exercised on the BSP — the fork/exec tests drive ring-3 tasks that are
pinned there, so a user task preempted on one CPU and resumed on another is
not yet a supported pattern. On 32-bit, APs only run kernel-mode tasks: there
is no per-AP user address space switching yet. The per-CPU state it would need
is in place; the syscall layer's own bookkeeping is the next piece of work
(see `ROADMAP.md`).

### Haku modules

A Haku module is loadable ring-0 code in a flat, self-describing container, not
an ELF or COFF object. The kernel has no object-file writer and does not learn
one: the host tool `tools/mkhakumod.py` reads the relocatable objects a
compiler emits, lays the allocatable sections out back to back, records the BSS
size and the entry/exit offsets, and reduces the relocations to records the
loader can apply. The `.hkm` layout (a 72-byte header followed by the image,
the relocation records and a string table) is documented in
`kernel/module/haku_module.h`; the kernel never reads an object header. The
header's version field names the architecture — 1 is i386, 2 is x86-64 — so an
image packed for one kernel is refused by the other instead of being relocated
with the wrong arithmetic.

The host tool accepts two object formats and two architectures. A NASM
`-f elf32` object is packed on its own and keeps the ELF addend convention. A
COFF object may be given together with others, and the tool links them:
`IMAGE_REL_I386_DIR32`/`REL32` (or the amd64 `ADDR64`, `ADDR32`, `REL32` and
`DIR64`) become container relocations, a symbol defined in the image resolves
to its section offset, an undefined one becomes a kernel import with one C
decoration underscore removed on i386, two `COMDAT SELECT_ANY` definitions fold
into the first, and sections the entry and exit hooks cannot reach are dropped.
For amd64 the tool also drops `.pdata`/`.xdata` (the kernel does no SEH
unwinding). The reachability pass is what keeps a C module small: it links one
library object and ships only the helpers it actually calls.

Four relocation types exist, each storing one 32-bit word. On i386, `R_386_32`
computes `S + A` and `R_386_PC32` computes `S + A - P`; on x86-64, `R_64_ABS`
and `R_64_PC32` do the same arithmetic on 64-bit addresses and store the low
half. The latter is safe because every address the kernel can hand a module —
the image from the heap, the kernel text and the exports — is below 4 GiB, and
an amd64 pointer is packed as its low half with a zero upper half. `P` is the
runtime address of the relocated word. The symbol is either an offset into the
string table — resolved against the kernel's export table — or
`HAKU_MODULE_SYM_BASE` (`0xFFFFFFFE`), which means "S is the module load base"
and carries the section-relative target in the addend. A module links only
against the kernel's export table; the lookup is a linear scan over eleven
entries, so loading is `O(relocations × exports)` with both bounded by
constants. The exports are six utilities (`haku_module_log`,
`haku_module_log_dec`, `haku_module_log_hex`, `haku_module_ticks`,
`haku_module_alloc`, `haku_module_free`), three capability calls
(`haku_module_register_device`, `haku_module_register_fs`,
`haku_module_mount`), and two struct fillers (`haku_module_fill_stat`,
`haku_module_fill_dirent`) that keep the kernel's `vfs_stat_t` and
`vfs_dirent_t` layouts out of module code. Imported names have their own
48-byte bound (`HAKU_MODULE_SYMBOL_MAX`), because an export such as
`haku_module_register_device` is longer than a 24-byte module name.

The loader (`kernel/module/module.c`) validates every field against the file
size and the fixed limits (64 KiB image, 64 KiB BSS, 256 relocations, 4 KiB of
import names), allocates and zeroes `image + bss`, copies the image, applies
the relocations, and only then publishes the name. A four-slot table tracks
each module as `FREE`, `LOADING`, `LIVE`, or `UNLOADING`, and the transitions
run under `spin_lock_irqsave`, so a second load or unload of the same name
reports `-EBUSY` instead of racing the hooks. `init_module` runs the module's
`int haku_module_init(void)` after the slot is reserved, propagates a negative
return from the hook, and otherwise maps a non-zero return to `-EINVAL`.

The x86-64 loader (`kernel/arch/x86_64/kernel/module64.c`) applies the same
container rules with the version 2 check and the two 64-bit relocation forms.
It exports the same eleven symbols so one library serves both kernels, but
only the six utilities do work: logging, ticks and the heap exist on the 64-bit
side, while the three capability calls and the two struct fillers answer
`-ENOSYS` because there is no 64-bit VFS or devfs to own the result. A 64-bit
module can therefore compute and report; publishing a device or a filesystem
waits for the 64-bit VFS. The lifecycle is the same four-slot table, with the
transitions under an atomic lock, and `init_module`/`delete_module` copy the
user buffer page by page through `copy_from_user64` before the loader sees it.

#### Writing a module in C

`include/haku/module.h` is the whole public interface. It declares the eleven
services above, the typed device callbacks, and `haku_module_fs_ops_t`, which
mirrors `vfs_fs_ops_t` field for field; `module.c` refuses to build if the two
structures ever differ in size, and the headless C-filesystem test exercises
their field order. A module defines

```c
int haku_module_init(void);   /* required; 0 accepts the load */
void haku_module_exit(void);  /* optional; teardown after detach */
```

and nothing else is special: the packer finds the entry and exit by name.

A C module links `libhakumod`, which is `kernel/module/lib/module_lib.c` plus
`kernel/module/lib/runtime.asm`. The C half supplies `memset`, `memcpy`,
`memmove`, `strlen`, `strcmp` and the compiler's 64-bit helpers (`__allmul`,
`__aulldiv`, `__aullrem`, `__alldiv`, `__allrem`); the assembly half supplies
the `__chkstk`/`___chkstk_ms` stack probes and the `__allshl`/`__aullshr`/
`__allshr` shift helpers, whose calling convention (count in `cl`, value in
`edx:eax`) C cannot express. The 64-bit helpers are spelled `_allmul` and so
on in C on purpose: the compiler's own reference to a helper is undecorated,
so a C definition of `__allmul` would produce `___allmul` and never resolve.
None of the library is linked eagerly — the packer's reachability pass drops
every section the module does not call.

There is no libc and no floating-point library. On i386 the kernel does not
save x87 or SSE state across a task switch and never sets `CR4.OSFXSR`, so a
32-bit module must not touch either; the build passes `/arch:IA32` (or
`-mno-sse -mno-sse2 -mno-mmx`) to keep the compiler from doing it silently. On
x86-64, SSE2 is part of the ABI, so `sched64_init` enables the FPU and every
switch `FXSAVE`s/`FXRSTOR`s the task; compiler-emitted SSE2 is therefore safe,
but a module still has no math library and must not assume x87. Module code
runs in ring 0 on the calling task's kernel stack, which is two 4 KiB pages on
i386 and 16 KiB on x86-64, so frames stay small and a module never blocks,
sleeps, or spins waiting for another task. A module that needs a large buffer
takes it from `haku_module_alloc`, not from the stack.

`build.py` compiles the 32-bit modules with

```
cl /nologo /c /GS- /Zl /Os /Gy /W3 /arch:IA32 /Iinclude
```

and the x86-64 demo with the x64 compiler and its own runtime:

```
cl /nologo /c /GS- /Zl /Os /Gy /W3 /Iinclude /Ikernel\module \
   /Ikernel\arch\x86_64\compat
nasm -f win64 kernel\module\lib\runtime64.asm -o build\x64_modruntime.obj
```

Each one is packed with the library objects:

```
python tools/mkhakumod.py build/mod_hello.obj build/modlib_module_lib.obj \
       build/modlib_runtime.obj build/HELLO.HKM --name HAKU-HELLO
python tools/mkhakumod.py build/x64_demo.obj build/x64_modlib.obj \
       build/x64_modruntime.obj build/X64MOD.HKM --name HAKU-X64
```

`/Gy` (one COMDAT per function) is required for the dead-code pass to be
useful; `/GS-` removes the security cookie, which would need a CRT; `/Zl`
keeps the default library out.

#### Capabilities owned by a module

A module is more than code: it can expose kernel functionality. There are two
capability kinds, and the module slot owns them. This layer belongs to the
32-bit kernel; the x86-64 build exports the same calls so one header serves
both, but answers `-ENOSYS` because it has no VFS or devfs yet. The two kinds
are:

- a character device, registered in devfs and reached as `/dev/<name>`;
- a filesystem backend (`vfs_fs_ops_t`, which `haku_module_fs_ops_t` mirrors
  in the public header), mounted once at a non-root prefix.

Registration is only valid while that slot's init hook is running. The loader
sets a single `module_api_owner` token around the hook, so the three
registration calls answer `-EPERM` from any other context — including the
module's own exit hook, which runs with no token. Each slot may own at most one
device and one filesystem, and may mount that filesystem once. The root prefix
is refused: a module adds a namespace instead of replacing the boot one.

Unloading is ordered so freed module code is never reachable. `delete_module`
first detaches the mount, then unregisters the filesystem, then removes the
device. Each step checks its own liveness:

- `vfs_unmount` refuses the root, reports `-ENOENT` when no backend is mounted
  at the prefix, and reports `-EBUSY` while any open file description carries
  that mount's index. The slot keeps its index but is skipped by path
  resolution and reused by the next `vfs_mount`, so descriptors that were
  already open never observe a shifted index.
- `vfs_unregister_fs` reports `-EBUSY` while a mount still points at the
  `vfs_fs_ops_t` and only then compacts the backend table (`VFS_MAX_FS` is 8,
  up from 4, so a module filesystem fits).
- `devfs_unregister_device` reports `-EBUSY` while a descriptor is open on the
  device. The devfs table has six built-in slots plus four module slots, and
  each slot carries an `opens` refcount; a module name is copied into kernel
  storage because the module image is freed at unload.

If any of those checks fails, `delete_module` restores `MODULE_LIVE` and
returns the error, leaving the module fully usable; closing the last descriptor
makes the next unload succeed. A failing init hook gets the same teardown, and
if a concurrent open pins the fresh resource the module stays `LIVE` and
`init_module` reports `-EBUSY` rather than freeing an image that is still in
use.

`build.py` packs three modules for the 32-bit image and installs them in both
the initramfs and the FAT12 seed disk:

- `MODULE.HKM` from the NASM object `haku_demo.asm` keeps the ELF32 host path
  alive and proves relocation and logging
  (`HAKU-MODULE-OK`/`HAKU-MODULE-EXIT`);
- `HELLO.HKM` from `hello.c` is a C device module: it registers `/dev/hello`, a
  read-only character device that returns `hello from a haku module\n`
  (`HELLO-MODULE-OK`), and before that it checks the runtime it is linked with
  — the string routines and the 64-bit multiply, divide and remainder helpers,
  against volatile operands the optimizer cannot fold — so a broken helper
  fails the load instead of corrupting a read later (`HELLO-LIB-OK`);
- `ROMFS.HKM` from `romfs.c` is a C filesystem backend that registers a
  read-only filesystem and mounts it at `/rom` with `README` and `VERSION`
  (`ROMFS-MODULE-OK`).

The x86-64 build packs a fourth, `X64MOD.HKM` from `x64demo.c`. It is seeded on
the x64 data disk rather than installed in an image, so the boot test exercises
the whole on-disk path: read the file, validate the version 2 container,
relocate `R_64_ABS`/`R_64_PC32` (including a pointer table that becomes
`DIR64` relocations), resolve imports, run the init hook, then unload and free
the image. Its checks cover the library's strings, a heap round trip, the tick
counter and a pointer table (`HAKU64-MODULE-OK`, `HAKU64-MODULE-LIB-OK`,
`HAKU64-MODULE-LOAD-OK`, `HAKU64-MODULE-UNLOAD-OK`); a duplicate load, a
truncated file and a corrupted magic must all be refused before
`HAKU64-MODULE-REJECT-OK` is printed.

The user-library test drives each of the three: it opens the device and the
files, checks that `delete_module` reports `-EBUSY` while a descriptor is open,
closes it, unloads, and reloads to prove the slots are reusable
(`USER-MODDEV-OK`, `USER-ROMFS-OK`).

Modules are trusted kernel code. The validation is a container check: it stops
a truncated or corrupt file from making the loader read out of bounds, but it
cannot sandbox code written maliciously, because the module runs in ring 0 with
the kernel's privileges. Both kernels allocate images from a heap that is
already executable — the 64-bit heap lives in the boot identity map and the
32-bit heap in its direct map — so executability is not what gates the feature;
the remaining gap is the 64-bit VFS/devfs that capability registration needs.

### Physical frame allocator

`kernel/arch/x86/mm/frame.c` now builds its usable-frame bitmap from the 64-bit base and
length fields in the E820 entries. Only complete 4 KiB pages wholly inside
type-1 usable ranges are admitted; invalid entries and overlapping reserved
ranges are excluded. The allocator explicitly reserves low boot/firmware
memory, the kernel's 448 KiB load window, page tables, the boot stack, the
direct-map tables, the ISA hole, and the 1–4 MiB heap. The pool is sized from
the memory map instead of a fixed limit: `paging_init` reads the highest
usable E820 address, rounds it up to a 4 MiB boundary, and maps exactly that
much, so a 512 MiB guest gets a 512 MiB pool and a 2 GiB guest gets the
1000 MiB the 32-bit address space can address once the local-APIC page and
the ACPI scratch window are reserved. `frame_usable_count()` reports
the E820-usable pages inside that window, and every tracked frame is
allocatable: a permanent supervisor direct map at `0xC0400000` addresses
physical `0–1000 MiB`, and its PDEs are copied into each user page directory.
The bitmaps, the direct-map page tables, and the per-frame reference counters
are all sized at boot (heap allocations and low RAM), so the kernel image does
not carry a worst-case array. The self-test chain-allocates past the old
8 MiB identity boundary, writes/reads a high frame through `PHYS_TO_VIRT`,
and probes the last usable frame, which fails if the map stops short of the
installed RAM. Freeing validates alignment, usability, and allocation state
so reserved pages cannot accidentally be released.
