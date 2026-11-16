# Userland

[Back to documentation index](README.md)

The freestanding user C library and the busybox/musl userland that runs as
PID 1.

Two libraries exist, one per ABI: `user/lib` for the 32-bit kernel described
below, and `user/lib64` for the x86-64 kernel, which is documented separately
in [the x86-64 C library](libc64.md). They share conventions — `-errno`
translated to `-1` plus `errno`, a fixed-base PE repacked as an ELF by a
`tools/pe2*.py` script — but no source file, because the structures and the
syscall entry differ at every level.

## User C library

`include/haku/` doubles as a freestanding C SDK: `<haku/haku.h>` includes the
syscall ABI, `types.h`, `errno.h`, `string.h`, `stdlib.h`, `unistd.h`,
`fcntl.h`, `stat.h`, `stdio.h`, `signal.h`, `mman.h`, `futex.h`, `dirent.h`,
`poll.h`, `termios.h`, and `resource.h`.

- Raw syscalls: `haku_syscall0`..`haku_syscall4` in `user/lib/syscall.asm`
  (i386 `int 0x80`, arguments in `ebx`/`ecx`/`edx`/`esi`, result in `eax`).
  They return the kernel's `-errno` unchanged.
- POSIX-style wrappers return `-1` and set `errno`: `read`, `write`, `close`,
  `lseek`, `dup`, `dup2`, `open`, `stat`, `lstat`, `fstat`, `unlink`,
  `rename`, `mkdir`, `rmdir`, `getpid`, `fork`, `vfork`, `waitpid`, `execve`,
  `spawn`, `getusage`, `gettimeofday`, `nanosleep`, `sleep`, `brk`, and
  `sbrk`. `vfork` is written in `syscall.asm` rather than C: the child runs
  on the caller's stack, so the pending return address is parked in `edx`
  across the syscall instead of being left where the child would overwrite
  it.
  `stat`/`lstat` use the Linux `fstatat64` (300) call with `AT_FDCWD` and
  `fstat` uses `fstat64` (197), so the library and a musl program read the
  same 96-byte `struct stat64` layout.
- Signals: `kill`, `tkill`, `tgkill`, `raise`, `signal`, `sigaction`,
  `sigprocmask`, and the `sigset` helpers (`sigemptyset`, `sigfillset`,
  `sigaddset`, `sigdelset`, `sigismember`) in `user/lib/signal.c`. The mask
  is a single word, so only signals 1..31 exist.
- Memory mappings: `mmap`, `mmap2`, `mprotect`, `mremap`, `madvise`,
  `munmap`, `set_thread_area`, and `get_thread_area` in `user/lib/mman.c`,
  with the `PROT_*`, `MAP_*`, `MREMAP_*`, and `MADV_*` constants in
  `include/haku/mman.h`.
- Strings and memory: `mem*`, `str*`, `atoi`, `malloc`/`calloc`/`realloc`/
  `free` (a first-fit free list over `brk`), and `printf`/`puts`/`putchar`
  with a `%c/%s/%d/%u/%x/%X/%p` subset (no floats; the formatter builds the
  line in a per-call buffer on the stack, which is what makes concurrent
  calls safe).
- `user/lib/crt0.asm` supplies `_start`: it calls `main(void)` and exits with
  the returned status.

`build.py` compiles the library and `user/tests/libtest.c` with the same
`clang-cl` used for the kernel (the `--target=i686-pc-windows-msvc` triple),
links a fixed-base PE32 at `0x40000000` with no CRT, and `tools/pe2elf.py`
repacks its RX/R/RW sections into ELF32 `PT_LOAD` segments. The image is
installed as `LIB.ELF`; `NEXT.ELF` spawns it, requires a clean exit status,
and the run prints `USER-LIB-OK`. There is no C runtime, dynamic linker,
standard header set, floating-point formatting, or thread-safe `malloc`.
`printf` and `errno` *are* thread-safe: the formatter works in a per-call
stack buffer, and `errno` lives in each thread's control block. data-image
filenames remain 8.3. The user ELFs are regenerated on every build, but an
existing `build/haku_fs.img` only receives `LIB.ELF` after `--fresh` or
`headless`.

### Threads: the pthread subset

