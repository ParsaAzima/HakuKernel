# HakuKernel Roadmap

This roadmap separates implemented capabilities from planned work. HakuKernel
aims to be a small, testable x86 operating system that grows toward running
independent programs and providing a POSIX-inspired environment. The long
term direction is a Linux-compatible syscall ABI plus a statically linked
GNU userland (musl, e.g. busybox) booted from an initramfs; full POSIX
compatibility is not a short-term goal.

## Current Status

- [x] BIOS boot into 32-bit protected mode, with a bootloader that can load a
  kernel larger than the original boot-sector limit.
- [x] E820-backed physical memory inventory, two-level paging with a
  memory-map-sized supervisor direct map, and separate user address spaces.
- [x] Size the frame pool and direct map from the detected RAM instead of a
  fixed 128 MiB (`paging_init` reads the highest usable E820 address and
  maps that much up to the 1020 MiB the 32-bit address space can reach;
  bitmaps, direct-map tables, and frame reference counters are sized at boot,
  `MEM-OK` reports the result, and `/proc/meminfo` describes physical memory
  rather than the kernel heap).
- [x] Static ELF32 programs running at ring 3, per-process kernel stacks, and
  PIT-driven preemptive scheduling.
- [x] `exit`, `read`, `write`, `open`, `close`, `lseek`, and `getpid` syscalls
  with user-pointer validation and `-errno` error returns.
- [x] Process lifecycle syscalls: `spawn`, `waitpid`, `execve`, `dup`/`dup2`,
  and per-process descriptors with inheritance and close-on-exit.
- [x] User virtual memory: a `brk` heap that grows and shrinks, anonymous
  private `mmap`, and `munmap` that trims, splits, and releases mappings for
  later reuse.
- [x] Time and accounting: `gettimeofday`, `nanosleep`, and the Haku-specific
  `getusage` snapshot of per-process memory and CPU usage.
- [x] ATA driver for both legacy channels (primary/secondary) and all
  master/slave positions, LBA28 with LBA48 fallback, bounded polling with
  software-reset retry recovery, and FLUSH CACHE after writes.
- [x] Writable FAT12 for 8.3 files and subdirectories (paths with `.`/`..`):
  create, read, write, truncate, append, mkdir/rmdir, with a VFS and
  per-process file descriptors.
- [x] QEMU headless integration tests covering multi-sector I/O, seeking,
  append, descriptor access checks, spawn/waitpid exit status, exec
  argument/environment stacks, per-process usage accounting, and persisted
  disk data.
- [x] Sleeping locks for the filesystem and the disk (a parked waiter costs no
  CPU and interrupts stay enabled through a transfer), plus a read cache in
  front of the drive (`ATA-CACHE-OK`).
- [x] A ring-3 error matrix for every syscall family (`USER-ERRNO-OK`), an
  expanded malformed-ELF suite (`ELF-REJECT-OK`), and a two-task concurrent
  filesystem test (`SCHED-CONCURRENT-OK`).
- [x] A mount table in the VFS (longest-prefix resolution, per-mount
  read-only, `EBUSY`/`EXDEV` for mount-point operations) and a RAM
  filesystem as the userland root, with `/disk` for FAT12, a device layer on
  `/dev`, and a generated `/proc` (`USER-INITRAMFS-OK`).
- [x] An initramfs cpio archive loaded at boot, unpacked into the RAM
  filesystem, with `/init` running as PID 1 (`INITRAMFS-UNPACK-OK`,
  `INIT-READ-OK name=init`); media without an archive still boots FAT12 as
  the root.
- [x] A statically linked musl binary booted as `/init` in Linux CI
  (`musl_probe`, `MUSL-PROBE-OK`): Linux-shaped initial stack and auxiliary
  vector, Linux-shaped `set_thread_area`, and the `_llseek`/`readv`/`writev`/
  `set_tid_address` calls an outside libc expects. `python build.py probe`
  boots any externally built ELF as PID 1 and asserts its markers.
- [x] `python build.py shell` boots a static busybox (musl, i386) as PID 1
  and runs `ash` with `PATH`-resolved applets, `uname`, `ls`, `cat`, `sort`,
  `wc`, and `/proc` access, all through the Linux-compatible ABI
  (`SHELL CHECK PASSED: busybox ash ran as pid 1, 306 applets resolved
  through $PATH`). This required accepting `O_LARGEFILE`, clamping large
  read/write counts, returning the exact break from `brk`, demand-grown
  user stack, 64-entry `mmap` table, FAT12 exec bits, cross-mount symlink
  expansion, and an in-kernel exec image cache to make QEMU/TCG performance
  usable. The interactive shell also turns job control on: the descriptor
  table reaches fd 31 (ash dups the terminal to fd 10) and a `/dev/tty`
  opened by path answers `TIOCGPGRP`/`TIOCSPGRP`. Process groups are real —
  a task records its group and session, `fork` inherits them, and `setpgid`
  plus the terminal's foreground group let ash run a foreground pipeline
  (`ls / | wc -l`) instead of failing with "can't set tty process group". The
  console owns the screen once userland starts: every kernel log line
  (heartbeat, exec notes, ring-3 fault reports) is written to COM1 only, and
  the 1 Hz heartbeat stops updating the corner counter, so nothing scrolls
  the prompt away; the serial log keeps the headless `T+1s`/`T+2s`/`T+3s`
  markers. The console also answers busybox's `ESC[6n` cursor-position query
  (the prompt is `$ `, not `$ [6n`) and implements `\b`/`\r` for its line
  editor.
