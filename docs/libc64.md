# The x86-64 user C library

[Back to documentation index](README.md)

`user/lib64/` is the C library an x86-64 ring-3 program links against. It
covers the subset of the POSIX surface that the kernel actually implements and
nothing else: no dynamic linking, no locale, no floating point, and threads
only as a thin layer over `clone`.

The first program built with it is `user/tests/init64.c`. The kernel loads it as
`/INIT.ELF` from the FAT12 volume and starts it at ring 3, so the boot test
proves the whole path — disk, ELF64 loader, user stack image, syscalls, library
— with a program whose source is ordinary C.

## Why a second tree instead of `user/lib`

`user/lib` is the 32-bit library and its headers describe 32-bit layouts:
`include/haku/abi.h` has a 96-byte `struct stat64`, 32-bit `size_t`, and the
i386 syscall numbers, and `user/lib/syscall.asm` is `int 0x80` with arguments
in `ebx`/`ecx`/`edx`/`esi`.

Sharing one tree would mean one header with `#ifdef`s around every structure
and every number, and a syscall stub that switches instruction sets in the
middle of a function. The two ABIs have almost nothing in common at that layer,
so `user/lib64/` is a separate, deliberately small tree instead:
`include/haku/abi64.h` (C) and `include/haku/abi64.inc` (NASM) describe the
x86-64 ABI once and both architectures keep their own library. `build.py`
compiles the two independently, and neither build touches the other's files.

## What is in the tree

| File | Contents |
|---|---|
| `crt64.asm` | `_start`: reads the initial stack image, finds the auxiliary vector, calls `haku64_tls_init`, calls `main(argc, argv, envp)`, exits with its return value; also holds `haku64_restore_rt`, the signal restorer that `sigaction` installs. |
| `syscall64.asm` | `haku64_syscall0`..`haku64_syscall6`: the raw kernel entry, returning `-errno` unchanged. |
| `haku_stdarg.h` | A freestanding `<stdarg.h>`: MSVC's `__va_start` intrinsic, or the clang builtins. |
| `tls64.c` | `_tls_index` and `_tls_used`, the TLS template markers, `haku64_tls_init`: finds `PT_TLS` through `AT_PHDR`, maps the block and a TEB, and installs the user GS base; `haku64_tls_new`/`haku64_tls_free` do the same per thread. |
| `thread64.c`, `thread64.asm` | `haku64_thread_spawn`/`haku64_thread_join` and the fire-and-forget `haku64_thread_create`: a 64 KiB stack, a handle, a per-thread TEB and TLS block, and a `clone` with `CLONE_SETTLS` and the two child tid flags; the asm stub starts the child at `haku64_thread_entry`, which installs GS and runs the body. |
| `pthread64.c` | The x64 half of the shared pthread subset (`include/haku/pthread.h`): create/join/detach/exit, robust mutexes and condition variables. A pthread owns a TEB page plus a 128 KiB stack and is cloned with `CLONE_DETACH`. |
| `robust64.c` | The per-task-id robust-list heads (a static table of `HAKU_PTHREAD_MAX_THREADS` entries) and `haku64_robust_ensure`/`haku64_robust_forget`/`haku64_robust_forget_all`. |
| `errno64.c` | `errno`, `haku64_errno_slot` (reads GS:32) and `haku64_errno_check`, which turns a kernel `-errno` into `-1` plus `errno`. |
| `string64.c` | `mem*` and `str*`: `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`. |
| `stdio64.c` | `printf`, `snprintf`, `puts`, `putchar`; the formatter builds each line in a 1 KiB buffer on the caller's stack. |
| `memory64.c` | `malloc`, `calloc`, `realloc`, `free` (first fit over a `brk` heap with block splitting) and `atoi`. |
| `unistd64.c` | `read`, `write`, `open`, `close`, `lseek`, `dup`, `dup2`, `dup3`, `pipe`, `pipe2`, `poll`, `ioctl`, `unlink`, `rename`, `mkdir`, `rmdir`, `chdir`, `getcwd`, `access`, `getpid`, `gettid`, `getppid`, `arch_prctl`, `fork`, `waitpid`, `execve`, `exit`, `_exit`, `brk`, `sbrk`, `stat`, `lstat`, `fstat`, `nanosleep`, `sleep`, `clock_gettime`, `uname`. |
| `mman64.c` | `mmap`, `munmap`, `mprotect`. |
| `signal64.c` | `sigaction`, `sigprocmask`, `sigaltstack`, `signal`, `raise`; `sigaction` sets `SA_RESTORER` and points it at `haku64_restore_rt`. |

