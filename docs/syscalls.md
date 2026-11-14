# System calls and the POSIX subset

[Back to documentation index](README.md)

The i386-compatible syscall ABI, memory-management calls, the implemented
POSIX surface, and the self-test markers that cover them.

The x86-64 kernel serves the ported subset under the x86-64 Linux numbers with
`rdi`/`rsi`/`rdx`/`r10`/`r8`/`r9` arguments and `rax` for the number and the
result, entered by `SYSCALL` with an `INT 0x80` fallback. This page documents
the 32-bit table in full because it is the complete one; the 64-bit image still
lacks pipes, most VFS backends (it has a FAT12 root and `/dev`) and signal
handlers.

## Syscall ABI

The syscall ABI follows
the i386 Linux syscall numbers for a small POSIX-oriented subset: `exit` (1),
`fork` (2), `read` (3), `write` (4), `open` (5), `close` (6), `waitpid` (7),
`unlink` (10), `execve` (11), `lseek` (19), `getpid` (20), `rename` (38),
`mkdir` (39), `rmdir` (40), `dup` (41), `brk` (45), `dup2` (63),
`symlink` (83), `readlink` (85),
`gettimeofday` (78), `mmap` (90), `munmap` (91), `stat` (106), `lstat` (107),
`fstat` (108), `wait4` (114), `clone` (120), `mprotect` (125),
`_llseek` (140), `readv` (145), `writev` (146),
`nanosleep` (162), `mremap` (163), `vfork` (190), `mmap2` (192),
`stat64` (195), `lstat64` (196),
`fstat64` (197), `madvise` (219), `exit_group` (252), `set_thread_area` (243),
`get_thread_area` (244), `set_tid_address` (258), `fstatat64` (300),
`uname` (122),
`getrusage` (77), `times` (43), `getppid` (64), `getuid` (24), `geteuid` (49),
`getgid` (47), `getegid` (50), and `statx` (383), with arguments in
`ebx`, `ecx`, `edx`, `esi`, and `edi` and syscall number/result in `eax`.
`_llseek` takes and returns a 64-bit offset through a user pointer, and
`readv`/`writev` walk at most `IOV_MAX` (1024) eight-byte i386 `iovec`
entries; a libc's stdio writes through `writev`, so a static libc prints
nothing without it.
The signal calls use the same numbers: `kill` (37), `rt_sigreturn` (173),
`rt_sigaction` (174), `rt_sigprocmask` (175), `tkill` (238), and `tgkill`
(270).
The Haku-specific `spawn` and `getusage` syscalls use `0x1000` and `0x1001`.
The public ABI definitions are in `include/haku/abi.h` for C and
`include/haku/abi.inc` for NASM; the build checks that their numeric
constants stay synchronized. They define the syscall numbers (the ones
above plus the Linux numbers the coming milestones add, which answer
`ENOSYS` until then), errno values, open flags, and the metadata layouts.
`haku_stat_t` is the 64-byte i386 Linux `struct stat` returned by `stat`,
`lstat`, and `fstat`; `haku_stat64_t` is the 96-byte i386 `struct stat64`
returned by `stat64`, `lstat64`, `fstat64`, and `fstatat64` — the layout
glibc and musl use on i386 — and `haku_statx_t` is the 256-byte `struct
statx` returned by `statx`, the call musl makes for `stat()` on i386.
Regular files report mode `0644` (`S_IFREG`), directories `0755`
(`S_IFDIR`), and the metadata carries the link count, owner, size, 512-byte
block size, and block count.
`haku_timeval_t` (`tv_sec`, `tv_usec`) and `haku_timespec_t`
(`tv_sec`, `tv_nsec`) are the 8-byte user time structures. `haku_mmap_arg_t`
is the 24-byte i386 `old_mmap` argument block.