`include/haku/pthread.h` is one contract for both ports; `user/lib/pthread.c`
and `user/lib64/pthread64.c` implement it on top of the same kernel calls.
What exists is deliberately small:

| Group | Calls |
|---|---|
| thread | `pthread_create`, `pthread_join`, `pthread_detach`, `pthread_self`, `pthread_equal`, `pthread_exit` |
| mutex | `pthread_mutex_init`/`_destroy`/`_lock`/`_trylock`/`_unlock`/`_consistent`, `PTHREAD_MUTEX_INITIALIZER` |
| cond | `pthread_cond_init`/`_destroy`/`_wait`/`_signal`/`_broadcast`, `PTHREAD_COND_INITIALIZER` |

Attributes, recursive and error-checking mutexes, read/write locks,
thread-specific data, cancellation, timed waits, and `pthread_atfork` are
**absent** and the headers do not declare them.

A thread is a kernel task created with
`clone(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS|CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|CLONE_DETACH)`.
`CLONE_DETACH` tells the kernel the task has no joiner, so its slot goes back
on the timer tick instead of staying a waitable zombie; join and detach are
pure library state. Each thread owns a control-block page (which also holds
its `haku_tcb_t` on i386 and is the TEB page on x86-64) plus a 128 KiB stack
mapping, and `munmap`s both when it is done.

Two details make the hand-off safe:

- A joiner first claims the thread with a `FINISHED → REAPED` compare-and-swap,
  then *waits for `tid` to reach zero*. The kernel zeroes that word inside the
  exit syscall (`CLONE_CHILD_CLEARTID`), and only then is the exiting thread
  off its user stack, so the joiner can never pull the pages out from under
  the syscall stub. A detached thread frees itself on the same basis.