Everything returns `-1` and sets `errno` on failure, using the kernel's
`-errno` return value; the raw `haku64_syscall*` wrappers do not translate it.

## The Microsoft x64 ABI

The userland follows the Microsoft x64 calling convention, because the
kernel's build already uses LLVM's `clang-cl` with the
`x86_64-pc-windows-msvc` triple, which emits it:
integer arguments in `RCX`/`RDX`/`R8`/`R9`, floating point in `XMM0`-`XMM3`,
32 bytes of shadow space at the callee's stack top, and a 16-byte aligned `RSP`
at every `CALL`.

Only the two assembly files know about that. The C files never touch registers,
so the library is an ordinary freestanding C library; `crt64.asm` moves the
initial stack image into the argument registers and `syscall64.asm` maps the
Microsoft registers onto the kernel's `RCX`/`RDX`/`R10`/`R8`/`R9` argument
order before `syscall`.

A consequence worth writing down: a C function is allowed to spill its register
arguments into its shadow space, i.e. `[rsp+8]` and up. `_start` therefore reads
`argc`/`argv`/`envp` *before* the call and then moves `RSP` 128 bytes further
down, so the shadow space of `main` cannot land on top of the words the kernel
left at the stack top. Without that the program starts with a valid `argc` and
a corrupted `argv`.

## Startup state

The kernel leaves the Linux x86-64 stack image at `RSP`:

```
[rsp]      argc
[rsp+8]    argv[0] .. argv[argc-1], NULL
           envp[0] .. , NULL
           AT_PHDR,   mapped program header table (images that map it)
           AT_PHENT,  sizeof(Elf64_Phdr)
           AT_PHNUM,  program header count
           AT_PAGESZ, page size
           AT_BASE,   0 (no interpreter)
           AT_FLAGS,  0
           AT_ENTRY,  entry point
           AT_UID/AT_EUID/AT_GID/AT_EGID, 0
           AT_HWCAP,  0
           AT_SECURE, 0
           AT_RANDOM, 16 seed bytes above this block
           AT_EXECFN, argv[0]
           AT_NULL,   0
```