`brk` grows or shrinks a per-process heap starting at a reserved virtual base
(`0x50000000`): the break is page-rounded and pages are mapped zero-filled as
it rises; lowering it unmaps the released pages and returns their frames; a
below-base break gives `EINVAL` and one past the heap limit gives `ENOMEM`.
`mmap` takes a pointer to an `old_mmap` argument block in `ebx` and
`mmap2` (192) takes the same values in registers; both map anonymous pages
only (`MAP_ANONYMOUS` is required, because Haku has no page cache yet) and
place mappings in a second reserved region (`0x60000000`) using first-fit over
a per-process table, so earlier releases are reused. `MAP_FIXED` places a
mapping at the given page-aligned address, which must be a free range inside
the mmap window: the image, the stack, and the heap are never replaced.
`MAP_SHARED` and `MAP_PRIVATE` are both accepted; a shared mapping is
indistinguishable from a private one until a second task can map it.
`munmap` (91) releases a range that lies inside
one tracked anonymous mapping, trims or splits the region, and frees the pages
(and any now-empty page tables). `PROT_WRITE` selects the PTE `R/W` bit, so a
`PROT_READ` mapping is enforced read-only by the MMU, and `PROT_NONE` keeps
the pages mapped but clears the user bit, so any access faults. Both regions
are reclaimed with the rest of the address space at exit or `execve`.

`mprotect` (125) changes the protection of a mapped range: `PROT_READ` clears
the `R/W` bit, `PROT_WRITE` sets it, and `PROT_NONE` clears the user bit so
the range faults on the next access. A range that is not mapped reports
`ENOMEM` the way Linux does, while a bad length, a bad `prot`, or an address
outside the mapping window reports `EINVAL`. A copy-on-write page keeps its
shared frame protected and remembers that its mapping is writable, so the first write
still produces a private copy instead of writing through to the parent.
`mremap` (163) grows, shrinks, or moves a mapping: without `MREMAP_MAYMOVE` a
mapping may only grow in place, and with it the mapping moves to a free range
and keeps its contents (the pages are copied through the direct map).
`MADV_DONTNEED` (219) releases the pages of a range, so the next access sees
zeroed memory again; the other advice values are hints and change nothing.
`set_thread_area` (243) points the user `FS` segment at the thread block the
program passes, which is the thread pointer a libc reads through `%fs:0`; the
GDT entry carries the base and every task switch restores the base of the
task that is about to run. `get_thread_area` (244) returns it again. The call
follows the Linux contract so a foreign libc can use it: the caller passes a
16-byte i386 `user_desc` with `entry_number` set to `0xFFFFFFFF`, the kernel
picks a fixed entry (6, selector `0x33`), writes the chosen number back into
the structure, and returns 0; musl then loads `entry * 8 + 3` into `%gs` and
reads its thread pointer from `%gs:0`. The nonzero return Linux does *not*
use matters: musl's stub treats any nonzero result as a failure and falls
back to `modify_ldt`. The base is also mirrored into `FS`, so Haku's own
library can keep using `%fs`. The
library wrappers live in `user/lib/mman.c` behind `include/haku/mman.h`, and
the ring-3 self-test covers the fixed mapping, the protection changes, the
fault on a read-only page, the move, the advice call, and the thread pointer
(`USER-MMAP-OK`).

The kernel sets `CR0.WP`, so ring-0 writes also honour `R/W=0`: read-only user
pages can only be filled through their physical direct-map alias. ELF segments
are mapped from their `PF_W` bit (the loader already rejects `W|X`), and
`munmap` clears the PTE before returning the frame, so both an `RX` store and a
store through a released mapping take a page fault. i386 without PAE has no
execute-disable bit, so W^X is enforced by the loader's `W|X` rejection rather
than by an NX permission.

## User-mode system calls