- `errno` is per thread: i386 keeps it in `haku_tcb_t.error` at `%fs:4`
  (located through the block's self pointer), x86-64 keeps it in the TEB at
  GS:32. `haku_errno_slot()` is two instructions, so every failure path can
  use it.

Mutexes are robust. The futex word holds the owner's task id plus the
kernel's `OWNER_DIED`/`WAITERS` bits, and the robust-list node embedded in
every mutex sits at `HAKU_PTHREAD_NODE_OFFSET` (16) so the one offset the
kernel reads applies to all of them. When a thread exits while holding a
mutex the kernel's exit walk marks the word owner-died and wakes a waiter;
the next `pthread_mutex_lock` returns `EOWNERDEAD`, `pthread_mutex_consistent`
declares the protected state repaired, and an unlock *without* that call
poisons the mutex so every later lock returns `ENOTRECOVERABLE`. Each thread
keeps its list head in a static table indexed by task id rather than in its
control block, because a thread frees the block before the exit syscall that
walks the list.

A condition variable is a counter plus a futex wake. A waiter reads the
counter, releases the mutex, and parks on the value it read; a signal bumps
the counter *after* the waiter's predicate was changed under the mutex, so a
signal that arrives in the window turns the park into `EAGAIN` instead of
getting lost.

`fork()` keeps only the calling thread: the child refreshes its handle's task
id, marks it as this process's main thread, and forgets every inherited
robust-list head.

## Running a real userland: busybox and musl

`python build.py shell` is the first time an unmodified third-party program
owns the machine. It also turned out to be the most demanding test the ABI
has had, because a shell does things a hand-written self-test never does:
`ls` a directory with 306 entries, `cat` a file into a 64 KiB buffer, `sort`
a few thousand lines (musl falls back to `mmap` for that), `fork`/`execve`
per command, and `--install -s /bin` to create 306 symlinks.

The boot path is the existing one. The shell image is a FAT12 volume with
`BUSYBOX` on it, the kernel mounts ramfs on `/`, mounts the FAT disk at
`/disk`, sees `disk/BUSYBOX`, and runs it with `argv[0] = "sh"` and
`argv[1..]` from `INIT.CMD` (`SHELL-BOOT`). busybox dispatches applets by the
basename of `argv[0]`, so the kernel must pass a name that is also a real
file: it uses the lowercase `disk/busybox` (FAT12 upper-cases 8.3 names
itself). The Alpine build has no standalone shell, so `sh` finds `ls` and
`cat` only through `$PATH` — the scripted run creates the links first.

What had to change in the kernel to get from "musl prints a line" to "a
usable shell":

- **`O_LARGEFILE` is accepted and ignored.** A 32-bit libc sets `0x8000` on
  every `open`, and `opendir` adds `O_DIRECTORY|O_CLOEXEC`; rejecting the
  flag made `ls /` fail with `EINVAL`.
- **Large `read`/`write` counts are clamped, not rejected.** busybox `cat`
  asks for 64 KiB at once; a short read is the correct answer.
- **`brk` returns exactly the break that was asked for.** musl treats the
  page-rounded value as a refusal and falls back to `mmap` for every
  allocation, which then hits the next limit.
- **The user stack grows on demand.** Two pages were enough for the
  self-test and not for `sort`; a fault below the initial stack now maps one
  more page, up to 1 MiB (`USER_STACK_GROWTH`).
- **A process can own more than 8 `mmap` regions.** `ls` on a large
  directory and `sort` on a few thousand lines both ran out of them; the
  per-process table is 64 entries now.
- **FAT12 files report mode `0755`.** A shell will not `exec` a `$PATH` file
  without an execute bit, and FAT has no permission bits to report.
- **Symlinks are followed across mount points.** `--install -s /bin` makes
  `/bin/ls -> /disk/busybox`, and an absolute target crosses from ramfs into
  the FAT12 mount. `vfs_resolve_links` expands up to 8 hops, strips the
  leading slash from an absolute target (VFS paths are canonical), and is
  applied at the top of `vfs_open`, `vfs_stat`, and `vfs_opendir` so every
  backend sees a path it can resolve itself. `execve` follows the link too,
  so all 306 applet names share one loaded image.
- **A 1 MiB exec is cached.** QEMU here is TCG-only, so one 1 MiB PIO load
  costs about 14 s; the first `execve` of a path reads it and keeps the
  image, and every later exec of the same file (or of another symlink to it)
  reuses it, which is the difference between "unusable" and "instant". The
  cache is keyed by path and size, and is also seeded with the boot image so
  PID 1 never pays for it. `ata_read_one` additionally moved to `rep insw`.
- **Job control is terminal-only.** `ash` enables it only after it dups the
  controlling terminal above fd 9, reads the foreground group, and writes it
  back. The descriptor table reaches fd 31, a `/dev/tty` opened by path
  answers the termios and `TIOCGPGRP`/`TIOCSPGRP` requests, and the
  process-group calls are real: a pipeline's children are moved into their
  own group and made the terminal's foreground group, then the shell takes
  its group back when the job finishes. The shell starts without
  `can't access tty; job control turned off`, and a pipeline such as
  `ls / | wc -l` or `echo hi | cat` forks, runs, and returns to the prompt
  (the scripted check asserts both). Stop signals still end a process.
- **The prompt is clean and the screen belongs to the shell.** busybox's line
  editor probes the terminal with `ESC[6n` before every prompt; the console
  answers with a cursor-position report instead of drawing the sequence, and
  kernel log lines stop mirroring to the display the moment userland starts.
  Typing, backspace, and command output therefore stay on a screen that no
  `T+<n>s` line or ring-3 fault report scrolls away (those still reach
  `build/serial.log`).

Interactive use is the same image without `INIT.CMD`: `python build.py shell`
drops into `ash` on the serial console. `ls` (sorted columns, 300+ entries),
`cat`, `head`, `grep`, `seq`, `sort`, `wc`, `uname`, `echo`, `mkdir`, `ln`,
`rm`, `cp`, `mv`, and `--install` all behave, `cat /proc/meminfo` and
`/proc/uptime` return live values, and `seq 1 20000 | wc -l` prints 20000. It
is a shell on top of the kernel's own VFS, not a Linux emulation layer, so
the tools that need a fuller `/proc` are shallow: `ps` prints only its header
(there is no `/proc/<pid>`), `df`/`mount` look for `/proc/mounts` and report
it missing, and `free` prints zeros because there is no `sysinfo` syscall.
There is also no `/etc`, no line discipline beyond what the console
provides, and `--install -s /bin` only fills the FAT12 image, which is
regenerated on every build.