Both `execve` and the kernel's own `/INIT.ELF` launch use the same builder
(`sys64_build_init_stack`), so the two paths cannot drift. `argv[0]` is
whatever path started the program and doubles as `AT_EXECFN`; the kernel does
not synthesize an environment, so `envp` is whatever the caller passed (empty
for the kernel's launch). `AT_PHDR` is present because
`tools/pe2elf64.py` emits a read-only `PT_LOAD` covering the ELF header and
program header table at the image base; an image without that segment simply
gets no `AT_PHDR` entry rather than a pointer to unmapped memory.

## Thread-local storage

`user/lib64/tls64.c` makes a compiler-generated `__declspec(thread)` variable
work. MSVC's x86-64 code generator does not call a helper for a static
executable; it emits:

```
mov eax, [_tls_index]     ; module index, 0
mov rcx, gs:[0x58]        ; TEB->TlsSlots
mov rax, [rcx + rax*8]    ; this module's TLS block
mov ... , [rax + offset]  ; the variable
```

so three pieces have to exist:

- `_tls_index`, defined as 0. A static executable is one module, and 0 is the
  only index the loader hands out.
- A TEB at the user GS base with `TlsSlots` at `+0x58` pointing at an array
  whose slot 0 is the block. `+0x30` points back at the TEB because that is
  what makes it a TEB.
- A writable copy of the linker's TLS template: `PT_TLS` gives the address and
  size, the startup code copies the file bytes and zeroes the `memsz - filesz`
  tail.

The template itself is declared in C, not found by a linker script:
`_haku_tls_start` is placed in `.tls` and `_haku_tls_end` in `.tls$ZZZ`,
which sort around every `.tls$` variable the compiler emits, so
`[start, end)` covers them without naming them. `_tls_used` in `.rdata$T` is
the structure MSVC's linker recognizes as the image's TLS directory.
`tools/pe2elf64.py` reads that directory out of the PE and writes an
equivalent `PT_TLS`.

`crt64.asm` walks the auxiliary vector before `main()`, calls
`haku64_tls_init`, and exits with status 70 if the block or the TEB cannot be
mapped or `arch_prctl` fails, so a TLS access cannot fault later with no
explanation. `haku64_tls_init` is exported so a custom entry point can do the
same.

The kernel side is the base, not the contents: `arch_prctl(158)` with
`ARCH_SET_FS`/`ARCH_SET_GS` stores a user address, `ARCH_GET_FS`/`ARCH_GET_GS`
copies it back, and the task keeps both bases. The scheduler reloads
`IA32_FS_BASE` and `IA32_KERNEL_GS_BASE` on every switch; `fork` and `clone`
inherit the bases and `execve` clears them, so a forked child starts with its
own copy-on-write view of the same block and a new image starts with none.
The library installs the TEB as the GS base because that is the base MSVC's
thread-local access reads; FS is available for code that wants it.

## Threads

`haku64_thread_create(fn, arg)` starts a second task in the same process and
returns its tid. It is detached: there is no handle, and when `fn` returns the
thread frees its TLS block, unmaps its own 64 KiB stack and exits, so the
kernel reclaims the task slot at the next switch. The tid is only useful for
`kill()` and `/proc`-style inspection. `haku64_thread_spawn(fn, arg)` returns
a handle that can be joined, and `haku64_thread_join(thread)` waits for it.

Both build a TEB plus a fresh copy of the TLS template with
`haku64_tls_new`, and call `clone` with every flag that makes a full thread:

```
CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_SYSVSEM |
CLONE_THREAD | CLONE_SETTLS | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID
```

`haku64_thread_create` adds `CLONE_DETACH`, the kernel's "no joiner" flag: the
slot is released by the scheduler instead of staying a waitable zombie.

`CLONE_SETTLS` sets only the child's FS base, to the address of its thread
control block (the TEB page). The kernel leaves GS alone because the TEB is a
library concept; `thread64.asm` reloads the start record from a register the
child inherited, and `haku64_thread_entry` calls `arch_prctl(ARCH_SET_GS)`
before it runs `fn`. From then on every `__declspec(thread)` access in that
thread reaches its own block, and a write there is invisible to the parent.

The mapping is laid out from the top down: the handle, then the start record,
then the initial RSP. The 32 bytes between the record and the RSP are exactly
the shadow space the entry call needs, and nothing above the initial RSP is
touched by the child's stack, so the handle stays readable for the whole
lifetime of the thread. Nothing is allocated on the heap and the parent never
frees anything the child might still be reading.

`CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID` are what make a join possible: the
kernel writes the child's tid into the handle's `done` word before the child
can run, and zeroes it — waking one futex waiter — when the child ends.
`haku64_thread_join` therefore sleeps on `futex(WAIT)` until the word clears,
reaps the tid with `waitpid`, frees the TEB with `haku64_tls_free`, and
unmaps the stack. Its return value is the thread's exit status. The handle
lives inside that stack mapping, so it is dangling after a successful join
and must not be used again.

A thread that calls `execve` replaces the whole process: the kernel ends the
rest of the thread group, moves the address space to the caller, resets the
thread pointer and the tid word, and enters the new image on its own stack
with a cleared register set. That is a Linux-compatible exec, so the new
program never sees the old threads.

`_exit` ends only the calling thread, while `exit`/`exit_group` take the
whole group down.

## pthreads and robust mutexes

`pthread64.c` implements the API declared in `include/haku/pthread.h` — the
same header the i386 library uses — on top of those kernel calls. A pthread
is cloned with `CLONE_DETACH`, so the kernel reclaims its slot instead of
leaving a waitable zombie; join and detach are library state only. Its TEB
page also holds the `haku_pthread` handle at `HAKU64_TEB_HANDLE_OFFSET`
(0x200) and the TEB records that address at `+0x50`
(`HAKU64_TEB_THREAD_OFFSET`), so `pthread_self()` is a GS load plus an add.
The stack is a separate 128 KiB mapping; join or the thread's own exit
releases the handle, TEB and TLS block with `haku64_tls_free` and the stack
with one `munmap`.

Three ordering rules are easy to get wrong and are worth naming:

- A thread publishes `FINISHED` *before* it issues its exit syscall, so a
  fast joiner cannot free on that transition alone: it must also wait for the
  kernel to zero `tid` (`CLONE_CHILD_CLEARTID`), and only then is the exiting
  thread off its user stack.
- `pthread_join` and `pthread_detach` both claim a finished thread with a
  `FINISHED → REAPED` compare-and-swap, so exactly one of them frees the
  pages.
- The clone stub loads the 32-bit `detached` stack argument with a 32-bit
  load (`mov r14d, [rsp+88]`); a qword load picks up the caller's unwritten
  upper half and makes a zero argument look nonzero.

A mutex is robust. The futex word holds the owner's task id plus the kernel's
`OWNER_DIED`/`WAITERS` bits, the node sits at `HAKU_PTHREAD_NODE_OFFSET` (16)
in every mutex so one `futex_offset` applies, and each thread's list head
lives in a static table indexed by task id — not in the TEB, which is already
unmapped by the time the exit syscall walks the list. A thread that dies
holding a mutex leaves the word owner-died; the next lock returns
`EOWNERDEAD`, `pthread_mutex_consistent` repairs it, and unlocking without
that call poisons it so later locks return `ENOTRECOVERABLE`.

`errno` is per thread through the TEB: `haku64_errno_slot()` reads GS:32
with `__readgsqword(0x30)`. `__declspec(thread)` was rejected for it because
the compiler then emits its own TEB layout and index for the variable, which
the library does not control.

`fork()` keeps only the calling thread. `haku64_after_fork` forgets every
inherited robust-list head and marks the surviving handle as this process's
main thread, so a forked child never joins or detaches the parent's thread.

## Signals

`signal64.c` exposes `sigaction`, `sigprocmask`, `sigaltstack`, `signal` and
`raise` over the Haku calls. The kernel queues the signal and delivers it on
the way back to ring 3, so `raise` returns normally and the handler starts with
a clean syscall behind it. Because the user stack is `NX`, the kernel cannot
write a trampoline there; `sigaction` therefore sets `SA_RESTORER` and points
the record at `haku64_restore_rt` in `crt64.asm`, which runs `rt_sigreturn`.
Passing a raw handler without that flag is refused with `EINVAL`.

A handler is compiled with the same Microsoft x64 convention as the rest of the
library, so the kernel passes the signal number in `RCX`; with `SA_SIGINFO` the
`siginfo` pointer is in `RDX` and the `ucontext` in `R8`. The saved context has
the kernel's register order (`r15`..`r8`, `rbp`, `rdi`, `rsi`, `rdx`, `rcx`,
`rbx`, `rax`, `rip`, `cs`, `rflags`, `rsp`, `ss`). An interrupted `SYSCALL`
leaves `RCX`/`R11` holding the entry RIP and flags, so those two fields of a
syscall-interrupted context carry no pre-call value; the ABI already treats
both as clobbered.