`read` supports stdin from the PS/2 keyboard and regular files; `write`
supports stdout/stderr to the active display and serial as well as writable
regular files. The standard descriptors `0`/`1`/`2` are real per-process
descriptors backed by a registered console device rather than special cases
in the syscall layer: they resolve through the same VFS descriptor table as
file handles, and the device itself only forwards to the terminal layer.
`open`, `close`, `lseek`, `unlink`, `rename`, `mkdir`, `rmdir`, `stat`,
`fstat`, and per-process file descriptors are backed by the VFS/FAT12 layer.
Paths are `/`-separated 8.3 names: a relative name starts at the process
working directory, an absolute one at the volume root, `.` keeps the current
directory, and `..` climbs to the parent (clamped at the root). `stat`
and `fstat` report byte size and mode (`S_IFREG | 0755` for files,
`S_IFDIR | 0755` for directories — FAT12 has no permission bits, and the
execute bits are what let a shell run an applet it found through `$PATH`)
through the Linux-shaped 64-byte `struct stat`. Reads and writes accept any
count: one above the 4 KiB internal window is served as a short read or a
short write, which is legal, so a libc that hands `cat` a 64 KiB buffer
works unchanged (the old `EINVAL` there made every `cat`, `grep`, and `ls`
on a large file fail). Errors return negative errno values (the user
library in `user/lib` translates these to `errno`).
Every FAT12 failure class keeps one distinct errno end to end: missing
files give `ENOENT`, bad flags/whence/pointers give `EINVAL`, existing
files give `EEXIST`, bad descriptors give `EBADF`, a full volume gives
`ENOSPC`, a non-directory path component gives `ENOTDIR`, opening or
unlinking a directory gives `EISDIR`, removing a non-empty directory gives
`ENOTEMPTY`, and disk I/O failures give `EIO`. A root mounted with
`VFS_MOUNT_RDONLY` rejects create, write, truncate, unlink, rename, mkdir,
and rmdir with `EROFS` while reads and metadata keep working. After the
user process exits,
the idle task runs an error self-test (`FS-ERRORS-OK`) including a
root-fill probe that must fail with `ENOSPC` (`FS-ENOSPC-OK`). The same
pass also remounts read-only, checks every mutating path returns `EROFS`,
remounts read-write (`FS-EROFS-OK`), covers empty files, exact-sector and
boundary-spanning I/O
(`FS-FILE-EDGE-OK`), injected cut/bad/reserved/cyclic FAT chains (each
restored, reads must give `EIO`, loops must terminate) plus a crafted
full data area (`FS-CORRUPT-OK`), and ATA bounds/NULL-transfer rejection
(`FS-ATA-OK`). The same pass checks the metadata behind `stat`/`fstat`:
type bits, size, link count, block size, and block count for a file and a
directory (`FS-STAT-OK`).

`gettimeofday` reports whole seconds and microseconds since boot derived
from the 100 Hz PIT tick counter; a NULL timeval is allowed (a no-op query),
an unmapped one gives `EFAULT`, and the unused timezone argument is ignored.
`nanosleep` rounds the requested interval up to the next timer tick, marks
the calling task `TASK_SLEEPING`, switches away, and lets the timer IRQ wake
it when its deadline passes, so it does not busy-wait. A zero interval
succeeds immediately; `tv_nsec` outside `[0, 1e9)` gives `EINVAL` and a NULL
request gives `EFAULT`. A pending signal interrupts a sleep: the task
wakes with `EINTR`. The number of sleeping tasks is not counted against idle, so a
sleeping process keeps the idle loop in the scheduler.

Blocking events use wait queues (`kernel/kernel/scheduler.h`). A queue is a
bitmap over scheduler task slots, so it needs no allocation and cannot fail.
`wait_queue_sleep` marks the caller `TASK_BLOCKED` with interrupts still
disabled; the caller then executes `sti; hlt`, so either the event IRQ returns
straight to the halted task or the timer IRQ saves that frame and switches to
another task. A blocked task is off the run queue, so when nothing else is
runnable the scheduler selects the idle context instead of rescheduling it.
`wait_queue_wake_all` is called from IRQ context and marks every parked task
runnable; waiters re-check their condition after resuming. Keyboard input is
the first user of this: IRQ1 pushes the character into the terminal's input
ring and wakes its readers, and `SCHED-WAITQ-OK` proves that a read waiting
for a key left the run queue and let idle run. `nanosleep` and `waitpid` keep
their existing `TASK_SLEEPING`/`TASK_WAITING` states; the blocked state is
deliberately separate so a child exiting cannot wake a task that is waiting
for console input.

Signals are implemented for the standard range 1..31 (`kill`, `tkill`,
`tgkill`, `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`). Each task keeps
a pending set, a blocked set, and one disposition per signal; the sets are
single words, so the mask fits in 32 bits (the real-time range 32..64 is
rejected with `EINVAL`). `rt_sigaction` takes the i386 kernel layout
(handler, flags, restorer, mask) and accepts a `sigsetsize` of 4 or 8, so
musl's eight-byte `sigset_t` call works unchanged. `SA_SIGINFO` handlers
receive the `siginfo` block and the `ucontext` the kernel saved;
`SA_RESTORER` uses the program's own restorer, and without one the kernel
installs a stub that calls `rt_sigreturn`. `SIGKILL` and `SIGSTOP` cannot be
caught or ignored (`EINVAL`).