- [x] An x86-64 (long mode) target built from the same tree — `python
  build.py x64` produces `haku64.img` — with a long-mode entry, four-level
  paging and a 2 MiB-page physmap of all RAM (so no 1020 MiB ceiling), an
  E820 frame allocator, a kernel heap, a GDT/TSS with IST stacks, a 64-bit
  IDT with ISR stubs, the 8259 remapped to `0x20`/`0x28`, and a 100 Hz PIT
  (`HAKU64-TIMER-OK`). It has no scheduler, syscalls, or userland yet;
  `haku.img` is still the default image and the only one that runs the
  system.

## Proposed Milestones

### 1. Storage Reliability — Immediate Priority

- [x] Return distinct, correct errors for FAT12/VFS I/O failures, missing
  files, full disks, and invalid input (`FS-ERRORS-OK`/`FS-ENOSPC-OK` in
  headless: ENOENT/EINVAL/EEXIST/EBADF/ENOSPC/EIO mapping plus a
  root-full probe; syscall checks cover bad flags, bad fd, bad whence).
- [x] Add FAT12 tests for corrupt or cyclic chains, full disks, empty files,
  sector boundaries, and ATA errors (`FS-FILE-EDGE-OK`, `FS-CORRUPT-OK`,
  `FS-ATA-OK`: empty files, exact-sector/span/partial I/O, injected
  cut/bad/reserved/cyclic chains with restore, crafted-full data area,
  ATA bounds and NULL-transfer rejection).