Signals are per task, not per thread group: a signal sent to a process reaches
one thread (the leader, or the first live thread when the leader is gone), and
`tgkill`/`raise` name a thread. A pending unblocked signal ends a blocking
`waitpid`, `nanosleep` or `poll` with `errno = EINTR`. `raise(SIGSTOP)` parks
the calling task and `kill(pid, SIGCONT)` resumes it; `waitpid` reports the
stop with `WUNTRACED` and the resume with `WCONTINUED`, reaping only on exit.
The terminal raises two signals itself: a console read from a background
process group stops that group with `SIGTTIN` (and the read fails with `EINTR`
after the resume, or with `EIO` if `SIGTTIN` is blocked or ignored), and a
session leader's exit sends `SIGHUP` to the console's foreground group. There
is no `sigqueue`,
no real-time range, no hardware-fault delivery (a ring-3 fault still ends the
group instead of entering a `SIGSEGV` handler), no `SA_RESTART` restart of the
interrupted call, and no `SIGTTOU` because the console never sets `TOSTOP`.
The foreground group itself is real state: a group with no live member is
refused by `tcsetpgrp` and never reported by `tcgetpgrp`.

## Console ioctls

`ioctl` in `unistd64.c` hands the request number to the kernel unchanged, and
the console is the only descriptor that answers. `TCGETS` reports the fixed
mode (`ICRNL|IXON`, `OPOST|ONLCR`, `ISIG|ICANON|ECHO|ECHOE|ECHOK`), `TCSETS`,
`TCSETSW`, `TCSETSF`, `TCSBRK` and `TCFLSH` succeed without changing it,
`TIOCGWINSZ` reports 80x25, and `TIOCGPGRP`/`TIOCSPGRP` read and write the
console's foreground process group (one console, so one stored group; the
session is not checked). The same stored group is what decides whether a
console read is a foreground one, and what a session leader's exit hangs up.
A group with no live member
is refused with `EPERM`, and a descriptor whose description is not the console
— including `/dev/null` and a pipe — fails with `ENOTTY`. A program that only
wants to know whether it is on a terminal can therefore rely on that error.