A queued signal is delivered when the task returns to user mode, which
happens on the next syscall, interrupt, or timer tick. The kernel builds a
frame on the user stack (restorer, three handler arguments, the saved
registers, the `siginfo` block, and the `ucontext`), rewrites the return
context to enter the handler, and restores it in `rt_sigreturn`. The signal
is blocked while its handler runs unless `SA_NODEFER` says otherwise, and
the blocked set is restored when the handler returns. `SIG_IGN` discards the
signal, `SIG_DFL` ends the process for every signal except `SIGCHLD`,
`SIGURG`, `SIGWINCH`, and `SIGCONT` (which are ignored); a parent then sees
the signal number in the wait status the way Linux reports it. A signal
queued for a task blocked in a syscall wakes it with `EINTR` (the console
read loop re-checks the input ring instead of returning `EINTR`). `kill`
with pid 0 targets the caller, pid -1 targets every other process, and any
other negative pid targets the members of that process group (`killpg`).
`fork` and
`vfork` reset the dispositions to the default and keep the blocked set;
`execve` resets the dispositions and keeps both the blocked set and the
pending signals. The C library wrappers live in `user/lib/signal.c` behind
`include/haku/signal.h`, and the ring-3 self-test covers delivery, blocking,
`SIG_IGN`, `SA_SIGINFO`, `SA_RESTORER`, the pid/tid forms, the default
fatal disposition, and the `EINTR` wake-up (`USER-SIGNAL-OK`). Process
groups exist for signal delivery and the terminal, but stop/continue
signals still end a process, so there is no job control beyond the
foreground group. There is no `sigqueue`, and one thread runs per process,
so a thread id is a process id.

Writes are not atomic: there is no journal, so a failing multi-sector
write leaves exactly its durable prefix (a short count means those bytes
are on disk and the size covers them), while a total failure restores
the pre-call size and releases clusters the call allocated
(`FS-ROLLBACK-OK`). Bytes are never attempted past a gap that failed to
zero, so no unwritten holes are ever claimed. If persisting the directory
entry itself fails, the call rolls back to the pre-call metadata rather
than leaking unreachable clusters; a TRUNC whose chain cannot be freed
zeroes the entry best-effort so the file reads back empty instead of
pointing at a broken chain. Anything beyond that best effort (for
example, clusters orphaned by a dying disk) stays leaked: there is no
fsck yet.

Open files survive removal and renaming: `unlink` deletes the name at
once while open descriptors keep reading, writing, and seeking the data
(clusters are released at the last such close); `rename` moves the entry
and repoints open descriptors to it, so they keep working under the new
name. Renaming onto an existing name fails with `EEXIST`, renaming a name
onto itself succeeds as a no-op, and every other missing name gives
`ENOENT`. The user test program proves all of this from ring 3 in both
processes (`USER-FILE-OPS-OK`).

`dup` and `dup2` point a second descriptor at the same open file
description: the copy shares the byte offset and access mode, and the file
stays open until the last descriptor closes, so closing the original leaves
the duplicate usable (and an open-on-a-removed-file survives to its final
close). `dup` returns the lowest free descriptor, `dup2` closes and reuses a
free or already-open target and is a no-op when both descriptors match, and
both report `EBADF` for an invalid source without disturbing the target. The
user program proves the shared offset, the surviving duplicate, and the
error cases in both processes (`USER-DUP-OK`).

Because `0`/`1`/`2` are ordinary descriptors, `dup2` can redirect stdout to a
file (a shell-style redirect) and `close` revokes a standard descriptor so
later I/O returns `EBADF` until `dup2` restores it. `spawn` copies the
parent's descriptors into the child, and every exit path releases the exiting
process's descriptors, so files a child leaves open are closed automatically
when it terminates. The user program and its child prove the redirect,
revoke, inherited-descriptor, and close-on-exit paths (`USER-STDIO-OK`,
`USER-FD-INHERIT-OK`); after both processes exit, the idle self-test asserts
their descriptor tables are empty.