- [x] Clean up allocated clusters and metadata as far as possible after failed
  writes, and document that operations are not atomic (`FS-ROLLBACK-OK`:
  gap failures keep the old size, short writes keep their durable prefix,
  directory-persist failures roll the chain back, failed TRUNC zeroes the
  entry; [the syscall notes](docs/syscalls.md#user-mode-system-calls)
  document the non-atomic model and its limits).
- [x] Avoid overwriting user data images during every build; separate initial
  image creation from preservation of an existing data image (existing
  `build/haku_fs.img` is reused as-is; only missing images or `--fresh`
  recreate it; `user.asm`-newer staleness warns; `headless` always starts
  fresh for deterministic fault-injection state).
- [x] Provide a way to inspect filesystem images after QEMU shuts down
  (`tools/lsfat12.py` + `python build.py inspect`: root listing, chains,
  `--cat`/`--dump` extraction; `headless` runs `--check` on the post-run
  image and fails the gate on mirrors/chains/orphan damage).

**Acceptance criteria:** QEMU success and fault-injection tests demonstrate
that failed operations do not leave leaked descriptors, corrupt FAT chains, or
unexpectedly lost image data across restarts.

### 2. VFS and Filesystem Completion

- [x] Define a filesystem-independent VFS interface and route descriptor
  operations through it (`vfs_fs_ops` mount/open/close/read/write/seek;
  FAT12 lives behind `fat12_vfs.c`, VFS holds no FAT12 references; boot
  registers the backend and mounts it as root; self-test guards the
  registry and proves the root survives bad mounts).
- [x] Add `unlink`, `rename`, `stat`, and `fstat`, including defined behavior
  for open files that are removed or renamed (syscalls 10/38/106/108;
  unlink defers the cluster free to the last open descriptor, rename
  repoints live descriptors; rename-to-existing gives `EEXIST`, same-name
  is a no-op; proven end to end from ring 3 as `USER-FILE-OPS-OK`, and
  the post-run image check confirms zero orphaned clusters).
- [x] Add subdirectories and basic paths, including rules for `.` and `..`
  (syscalls `mkdir`/`rmdir` 39/40; `/`-separated 8.3 paths resolved with
  real `.`/`..` entries, parent climbing clamped at the root; open/stat/
  unlink/rename are path-aware; `stat` reports `S_IFDIR`; directories are
  single-cluster and do not grow yet; proven from ring 3 as `USER-DIR-OK`
  and the host-side inspector now recurses into subdirectories).
- [x] Implement and test relevant POSIX errors such as `EEXIST`, `ENOENT`,
  `ENOSPC`, `ENOTDIR`, `EISDIR`, and `EROFS` (all covered, plus
  `ENOTEMPTY`; root mounts can be read-only via `VFS_MOUNT_RDONLY`, which
  rejects create/write/truncate/unlink/rename/mkdir/rmdir with `EROFS`
  while reads keep working, tested as `FS-EROFS-OK`).
- [x] Introduce appropriate disk and file locks instead of disabling interrupts
  for the duration of all I/O; add a sector cache if needed (`mutex_t` in
  `kernel/kernel/sync.h` parks a waiter on a wait queue instead of spinning;
  the VFS and the ATA driver each own one, so a task queued behind a disk
  transfer is `TASK_BLOCKED` and costs no CPU, while interrupts stay enabled
  for the whole transfer. A 16-slot direct-mapped read cache sits in front of
  the drive and absorbs the repeated FAT and directory reads
  (`ATA-CACHE-OK`); writes remain write-through and refresh or invalidate the
  cached copy, and selecting another device flushes it. Two tasks using the
  filesystem at once is covered by `SCHED-CONCURRENT-OK`. Advisory record
  locks are still missing: `F_SETLK` is accepted and changes nothing).
- [x] Decide separately whether to support long filenames, partition tables,
  or FAT16; require interoperability tests before expanding scope (decision:
  none of the three for the data disk. [Storage and filesystems](docs/storage.md#why-fat12-with-83-names-and-not-something-bigger)
  records the reasoning: the userland root moves to an
  in-memory filesystem with long names and permission bits, which leaves
  FAT12 as an interchange format where 8.3 names suffice. Any future change to
  the on-disk format must first pass an `mtools`/`fsck.fat` round-trip in CI
  and agree with `tools/lsfat12.py`).

**Acceptance criteria:** Multiple processes can use independent files
concurrently; file and directory operations, EOF, seeking, and errors are
tested, and common FAT tools can still read the disk image.

### 3. Processes, Syscalls, and Program Execution

- [x] Replace the fixed test-program list with `execve` to load and replace
  the current process image from an ELF file on disk (`NEXT.ELF` headless
  test; PID and open descriptors survive replacement; argc/argv/envp are
  built and validated on the new user stack).
- [x] Add `waitpid`, a simpler ELF-based `spawn` process-creation model, and
  process exit status management (blocking parent wait, reaped child records,
  and encoded status validated in QEMU).
- [x] Define the initial user-stack convention for `argc`, `argv`, `envp`,
  and the terminating `AT_NULL` auxiliary-vector entry.
- [x] Add standard descriptors 0/1/2, descriptor inheritance or transfer, and
  automatic close-on-exit behavior (a registered console device backs fds
  0/1/2; `dup2` redirects and `close` revokes them; spawned children inherit
  the parent's descriptors, and exit releases them).
- [x] Add `dup`/`dup2` with a shared, refcounted open-file-description
  (`USER-DUP-OK`: shared offset, surviving duplicate, EBADF cases).
- [x] Add limited `brk`/`mmap` (per-process heap break plus an anonymous
  private `mmap` using the i386 old_mmap argument block; both map zero-filled
  user pages from reserved regions; `brk` also shrinks and releases pages, and
  `munmap` (91) trims, splits, and frees mappings so later `mmap` calls reuse
  the address space; `USER-MEM-OK`).
- [x] Add time syscalls such as `gettimeofday`/`nanosleep` (`gettimeofday`
  reads the PIT tick counter; `nanosleep` parks the task until its wake tick
  in the timer IRQ instead of busy-waiting; `USER-TIME-OK`).
- [x] Define a public user/kernel C ABI header for syscall numbers, errno
  values, open flags, and the `stat` structure.
- [x] Test invalid arguments, permissions, and device errors for every syscall
  (`USER-ERRNO-OK` drives each syscall family from ring 3 with a bad
  descriptor, an invalid user pointer, a missing path, and a bad argument, and
  asserts the documented errno — `EBADF`, `EFAULT`, `ENOENT`, `EEXIST`,
  `EISDIR`, `ENOTDIR`, `ENOTEMPTY`, `EINVAL`, `ERANGE`, `ENOTTY`, `EPIPE`,
  `ESRCH`, `ECHILD`, `ENOMEM`. Permission errors come from the read-only
  mount (`FS-EROFS-OK`), and device errors from the console ioctl cases plus
  the injected ATA failures (`FS-ATA-OK`)).

**Acceptance criteria:** A basic shell can launch programs from disk, collect
their output and exit status, and run multiple programs without unintended
address-space or descriptor sharing.

### 4. Concurrency and Memory Management

- [x] Add kernel synchronization primitives (spinlocks and IRQ-save locks) and
  define locking rules for the scheduler, allocators, VFS, and drivers
  (`kernel/kernel/sync.h`/`.c`: `irq_lock`/`irq_unlock`, task-context
  `spin_lock`/`spin_unlock`, and `spin_lock_irqsave`; VFS uses one
  task-context lock around descriptor/OFD changes and backend calls, the
  frame/heap allocators and console use IRQ-save locks, ATA and the keyboard
  ring buffer are serialized per the [locking rules](docs/kernel-internals.md#locking)).
- [x] Separate wait queues and sleep/wakeup from busy-waiting so blocking I/O
  sleeps the current process instead of spinning (`kernel/kernel/scheduler.h`
  wait queues use a task-slot bitmap and a distinct `TASK_BLOCKED` state;
  blocking console reads park until IRQ1 wakes them, proven by
  `SCHED-WAITQ-OK`, while `nanosleep`/`waitpid` keep their existing states).
- [x] Extend allocation beyond the current identity-mapped range and add
  user virtual-memory allocation and release (the frame allocator uses every
  tracked E820 frame through a supervisor direct map at `0xC0400000`, sized
  from the memory map; its self-test allocates and touches a frame above
  8 MiB and probes the last usable frame. `brk` now shrinks and unmaps
  released heap pages, and `munmap` (91) trims, splits, and frees anonymous
  mappings so later `mmap` calls reuse the address space; `USER-MEM-OK`
  covers shrink, release, and reuse).
- [x] Improve page protections and test read-only, unmapped, and
  use-after-free access (the kernel sets `CR0.WP`, maps `mmap` pages from
  `PROT_WRITE` so `PROT_READ` is enforced read-only, and only tears down user
  PDEs/PTEs; `USER-PROT-OK` spawns a
  child per case: store into an `RX` segment, store into a `PROT_READ`
  mapping, read from unmapped memory, and store through a `munmap`ed page,
  checked against `#PF` error codes 7, 7, 4, and 6). A `PROT_NONE`
  reservation was rejected at that point; the memory-syscall item below
  accepts it and makes the range fault on access.
- [x] Track memory and resource usage per process (paging maintains
  mapped-page, page-table, and peak counters per address space; the scheduler
  attributes timer ticks by CPL and counts syscalls plus voluntary and
  involuntary switches; `getusage` (0x1001) returns a 64-byte snapshot;
  `USER-USAGE-OK` covers growth, release, tick, syscall, and peak deltas).

**Acceptance criteria:** A process blocked on I/O does not prevent other
processes from making progress; allocator stress tests complete without frame
leaks or page-table corruption.

### 5. Hardware and User Experience

- [x] Define a terminal and text-I/O abstraction, separating keyboard input
  from syscalls and display backends (`kernel/drivers/terminal.c/.h` owns the
  output path and the input ring/wait queue; `kbd.c` only translates scancodes;
  the VFS console device forwards to `terminal_read`/`terminal_write_bytes`).
- [x] Add framebuffer support only with a boot protocol or safe discovery of
  the framebuffer mode and memory; retain VGA text as a fallback
  (`display.h` defines the shared `display_ops_t` API implemented by
  `vga_text.c` and `framebuffer.c`; the boot path selects the framebuffer
  backend after `framebuffer_init` succeeds, otherwise VGA text stays active).
- [x] Improve ATA timeout and recovery behavior and support more devices or
  controllers; consider IRQ/DMA instead of PIO polling if required
  (`kernel/drivers/ata.c` scans both legacy channels and all master/slave
  positions, rejects floating-bus and ATAPI devices, uses bounded polling
  with software-reset retry recovery, and flushes the write cache; FAT12
  mounts from any detected disk; IRQ/DMA completion is deliberately deferred
  because the FAT mount runs before the PIC/PIT exist, so the synchronous
  path stays deterministic).
- [x] Add leveled logging, a panic handler, and useful crash reports in QEMU
  (`klog` exposes DEBUG/INFO/WARN/ERROR/CRIT with an INFO threshold and a
  filtering self-test; `panic.c` halts on fatal faults and boot failures;
  `crash.c` prints CR0/CR2/CR3, the exception frame, registers, and the
  validated stack to the display and `build/serial.log`; ring-3 faults reuse
  the report without halting the kernel).

**Acceptance criteria:** The system remains diagnosable without graphics;
display backends use a shared API, and missing framebuffer support does not
prevent boot.

### 6. Developer Tooling and Compatibility

- [x] Provide a small user-space library and headers for syscalls, strings,
  memory, and file I/O (`include/haku/` defines the C API; `user/lib/` adds
  raw `int 0x80` stubs, errno-translating POSIX wrappers, `mem*`/`str*`, a
  first-fit `malloc` over `brk`, and a small `printf`).
- [x] Add a build target for user C programs and document a stable toolchain
  and ABI (`build.py` compiles `user/lib/` and `user/tests/libtest.c` with the
  kernel `cl.exe`/`clang-cl`, links a fixed-base PE32 without a CRT, and
  `tools/pe2elf.py` emits the ELF32 `PT_LOAD` image installed as `LIB.ELF`;
  `NEXT.ELF` spawns it in the headless run and checks `USER-LIB-OK`).
- [x] Add automated tests for malformed ELF files, syscalls, VFS, FAT, and the
  scheduler (`ELF-REJECT-OK` breaks one header or program-header field at a
  time — class, byte order, machine, entry, table geometry, `p_offset`,
  `p_vaddr`, `p_filesz` against `p_memsz`, `PT_INTERP`, writable+executable —
  and requires every mutation plus a truncated image to be rejected;
  `USER-ERRNO-OK` is the syscall and VFS error matrix;
  `SCHED-CONCURRENT-OK` runs two tasks against the filesystem at once; the
  FAT suite stays `FS-ERRORS-OK`/`FS-EROFS-OK`/`FS-FILE-EDGE-OK`/
  `FS-CORRUPT-OK`/`FS-ATA-OK`/`FS-STAT-OK`/`FS-ROLLBACK-OK`/`FS-ENOSPC-OK`).
- [x] Define the limited POSIX subset actually supported; add conformance
  tests and document limitations for each capability
  ([the POSIX subset notes](docs/syscalls.md#supported-posix-subset) list
  the implemented calls by area, what is deliberately
  absent, and which calls are accepted without changing state; the ring-3
  self-tests `USER-LIB-OK` and `USER-FS-ABI-OK` exercise the surface, though
  there is no formal conformance suite).
- [x] Document manual testing and disk-image recovery or replacement
  ([manual testing](docs/building.md#manual-testing-and-image-recovery): a
  windowed run for hands-on testing, `--fresh` to rebuild a discarded data
  image, and
  `inspect --check` to prove an image is damaged before throwing it away).

**Acceptance criteria:** An independent C program can be built and run using
the documented toolchain, and `python build.py headless` runs all regression
tests repeatably.

### 7. Linux-Compatible ABI and a GNU Userland

Goal: run a real GNU userland (statically linked musl, e.g. busybox) on top
of HakuKernel. glibc is out of scope: it needs the 64-bit `*64` syscall
variants, `rseq`/`membarrier`, and a much wider syscall surface. The
userland root becomes a Unix-like RAM filesystem, not FAT12.

- [x] Publish a Linux-compatible syscall ABI for i386 (keep the numbers that
  already match) and a real `struct stat` layout, so unmodified
  Linux-targeted binaries can call the kernel
  (`include/haku/abi.h` lists the i386 Linux numbers, the implemented ones
  plus the ones the later milestones add; `haku_stat_t` is the 64-byte i386
  `struct stat`, `haku_stat64_t` the 96-byte `struct stat64` that glibc and
  musl use on i386, and `haku_statx_t` the 256-byte `struct statx`; the
  kernel implements `stat`/`lstat`/`fstat`, `stat64`/`lstat64`/`fstat64`,
  `fstatat64`, and `statx`, and the user library reads the same layout as
  musl; the Haku-specific `spawn`/`getusage` pair stays outside the Linux
  range).
- [x] Add process creation and reaping: `fork` (a private copy of the address
  space, the caller's saved register frame with `eax` zeroed, and the
  descriptor table), the `SIGCHLD` form of `clone`, `wait4` (`WNOHANG` and
  any-child forms; `rusage` is accepted but zeroed), and `exit_group`. The
  ELF-only `spawn` model is no longer the only way to start a process; it is
  covered by `USER-FORK-OK` in the ring-3 self-test and by the C library
  (whose `waitpid` is now a `wait4` wrapper).
- [x] Add `vfork` (share the address space until `execve`/`exit`) and
  copy-on-write for `fork` instead of the eager copy, so large processes do
  not pay for a full duplicate. Pages are shared with per-frame reference
  counts and broken on the write-protection fault, for kernel-side copies
  too; a `vfork` parent stays suspended until the child execs or exits and
  then resumes with the child PID. The library's `vfork` keeps its return
  address in `edx` across the syscall because the child runs on the caller's
  stack. Covered by `USER-COW-OK`, `USER-VFORK-OK`, `VFORK-EXEC-RAN`, and a
  `vfork` case in the C library self-test. `clone` still rejects the
  thread-creating flags, stack pointers, and parent-tid arguments with
  `EINVAL`.
- [x] Add signals: `kill`/`tkill`/`tgkill`, `rt_sigaction`,
  `rt_sigprocmask`, `rt_sigreturn`, pending-signal delivery, and default
  dispositions. Each task keeps a pending set, a blocked set, and one
  disposition per signal (a single word each, so signals 1..31 exist and
  the real-time range is rejected with `EINVAL`). Delivery happens on the
  way back to user mode: the kernel builds a frame on the user stack
  (restorer, handler arguments, saved registers, `siginfo`, `ucontext`),
  enters the handler, and `rt_sigreturn` restores the interrupted context.
  `SA_SIGINFO` and `SA_RESTORER` are honored (without a program restorer
  the kernel installs a stub that calls `rt_sigreturn`); `SIG_IGN`
  discards, `SIG_DFL` ends the process for every signal except `SIGCHLD`,
  `SIGURG`, `SIGWINCH`, and `SIGCONT`, and a parent sees the signal number
  in the wait status. A signal queued for a task blocked in a syscall
  wakes it with `EINTR`. `fork`/`vfork` reset the dispositions and keep the
  blocked set; `execve` resets the dispositions and keeps the blocked set
  and the pending signals. Covered by `USER-SIGNAL-OK` in the C library
  self-test. Process groups and sessions are recorded per task, inherited
  across `fork`, moved by `setpgid`, and addressed by a negative `kill` pid,
  which is what a job-control shell needs. Still missing: stop/continue job
  states, `sigqueue`, and real-time signals.
- [x] Add the memory syscalls a libc needs: `mmap2` with
  `MAP_FIXED`/`MAP_ANONYMOUS`/`MAP_SHARED`, `mprotect`, `mremap`,
  `madvise`, and thread-local storage through `set_thread_area` (today
  `mmap` only accepts `MAP_PRIVATE|MAP_ANONYMOUS` and rejects fixed
  addresses).
  `mmap` (the old argument block) and `mmap2` (the register form) now
  share one path: anonymous pages with the full protection set, a
  `MAP_FIXED` placement inside the mmap window, and the shared/private
  flag. `mprotect` changes the `R/W` bit of a range or clears the user
  bit for `PROT_NONE`; a copy-on-write page keeps its shared frame
  protected and remembers that its mapping is writable, so the first
  write still copies. `mremap` grows, shrinks, or moves a mapping
  (`MREMAP_MAYMOVE` copies the contents through the direct map),
  `MADV_DONTNEED` releases the pages so they come back zeroed, and
  `set_thread_area` points the user `FS` segment at the thread block,
  which is the thread pointer a libc reads through `%fs:0`. The library
  gains `include/haku/mman.h` and `user/lib/mman.c`; the ring-3
  self-test covers the fixed mapping, the protection changes, the fault
  on a read-only page, the move, the advice call, and the thread pointer
  (`USER-MMAP-OK`). Still missing: file-backed mappings (there is no
  page cache yet), `mremap` with `MREMAP_DONTUNMAP`, and shared
  mappings that a second task can observe.
- [x] Add `futex` so libc locks, and later threads, behave correctly
  (`FUTEX_WAIT`/`FUTEX_WAKE` plus the `_PRIVATE` and `_BITSET` forms, with a
  relative timeout that reports `ETIMEDOUT`; waiters park on the scheduler's
  address queue instead of spinning, and the C library covers the value
  check, the wake, and the timeout as `USER-FUTEX-OK`). Thread creation
  itself stays out of scope: `clone` still rejects the thread flags.
- [x] Extend the filesystem ABI for libc and shell tooling: the `*at` family
  (`openat`, `unlinkat`, `renameat`, `mkdirat`, `faccessat`, and
  `fstatat` through the existing `fstatat64`/`statx`), `fchmod`/`fchown`
  and their `*at` forms, `truncate`/`ftruncate`, `access`, `getdents64`,
  `chdir`/`getcwd`, `dup3`, `pipe`/`pipe2`, `fcntl`, `ioctl` (termios and
  `TIOCGWINSZ` for an interactive shell), `poll`, `getrlimit`/`setrlimit`/
  `prlimit64`, `getrandom`, `sched_yield`, `prctl`, and `personality`.
  Every path syscall resolves a relative name against the task's working
  directory, which `chdir`/`getcwd` keep as a canonical path, and a `dirfd`
  argument resolves against the directory descriptor the caller opened.
  `getdents64` packs Linux `linux_dirent64` records from a new backend
  `opendir`/`readdir` pair, `pipe` uses two descriptions with separate
  reader and writer counts (end of file once every write end closes,
  `EPIPE` plus `SIGPIPE` once every read end does), `fcntl` reports and sets
  `O_APPEND`/`O_NONBLOCK` and duplicates onto a requested descriptor, and
  the console answers `TCGETS`/`TCSETS*`, `TIOCGWINSZ`, `FIONREAD`,
  `TIOCGPGRP`/`TIOCSPGRP` with the caller's pid as the foreground group, and
  `TIOCNOTTY`, while a `/dev/tty` opened by path answers the same requests.
  Permission and ownership calls succeed without changing anything because
  FAT12 has no mode or owner bits, and `getrlimit` reports the fixed table
  sizes (32 descriptors, 4 tasks). `poll` blocks (zero scans once, negative
  waits forever, positive is a millisecond deadline) by parking on a
  scheduler activity queue the timer wakes every tick, so the wait and its
  deadline are re-scanned at 100 Hz without per-descriptor wakeups; `select`
  (142 `_newselect` and 82, the number musl uses) feeds the same scan, and
  the legacy `getdents` layout is produced by rewriting the 64-bit records in
  place. Proven from ring 3 as `USER-FS-ABI-OK`.
  Still missing: real record locks (`F_SETLK` is accepted as a no-op).
- [x] Add a Unix-like filesystem for the userland: an in-memory filesystem
  (initramfs) with long names, permission bits, symlinks, timestamps, and
  nested directories, plus a device layer (`/dev/null`, `/dev/zero`,
  `/dev/urandom`, `/dev/console`, `/dev/tty`) and a minimal `/proc`
  (FAT12 stays as the data-disk backend for the existing 8.3 tree).
  The VFS now holds a mount table instead of a single root backend, with
  longest-prefix resolution, per-mount read-only flags, `EBUSY` on mount
  points, and `EXDEV` for a rename across mounts. `ramfs` is the writable
  root: 48-byte name components, mode bits, PIT timestamps, nested
  directories, symlinks with a hop limit, unlink-while-open, and a fixed
  512-node table with heap-backed file data. `devfs` serves the six device
  nodes through the ordinary descriptor path (`ESPIPE` on seek, no unlink or
  rename), and `procfs` generates `version`/`uptime`/`meminfo` at open time
  and refuses writes with `EACCES`. `symlink`/`readlink` became real
  syscalls, and `lstat`/`AT_SYMLINK_NOFOLLOW` now describe the link instead
  of its target through a new optional backend operation. Proven from ring 3
  as `USER-INITRAMFS-OK` (root listing, a name past 8.3, a two-level
  directory, a followed and an unfollowed link, `/dev/null`, `/dev/zero`,
  `/proc/version`, `/proc/meminfo`, and `/disk/HAKU.ELF`).
- [x] Load a userland image at boot: unpack an initramfs/cpio archive into
  the RAM filesystem and `execve` `/init` as PID 1 (the boot path currently
  loads a single ELF from the FAT12 disk).
  `tools/mkcpio.py` writes a `070701` archive and `kernel/fs/initramfs/cpio.c`
  unpacks it through the public VFS (`mkdir -p` for parents, symlinks, and
  regular files, with the header-plus-name padding rule). The kernel mounts
  `ramfs` on `/`, creates `/disk` as a real directory, mounts the FAT12 data
  disk there, reads `disk/INITRD`, unpacks it, mounts `/dev` and `/proc`, and
  runs `/init` as PID 1 (`INITRAMFS-UNPACK-OK`, `INITRAMFS-ROOT`,
  `INIT-READ-OK`). An image without an archive still boots with FAT12 as the
  root, so older media keeps working. The headless run asserts the whole
  path: every ring-3 marker now comes from a process that was started out of
  the RAM filesystem.
- [x] Build the userland in the Linux CI (static musl, e.g. busybox and/or
  GNU coreutils), ship it with the release artifacts, and extend the
  headless test to assert its behavior (for example `busybox sh -c "echo"`
  and a file round-trip through the RAM filesystem).
  `user/tests/musl_probe.c` is built with an `i486-linux-musl` cross
  toolchain in a dedicated `musl` CI job and booted as `/init` through the new
  `python build.py probe --init=<elf> --expect=<marker>` mode, which skips
  the keyboard injection and asserts the boot markers plus the requested
  one. Making an outside libc work required four ABI corrections: the
  initial process now starts on the same stack `execve` builds (previously
  PID 1 got a bare `esp` at the top of the region, which faulted), the
  auxiliary vector carries `AT_PHDR`/`AT_PHENT`/`AT_PHNUM` so musl can find
  its own `PT_TLS` segment (Haku's images do not map their header table, so
  the loader maps a copy on a page below the stack), `set_thread_area`
  follows the Linux contract (accepts `0xFFFFFFFF`, writes the chosen entry
  back, returns it) so musl's `%gs` setup succeeds, and the kernel gained
  `_llseek` (140), `readv`/`writev` (145/146), and `set_tid_address` (258) —
  musl's stdio writes through `writev`, so without it a static libc prints
  nothing. The probe asserts `MUSL-OK`, a file round trip, `chdir`/`getcwd`,
  and a `readdir` of `/` before printing `MUSL-PROBE-OK`.
  The filesystem ABI now covers the calls busybox leans on for an
  interactive shell: blocking `poll`, `select` through both i386 numbers (142
  and 82), and the legacy `getdents` layout alongside `getdents64`.
  Still missing for a full busybox userland: record locks, and enough of
  `/proc` for `ps`. The probe ships with the CI artifacts, not
  with the release image; the release still contains Haku's own userland.

**Acceptance criteria:** A statically linked musl binary runs as PID 1 from
an initramfs or FAT12 data disk, executes shell builtins and applets
(`python build.py shell` boots busybox `ash` and asserts `SHELL-OK`,
`APPS:<count>`, and `SHELL-DONE`), reads and writes files in the RAM
filesystem, and the headless test asserts its output; glibc, dynamic
linking, and shared libraries stay out of scope.

### 8. x86-64 (long mode)

The 32-bit kernel remains the default image; this milestone adds a 64-bit
boot path and ports the kernel onto it in stages. Every stage must leave
`python build.py headless` green, so the two targets stay independent until
the port is complete.

- [x] Boot into IA-32e long mode and run 64-bit C.
  `kernel/arch/x86_64/boot/entry64.asm` sets `CR4.PAE`, `EFER.LME`, builds a
  PML4/PDPT/PD identity map of the first 1 GiB with 2 MiB pages, and
  far-jumps to a 64-bit code segment; `kernel/arch/x86_64/kernel/boot64.c` is
  the first C code the 64-bit path runs, which also proves the Microsoft x64
  calling convention across the asm/C boundary. `python build.py x64` reuses
  the existing stage1/stage2 floppy, and `tools/pe2bin.py` now converts PE32+
  images as well as PE32.
- [x] Memory: four-level paging helpers, a supervisor physmap of all RAM at
  `0xFFFF800000000000` built from 2 MiB pages (one page directory per GiB,
  so no page tables and no 1020 MiB ceiling — a 4 GiB guest reports a
  5120 MiB frame limit), an E820 frame allocator whose bitmaps are sized
  from the detected limit, a bump allocator for the structures that precede
  them (`bootmem64.c`), and an 8 MiB free-list kernel heap. Self-tests cover
  the physmap alias, allocate/free, a 4 KiB mapping through the new
  `paging64_map_page`, and heap coalescing (`HAKU64-SELFTEST-OK`).
- [x] Interrupts: 64-bit IDT and TSS with an IST stack, rewritten ISR stubs,
  and the PIC/PIT path. `gdt64.c` builds a six-entry GDT (kernel and ring-3
  segments plus a 64-bit TSS) and wires IST1 to `#DF` and IST2 to NMI, so a
  double fault — normally the *result* of a bad kernel stack — can still be
  reported. `isr64.asm` has one stub per vector, zero-fills the error-code
  slot where the CPU does not supply one, and hands a `regs64_t` to
  `isr64_dispatch`; `pic64.c` remaps the 8259 to `0x20`/`0x28` and `timer64.c`
  drives the PIT at 100 Hz. MSVC x64 has no inline assembly, so `CLI`/`STI`/
  `LIDT`/`LGDT`/`LTR`, port I/O, `CR2` and `PAUSE` are NASM routines, and the
  `HAKU64-EXC-OK` self-test provokes `INT3`, `#PF` and `#DE` and resumes each
  one by setting `RIP` to a label the assembler exported for the instruction
  after the faulting one — no instruction decoder and no hardcoded lengths.
  The IDT layout is asserted separately (`IST` indices, gate type, distinct
  stubs per vector, one shared catch-all).
- [ ] Processes: 64-bit context switch, per-process page tables, ring-3
  entry, and the syscall gate (`syscall`/`sysret`, or `int 0x80` with a
  64-bit frame).
- [ ] Syscall ABI: x86-64 Linux syscall numbers and the
  `rdi/rsi/rdx/r10/r8/r9` convention, replacing the i386 table in
  `usermode.c` (97 cases).
- [ ] Userland: ELF64 loader, a 64-bit build of `user/lib`, and `musl-gcc`
  binaries in the probe.

**Acceptance criteria:** `python build.py x64` boots to a 64-bit user process
running the same test suite as the 32-bit image, with its own headless marker
set; the 32-bit image keeps working from the same tree.

## Out of Scope for This Roadmap

These features require separate milestones and design decisions:

- SMP and architectures other than x86 (x86-64 itself is Milestone 8).
- Networking, TCP/IP, USB, audio, and a complete graphical interface.
- Full POSIX and glibc compatibility, a dynamic linker, and shared
  libraries; the userland target is a statically linked musl userland
  (see Milestone 7).
- Journaling or transactional guarantees and power-loss recovery.

## Delivery Process

Keep each milestone small and reversible, update relevant documentation, and
validate changes with the build and headless tests. Move an item to **Current
Status** only after it has a repeatable test and its limitations are documented.