## Build pipeline

`python build.py` (x64) compiles `user/lib64/*.c` and `*.asm` and
`user/tests/init64.c` with the same compiler as the kernel, links a fixed-base
PE32+ with no CRT at `0x8000400000`, and `tools/pe2elf64.py` repacks it into an
ELF64 image with a header `PT_LOAD`, one `PT_LOAD` per RX/R/RW section, and a
`PT_TLS` pointing at the TLS template inside the read-only segment. `build.py`
then seeds the result into the FAT12 image as `INIT.ELF`.

The x64 build checks the two ABI headers before it calls the compiler:
`verify_abi_headers` / `verify_abi64_headers` compare every `HAKU64_*`
constant in `include/haku/abi64.h` against `include/haku/abi64.inc` and fail
the build when a number drifts. The comparison is one-way on purpose:
`abi64.inc` also carries NASM-only values (clone and futex flags, `poll`
events) that C does not need.

## The `/INIT.ELF` self-test

`test_libc64()` in `kernel/arch/x86_64/kernel/boot64.c` loads `/INIT.ELF` from
the disk, builds the same `argc`/`argv`/auxv stack `execve` does with
`sys64_build_init_stack` (`argv[0] = "/INIT.ELF"`), starts it with
`task64_create_user`, waits for it to finish and requires a clean exit status.
A failed check prints its own line naming the call and the line number, so a
regression says what broke instead of only changing an exit status:

| Marker | Meaning |
|---|---|
| `HAKU64-LIBC-START: argc=1` | the program reached `main` with the expected arguments |
| `HAKU64-LIBC-FAIL: <what> (line <n>)` | that check failed; the run fails |
| `HAKU64-USER-TLS-OK` | TLS initializer, zero fill, read/write, FS/GS bases and fork isolation all passed |
| `HAKU64-USER-THREAD-OK` | a cloned thread got its own stack, TLS block, FS and GS bases, and the parent's block stayed put |
| `HAKU64-USER-THREAD-JOIN-OK` | `CLONE_CHILD_SETTID` published the tid, the joiner slept on the clear-tid word and woke with the thread's status |
| `HAKU64-USER-PTHREAD-OK` | three `pthread_create` workers serialized 150 increments under a mutex and woke the main thread through a condition variable, then joined with their return values |
| `HAKU64-USER-DETACH-OK` | a detached pthread freed its own pages and `kill(tid, 0)` then reported `ESRCH` |
| `HAKU64-USER-ROBUST-OK` | a thread died holding a robust mutex: `EOWNERDEAD`, then `pthread_mutex_consistent` to repair it, and a second one left `ENOTRECOVERABLE` |
| `HAKU64-USER-PIPE-OK` | a 512-byte transfer through a 4 KiB ring arrived intact, `poll` saw data then an empty pipe, the last writer's close turned the next read into EOF, and `O_NONBLOCK` reported `EAGAIN` |
| `HAKU64-USER-PIPE-SIGPIPE-OK` | a forked child's write to a pipe with no reader ended it with the default `SIGPIPE` action instead of returning `EPIPE` |
| `HAKU64-USER-PIPE-BLOCK-OK` | a child's 8 KiB write to a 4 KiB ring blocked until the parent drained it, and every byte arrived intact |
| `HAKU64-USER-SIGNAL-OK` | a handler ran with the right signal number and its own signal blocked, the mask was restored, a blocked signal stayed pending until `SIG_UNBLOCK`, `SIG_IGN` dropped one, `SA_ONSTACK` put the frame on the alternate stack, and a signal ended `nanosleep` with `EINTR` and the remaining time |
| `HAKU64-USER-STOP-OK` | a child stopped itself with `SIGSTOP`, `wait4(WUNTRACED)` reported `(SIGSTOP << 8) | 0x7F` without reaping it, `kill(pid, 0)` still found it, `SIGCONT` resumed it, `wait4(WCONTINUED)` reported `0xFFFF` once, and the child then exited cleanly |
| `HAKU64-USER-TTY-OK` | `TCGETS` reported canonical mode, `TIOCGWINSZ` 80x25, `TIOCGPGRP`/`TIOCSPGRP` round-tripped the caller's group, an unknown group was refused with `EPERM`, and `TCGETS` on `/dev/null` failed with `ENOTTY` |
| `HAKU64-USER-TTY-SIGNAL-OK` | a background read with `SIGTTIN` blocked failed with `EIO`, the same read with the default disposition stopped on `SIGTTIN` and returned `EINTR` after `SIGCONT`, and a session leader's exit sent `SIGHUP` to the console's foreground group |
| `HAKU64-USER-ERRNO-THREAD-OK` | a thread's `errno` write did not reach its creator's |
| `HAKU64-USER-THREAD-EXEC-IMAGE-OK` | the image a thread `execve`d is running (printed by the new image) |
| `HAKU64-USER-THREAD-EXEC-OK` | the exec ended the group leader, which was reaped, and the thread's image exited with 42 |
| `HAKU64-LIBC-OK` | every check passed; required by the x64 marker list |
| `HAKU64-LIBC-RUN-OK` | the kernel reaped it with exit status 0 |