A process also has a working directory. `chdir` resolves the target, requires
a directory, and stores the result as a canonical root-relative path (no
leading slash, no `.` or `..` components, empty for the root); `getcwd`
rebuilds `/` plus that path. Every path syscall resolves relative names
against it, so `open("data.txt")` and `execve("NEXT.ELF", ...)` follow the
process, while a leading `/` still starts at the volume root. The root itself
canonicalizes to `.` for the VFS, which has no name for it. `fork`, `vfork`,
and `spawn` children inherit the directory, and `execve` keeps it.

The `*at` family takes a directory descriptor: `openat`, `unlinkat` (with
`AT_REMOVEDIR`), `renameat`, `mkdirat`, `faccessat`, and the `dirfd` form of
`fstatat64`/`statx` join a relative name to the directory the descriptor was
opened on, and accept `AT_FDCWD` for the working directory. `open` with
`O_DIRECTORY` returns a descriptor for enumeration: `getdents64` fills the
user buffer with packed Linux `linux_dirent64` records (inode, offset,
record length, type, name) from the backend's `opendir`/`readdir` pair, and a
closed directory handle releases a directory object rather than a file one.
Enumeration skips deleted slots, long-name fragments, volume labels, and
`.`/`..`, so it lists the names a user would see.

`pipe`/`pipe2` create a 512-byte in-memory pipe with two open file
descriptions and separate reader/writer counts: a read returns `0` once
every write end closes, a write to a pipe with no readers raises `SIGPIPE`
and returns `EPIPE`, and `O_NONBLOCK` (set at creation or through
`fcntl(F_SETFL)`) makes both directions return `EAGAIN` instead of blocking.
Blocking pipe reads park on a wait queue like the console does. `fcntl`
answers `F_GETFD`/`F_SETFD`, `F_GETFL`/`F_SETFL` (`O_APPEND`, `O_NONBLOCK`),
and `F_DUPFD`/`F_DUPFD_CLOEXEC`; `F_GETLK` reports "unlocked" and
`F_SETLK`/`F_SETLKW` succeed as no-ops, because Haku tracks no record locks.
`dup3` is `dup2` with an `O_CLOEXEC` flag check and rejects identical
descriptors with `EINVAL`.

`ioctl` on the console answers `TCGETS` with the terminal's fixed mode
(`ICRNL|IXON`, `OPOST|ONLCR`, `ISIG|ICANON|ECHO|ECHOE|ECHOK`, erase `0x7F`,
EOF `^D`), accepts `TCSETS`/`TCSETSW`/`TCSETSF` without changing that mode,
reports an 80x25 window for `TIOCGWINSZ`, and returns `ENOTTY` for a
descriptor that is not the console. A terminal opened by path (`/dev/tty`,
`/dev/console`) answers the same requests. `TIOCSPGRP` records the
foreground process group after checking that the group is alive, and
`TIOCGPGRP` reports it (falling back to the caller's group when the stored
group has exited); `TIOCNOTTY` is accepted. The group and session calls are
real: `getpgrp`/`getpgid` report the recorded group, `setpgid` moves the
caller or a child, and `setsid`/`getsid` track the session, which is what an
interactive `ash` needs to turn job control on and run a foreground pipeline
instead of dying with "can't set tty process group". `poll` reports readiness per descriptor (a
pipe with data is `POLLIN`, a closed write end adds `POLLHUP`, a closed read
end makes the write side `POLLERR`), and it blocks: a zero timeout scans once,
a negative one waits indefinitely, and a positive one is a millisecond
deadline. The wait is driven by the timer rather than by per-descriptor
wakeups — every tick wakes tasks parked on the scheduler's activity queue, so
they re-scan at 100 Hz and read the same tick counter as the clock. That costs
at most one tick of latency and one wake per parked poll per tick, and needs
no console/pipe plumbing. `select` (both i386 numbers, 142 `_newselect` and 82,
which musl uses) converts its three sets into the same readiness scan; Haku
has no exceptional conditions, so the except set always comes back empty. The
legacy `getdents` record format is produced too, by rewriting the `getdents64`
records in place: each becomes `{ u32 ino, u32 off, u16 reclen, name }` padded
to four bytes, which is shorter than the 64-bit record, so a caller with a
fixed buffer keeps calling until the call returns 0. `access` checks that the
name resolves and
refuses `X_OK` because nothing is marked executable; `fchmod`, `fchown`,
and their `*at` forms succeed without changing anything because FAT12 stores
no mode or owner bits. `getrlimit`/`setrlimit`/`prlimit64` report the fixed
table sizes (32 descriptors, 4 tasks, `RLIM_INFINITY` elsewhere) and accept a
new value without applying it. `getrandom` fills a buffer from a xorshift
generator seeded by the tick counter and the pid, `sched_yield` hands the
CPU to the next runnable task, `prctl` stores a 15-character process name
and accepts the remaining options as no-ops, and `personality` always
reports `PER_LINUX`. The C library covers all of it
(`include/haku/dirent.h`, `poll.h`, `termios.h`, `resource.h`, and the
`*at` wrappers in `fcntl.h`), and the ring-3 self-test exercises the whole
path as `USER-FS-ABI-OK`.

