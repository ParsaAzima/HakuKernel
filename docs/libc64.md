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
| `crt64.asm` | `_start`: reads the initial stack image, finds the auxiliary vector, calls `haku64_tls_init`, calls `main(argc, argv, envp)`, exits with its return value. |
| `syscall64.asm` | `haku64_syscall0`..`haku64_syscall6`: the raw kernel entry, returning `-errno` unchanged. |
| `haku_stdarg.h` | A freestanding `<stdarg.h>`: MSVC's `__va_start` intrinsic, or the clang builtins. |
| `tls64.c` | `_tls_index` and `_tls_used`, the TLS template markers, `haku64_tls_init`: finds `PT_TLS` through `AT_PHDR`, maps the block and a TEB, and installs the user GS base. |
| `errno64.c` | `errno` and `haku64_errno_check`, which turns a kernel `-errno` into `-1` plus `errno`. |
| `string64.c` | `mem*` and `str*`: `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr`. |
| `stdio64.c` | `printf`, `snprintf`, `puts`, `putchar` over one 1 KiB formatting buffer. |
| `memory64.c` | `malloc`, `calloc`, `realloc`, `free` (first fit over a `brk` heap with block splitting) and `atoi`. |
| `unistd64.c` | `read`, `write`, `open`, `close`, `lseek`, `dup`, `dup2`, `dup3`, `unlink`, `rename`, `mkdir`, `rmdir`, `chdir`, `getcwd`, `access`, `getpid`, `gettid`, `getppid`, `arch_prctl`, `fork`, `waitpid`, `execve`, `exit`, `_exit`, `brk`, `sbrk`, `stat`, `lstat`, `fstat`, `nanosleep`, `sleep`, `clock_gettime`, `uname`. |
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
- **One TLS block per task.** `__declspec(thread)` and `arch_prctl` work, but
  `clone` has no `CLONE_SETTLS`, so the library cannot start a second thread
  with its own block. A forked child inherits the base and gets a copy-on-write
  view of the block; the parent's values stay put until it writes again.
- **`mmap` is anonymous-first.** An anonymous mapping needs `fd == (u64)-1`
  and takes its offset in bytes; there is no file-backed mapping from the
  library yet.
- **One 1 KiB formatting buffer.** `printf` output longer than that is
  truncated rather than allocated, and the buffer is not thread safe.
- **8.3 filenames.** The volume is FAT12, so a path in the self-test is a short
  uppercase name.