The checks cover string and memory functions, formatted output, `malloc`, the
file calls on the FAT12 volume (`open`/`read`/`write`/`lseek`/`stat`/`unlink`/
`mkdir`/`rmdir`/`getcwd`/`uname`, including the `errno` of a failed `open`),
`fork` with a copy-on-write child and `waitpid`, `mmap`/`munmap`,
`clock_gettime`/`nanosleep`, and thread-local storage: a static initializer, a
zero-filled variable, a write and read back, `arch_prctl` `GET_GS`/`GET_FS`,
`SET_FS` with a readback and an out-of-range rejection, and a forked child that
writes its own TLS block while the parent keeps the old value. The child
process verifies that it has its own pid and that `getppid()` names the parent,
which is what catches a forked task inheriting the parent's thread-group id.
`haku64_thread_create` is checked the same way: the thread starts on its own
stack with the template values, sees the parent's pid and its own tid, writes
its own TLS block, and exits as a detached task (the test polls
`kill(tid, 0)` for the reaped slot). The join test reads the tid back from the
handle and joins a thread that is still pausing, so the parent really sleeps
on the clear-tid word. The exec test forks, has the child's leader park on a
word nothing writes, and lets a thread of that child `execve` this same file
with `argv[1] = "thread-exec"`; the new image prints its marker and returns
42, and the parent reaps the ended leader (status 0) and the reparented
thread (status 42). Finally the pthread tests above run the same library
surface a real program would: create/join, a mutex and condition variable, a
detached worker, the two robust-mutex outcomes, and per-thread `errno`.

## Deliberate limits

- **Static only.** There is no dynamic linker, no `PT_INTERP`, no shared
  library and no relocations beyond what the fixed-base link produced.
- **No constructors or `atexit`.** `_start` calls `main` and `exit` directly.
  A static program that only uses this library needs no more.
- **No floating point in `printf`.** `%f`, `%e` and `%g` are not implemented;
  the kernel has no FPU context switch for user tasks beyond FXSAVE/FXRSTOR of
  the SSE state.
- **Signals have dispositions and a stop state, but no terminal-generated job
  control.** `sigaction`, `sigprocmask`, `sigaltstack`, `signal` and `raise`
  work, a handler needs the restorer `sigaction` supplies, and `SIGSTOP`/
  `SIGCONT` park and resume a task with `wait4` reporting both events. There is
  no `sigqueue`, no real-time range, no `SIGHUP`/`SIGTTIN`/`SIGTTOU` from the
  keyboard, and a hardware fault is never turned into a signal. `pipe`/`pipe2`
  and `poll` exist; a write with no reader fails with `EPIPE` and raises
  `SIGPIPE`, whose default action ends the process. `fcntl` is reached through
  the raw `haku64_syscall3` wrapper because the library does not wrap it yet.
- **`haku64_thread_create` is detached, not joinable.** Its thread frees its
  own stack and TLS block on return, so there is no status to collect; use
  `haku64_thread_spawn`/`haku64_thread_join` or `pthread_create` when the
  result matters.
- **`mmap` is anonymous-first.** An anonymous mapping needs `fd == (u64)-1`
  and takes its offset in bytes; there is no file-backed mapping from the
  library yet.
- **`printf` truncates at 1 KiB.** Each call formats into a buffer on its own
  stack, so concurrent calls cannot corrupt each other or `errno`, but output
  longer than the buffer is truncated rather than allocated.
- **8.3 filenames.** The volume is FAT12, so a path in the self-test is a short
  uppercase name.