Not implemented: real record locks (`F_GETLK`/`F_SETLK`/`F_SETLKW` are
accepted without tracking anything) and `mremap` with `MREMAP_DONTUNMAP`.

Loadable modules use the Linux i386 numbers `init_module` (128) and
`delete_module` (129), but the image is the packed `.hkm` container described
in [architecture](architecture.md#haku-modules), not an ELF object.
`init_module(image, len, params)` copies at most 73 800 bytes of the caller's
image into the kernel heap, validates and relocates it, runs its
`haku_module_init`, and keeps it if that returns zero (a negative return is
propagated; any other non-zero return becomes `-EINVAL`). `params` has no
meaning and is accepted only when it is NULL or a readable pointer; the call
reports `-E2BIG` for an oversized buffer and `-EINVAL` for a bad container.
`delete_module(name, flags)` requires `flags == 0`, copies at most 24
characters of the name page by page, and reports `-ENOENT` for a name that is
not loaded. A module that registered a device or a filesystem is detached
first: the call reports `-EBUSY` while a descriptor opened through that device
or mount is still live, and leaves the module loaded and usable. When the
teardown succeeds it runs `haku_module_exit` (which can no longer register
anything) and frees the image. The calls are not restricted by uid — Haku has a
single privileged user — but module code runs in ring 0, so only a loader that
already trusts the image should call `init_module`.

The x86-64 kernel serves the same two operations under the x86-64 Linux
numbers, `init_module` (175) and `delete_module` (176). The container must be
version 2; the length bound and page-wise copies are the same, and the loader
relocates the 64-bit forms. A 64-bit module can log, read ticks and use the
kernel heap, but its capability calls answer `-ENOSYS` until there is a 64-bit
VFS, so `delete_module` has nothing to detach.

Before copying, the kernel validates every covered page in the current page
tables for presence, user access, and write permission where required. It
rejects unmapped/kernel pointers, wraparound, invalid descriptors and
oversized I/O. The syscall entry enables interrupts while in kernel code so
blocking keyboard reads can make progress; the keyboard wait is atomic
against the interrupt/wakeup race. Ring-3 page faults are contained and
reported rather than halting the kernel: the fault handler prints a crash
report (vector, error code, `CR2`, control registers, general registers, and
the validated user stack), encodes the error code in the exit status byte
(`0x80 | err`), and terminates only the faulting process. These syscalls are an
initial POSIX-inspired kernel interface, not full POSIX conformance. Run
`python build.py headless` to test read/write/open/close/lseek/getpid/exit,
gettimeofday/nanosleep, brk/mmap/munmap, per-process `getusage` memory and
resource accounting, bad pointers, descriptor errors, input copying,
disk-backed create/write/read across a sector boundary, and continued
timer operation. The protection test spawns one child per fault
(`USER-PROT-OK`): a store into an `RX` segment, a store into a `PROT_READ`
mapping, a read from unmapped memory, and a store through a `munmap`ed page,
each checked against its expected `#PF` error code (`7`, `7`, `4`, `6`). The
blocking read is also observed to park the process (`SCHED-WAITQ-OK`) before
the injected key arrives.

The error matrix (`USER-ERRNO-OK`) drives each syscall family with a bad
descriptor, an invalid user pointer, a missing path, and a bad argument, and
requires the documented errno instead of a success, a kernel fault, or an
unset return value: `EBADF`, `EFAULT`, `ENOENT`, `EEXIST`, `EISDIR`,
`ENOTDIR`, `ENOTEMPTY`, `EINVAL`, `ERANGE`, `ENOTTY`, `EPIPE`, `ESRCH`,
`ECHILD`, and `ENOMEM`. The ELF rejection test (`ELF-REJECT-OK`) breaks one
header or program-header field at a time — class, byte order, machine, entry,
table geometry, `p_offset`, `p_vaddr`, `p_filesz` against `p_memsz`, `PT_INTERP`,
and the writable+executable combination — and requires the loader to reject
every one of them, plus a truncated image. `SCHED-CONCURRENT-OK` runs two
children that create, verify, and unlink their own files at the same time,
which exercises the sleeping VFS and disk mutexes end to end.
`USER-INITRAMFS-OK` is the boot-namespace check described in
[storage and filesystems](storage.md#the-boot-namespace-initramfs-ramfs-dev-and-proc):
it runs as a process started from the unpacked archive, so
a broken mount table or a missing `/init` fails the build before anything
else can pass.

## Supported POSIX subset

Haku implements a POSIX-*inspired* subset aimed at a statically linked musl
userland; conformance is not a goal. What exists, by area:

- **Process:** `exit`/`exit_group`, `fork`, `vfork`, `clone` (fork form),
  `wait4`/`waitpid`, `execve`, `getpid`/`getppid`/`getuid`/`geteuid`/
  `getgid`/`getegid`/`gettid`, `getpgrp`/`getpgid`/`setpgid`/`setsid`/
  `getsid`, `kill`/`tkill`/`tgkill`, `rt_sigaction`/
  `rt_sigprocmask`/`rt_sigreturn`, `sched_yield`, `prctl` (process name),
  `personality`, `times`, `getrusage` (zeroed), `uname`,
  `getrlimit`/`setrlimit`/`prlimit64` (fixed values), `getrandom`.
- **Memory:** `brk`, `mmap`/`mmap2` (anonymous only), `munmap`, `mprotect`,
  `mremap`, `madvise`, `set_thread_area`/`get_thread_area`.
- **Files:** `open`/`openat`, `close`, `read`, `write`, `lseek`,
  `dup`/`dup2`/`dup3`, `pipe`/`pipe2`, `fcntl`, `ioctl` (console),
  `unlink`/`unlinkat`, `rename`/`renameat`, `mkdir`/`mkdirat`, `rmdir`,
  `stat`/`lstat`/`fstat` (32- and 64-bit), `fstatat64`, `statx`,
  `symlink`/`readlink` (on the RAM root),
  `truncate`, `ftruncate`, `access`/`faccessat`, `chdir`, `getcwd`,
  `getdents`, `getdents64`, `poll`, `select`, `utimensat` (accepted,
  timestamps are not stored), and
  `chmod`/`chown`/`fchmod`/`fchown` (accepted,
  nothing to change).
- **Time:** `gettimeofday`, `nanosleep`.
- **Modules:** `init_module` (packed `.hkm` image, params ignored; the module
  may register a device and mount a filesystem while its init hook runs),
  `delete_module` (detaches those resources first, reports `-EBUSY` while a
  descriptor still uses them, then runs the exit hook and frees the image).
- **Haku-specific:** `spawn`, `getusage`, `futex`.

Deliberately absent: job-control stop/continue states (the group and session
calls are real, but stop signals still end a process), hard links, file-backed
mappings, `socket`, a user-facing `mount` (users cannot reparent the boot
namespace; loadable modules mount their own prefix during init), networking,
pipes on x86-64, most of the 64-bit namespace (FAT12 at `/` and devfs at
`/dev` exist; ramfs, procfs and initramfs do not), and the capability half of
the x64 module API (registration answers `-ENOSYS`).
`clone`-created threads are present: a thread shares the caller's address
space, descriptors and signal dispositions, reports the process id from
`getpid` and its own id from `gettid`, and the thread form is exercised as
`USER-THREAD-OK`.
Where a call exists but cannot be honoured — permission bits, record locks,
terminal modes — it succeeds
without changing state and the limitation is documented above instead of
returning a surprise error.
