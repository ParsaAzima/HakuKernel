# The x86-64 user C library

[Back to documentation index](README.md)

`user/lib64/` is the C library an x86-64 ring-3 program links against. It
covers the subset of the POSIX surface that the kernel actually implements and
nothing else: no dynamic linking, no threads, no locale, no floating point.

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
| `crt64.asm` | `_start`: reads the initial stack image, calls `main(argc, argv, envp)`, exits with its return value. |
| `syscall64.asm` | `haku64_syscall0`..`haku64_syscall6`: the raw kernel entry, returning `-errno` unchanged. |
| `haku_stdarg.h` | A freestanding `<stdarg.h>`: MSVC's `__va_start` intrinsic, or the clang builtins. |
| `errno64.c` | `errno` and `haku64_errno_check`, which turns a kernel `-errno` into `-1` plus `errno`. |
| `string64.c` | `mem*` and `str*`: `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`. |
| `stdio64.c` | `printf`, `snprintf`, `puts`, `putchar` over one 1 KiB formatting buffer. |
| `memory64.c` | `malloc`, `calloc`, `realloc`, `free` (first fit over a `brk` heap with block splitting) and `atoi`. |
| `unistd64.c` | `read`, `write`, `open`, `close`, `lseek`, `dup`, `dup2`, `dup3`, `unlink`, `rename`, `mkdir`, `rmdir`, `chdir`, `getcwd`, `access`, `getpid`, `gettid`, `getppid`, `fork`, `waitpid`, `execve`, `exit`, `_exit`, `brk`, `sbrk`, `stat`, `lstat`, `fstat`, `nanosleep`, `sleep`, `clock_gettime`, `uname`. |
| `mman64.c` | `mmap`, `munmap`, `mprotect`. |

Everything returns `-1` and sets `errno` on failure, using the kernel's
`-errno` return value; the raw `haku64_syscall*` wrappers do not translate it.

## The Microsoft x64 ABI

The userland follows the Microsoft x64 calling convention, because the
kernel's build already requires `cl.exe`/`clang-cl` and both of those emit it:
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
           envp[0] .. , NULL                  (execve only)
           AT_PAGESZ, page size               (execve only)
           AT_ENTRY, entry point              (execve only)
           AT_NULL                            (execve only)
```

`execve` builds the full image; the kernel's own `/INIT.ELF` launch writes only
`argc`, `argv[0]`, the `argv` terminator and an empty `envp` terminator, which
is what `crt64.asm` reads. `envp` is empty either way: the kernel does not build
an environment yet.

There is no TLS, no `%fs` base, and no `arch_prctl(158)`. A program that needs
thread-local storage has to allocate it itself and keep it in a global.

## Build pipeline

`python build.py` (x64) compiles `user/lib64/*.c` and `*.asm` and
`user/tests/init64.c` with the same compiler as the kernel, links a fixed-base
PE32+ with no CRT at `0x8000400000`, and `tools/pe2elf64.py` repacks its
RX/R/RW sections into four ELF64 `PT_LOAD` segments. `build.py` then seeds the
result into the FAT12 image as `INIT.ELF`.

The x64 build checks the two ABI headers before it calls the compiler:
`verify_abi_headers` / `verify_abi64_headers` compare every `HAKU64_*`
constant in `include/haku/abi64.h` against `include/haku/abi64.inc` and fail
the build when a number drifts. The comparison is one-way on purpose:
`abi64.inc` also carries NASM-only values (clone and futex flags, `poll`
events) that C does not need.

## The `/INIT.ELF` self-test

`test_libc64()` in `kernel/arch/x86_64/kernel/boot64.c` loads `/INIT.ELF` from
the disk, lays out `argc = 1` and `argv[0] = "/INIT.ELF"`, starts it with
`task64_create_user`, waits for it to finish and requires a clean exit status.
A failed check prints its own line naming the call and the line number, so a
regression says what broke instead of only changing an exit status:

| Marker | Meaning |
|---|---|
| `HAKU64-LIBC-START: argc=1` | the program reached `main` with the expected arguments |
| `HAKU64-LIBC-FAIL: <what> (line <n>)` | that check failed; the run fails |
| `HAKU64-LIBC-OK` | every check passed; required by the x64 marker list |
| `HAKU64-LIBC-RUN-OK` | the kernel reaped it with exit status 0 |

The checks cover string and memory functions, formatted output, `malloc`, the
file calls on the FAT12 volume (`open`/`read`/`write`/`lseek`/`stat`/`unlink`/
`mkdir`/`rmdir`/`getcwd`/`uname`, including the `errno` of a failed `open`),
`fork` with a copy-on-write child and `waitpid`, `mmap`/`munmap`, and
`clock_gettime`/`nanosleep`. The child process verifies that it has its own pid
and that `getppid()` names the parent, which is what catches a forked task
inheriting the parent's thread-group id.

## Deliberate limits

- **Static only.** There is no dynamic linker, no `PT_INTERP`, no shared
  library and no relocations beyond what the fixed-base link produced.
- **No constructors or `atexit`.** `_start` calls `main` and `exit` directly.
  A static program that only uses this library needs no more.
- **No floating point in `printf`.** `%f`, `%e` and `%g` are not implemented;
  the kernel has no FPU context switch for user tasks beyond FXSAVE/FXRSTOR of
  the SSE state.
- **No threads, no signals, no pipes.** `fork`, `execve`, `wait4`, `clone` with
  `CLONE_VM` and `futex` exist in the kernel, but the library does not build a
  threading layer on them, and neither `pipe` nor `sigaction` has a syscall on
  the 64-bit side yet.
- **`mmap` is anonymous-first.** An anonymous mapping needs `fd == (u64)-1`
  and takes its offset in bytes; there is no file-backed mapping from the
  library yet.
- **One 1 KiB formatting buffer.** `printf` output longer than that is
  truncated rather than allocated, and the buffer is not thread safe.
- **8.3 filenames.** The volume is FAT12, so a path in the self-test is a short
  uppercase name.
