<h1 align="center">
 Haku v0.2.0 — a tiny x86 operating system in C
</h1>

## Tech Stack

<p align="center">
  <img src="https://img.shields.io/badge/Language-C-A8B9CC?style=for-the-badge&logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/Assembly-NASM-333333?style=for-the-badge" alt="NASM" />
  <img src="https://img.shields.io/badge/Architecture-x86%20%2F%20i386-0071C5?style=for-the-badge" alt="x86" />
  <img src="https://img.shields.io/badge/Kernel-Bare%20Metal-222222?style=for-the-badge" alt="Bare Metal" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Compiler-MSVC%20%2F%20Clang--CL-5C2D91?style=for-the-badge&logo=visualstudio&logoColor=white" alt="MSVC and Clang-CL" />
  <img src="https://img.shields.io/badge/Linker-MS%20LINK%20%2F%20LLD-68217A?style=for-the-badge" alt="Linker" />
  <img src="https://img.shields.io/badge/Build-Python-3776AB?style=for-the-badge&logo=python&logoColor=white" alt="Python" />
  <img src="https://img.shields.io/badge/Assembler-NASM-333333?style=for-the-badge" alt="Assembler" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Emulator-QEMU-FF6600?style=for-the-badge&logo=qemu&logoColor=white" alt="QEMU" />
  <img src="https://img.shields.io/badge/Boot-BIOS-555555?style=for-the-badge" alt="BIOS" />
  <img src="https://img.shields.io/badge/Memory-Paging-0078D4?style=for-the-badge" alt="Paging" />
  <img src="https://img.shields.io/badge/Executable-ELF32-6E4C13?style=for-the-badge" alt="ELF32" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Filesystem-FAT12-4CAF50?style=for-the-badge" alt="FAT12" />
  <img src="https://img.shields.io/badge/Storage-ATA%20PIO-607D8B?style=for-the-badge" alt="ATA PIO" />
  <img src="https://img.shields.io/badge/Interface-VFS-795548?style=for-the-badge" alt="VFS" />
  <img src="https://img.shields.io/badge/ABI-i386%20Syscalls-455A64?style=for-the-badge" alt="i386 syscall ABI" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/CI-GitHub%20Actions-2088FF?style=for-the-badge&logo=githubactions&logoColor=white" alt="GitHub Actions" />
  <img src="https://img.shields.io/badge/Platform-Windows%2011-0078D4?style=for-the-badge&logo=windows11&logoColor=white" alt="Windows 11" />
  <img src="https://img.shields.io/badge/Testing-Headless%20QEMU-FF6600?style=for-the-badge" alt="Headless QEMU testing" />
</p>

<p align="center">
  <a href="https://github.com/ParsaAzima/HakuKernel/actions/workflows/ci.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/ParsaAzima/HakuKernel/ci.yml?branch=main&style=for-the-badge&label=CI" alt="CI status" />
  </a>
  <img src="https://img.shields.io/badge/Version-v0.1-blue?style=for-the-badge" alt="Version v0.1" />
  <img src="https://img.shields.io/badge/License-See%20LICENSE-lightgrey?style=for-the-badge" alt="License" />
</p>

---


[![CI](https://github.com/ParsaAzima/HakuKernel/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ParsaAzima/HakuKernel/actions/workflows/ci.yml)

Bootloader (NASM) loads a C kernel and switches the CPU to 32-bit protected
mode. The kernel initializes paging, interrupts and a GDT/TSS, then reads
user ELF programs from a FAT12 filesystem on an ATA disk and schedules them
at ring 3.

## Prerequisites (Windows 11)

| Tool | Version here | Notes |
|---|---|---|
| Visual Studio 2022 | 17.x, **Desktop development with C++** | we use the x86 `cl.exe` directly — no project/solution needed |
| NASM | 3.02 | `winget install -e --id NASM.NASM` |
| QEMU | 11.x | `C:\Program Files\qemu\qemu-system-i386.exe` |
| Python 3 | 3.14 | drives the whole build (`build.py` + helpers) |

## Build & run

```cmd
cd D:\Source\Haku
python build.py                 :: build haku.img only (data image preserved)
python build.py run             :: build + boot in a QEMU window
python build.py run --fresh     :: recreate the FAT12 data image, then boot
python build.py headless        :: fresh data image + boot headless, verify
python build.py shell           :: boot a busybox ash shell (interactive)
python build.py shell --check   :: scripted shell run, assert its markers
python build.py x64             :: build haku64.img (64-bit kernel) and boot it
python build.py probe --init=X  :: boot an external ELF as /init, check markers
python build.py --memory=1024   :: guest RAM in MiB (default 512, kernel cap 1020)
python build.py inspect ...     :: examine a FAT12 image (list/check/extract)
```

The kernel does not assume a memory size: it reads the E820 map, sizes the
supervisor direct map and the frame pool from it, and reports both in the
boot log (`MEM-OK: usable=…kB free=…kB direct-map=…MB`). `--memory=` only
changes how much RAM QEMU gives the guest; the kernel uses whatever it finds,
up to the 1020 MiB the 32-bit address space can map without PAE.

`python build.py headless` verifies ATA detection on both legacy channels
(`ATA-SCAN-OK devices=2`), the software-reset recovery path
(`ATA-RECOVERY-OK`), cross-channel sector reads (`ATA-MULTI-OK`),
FAT12 mounting and
multi-cluster file reads, subdirectory/path resolution (`USER-DIR-OK`),
ring-3 process entry, per-process kernel stacks,
syscall handling, `spawn`/`waitpid` and exit-status reporting, page-protection
faults (`USER-PROT-OK`), `execve` image replacement, process exit/reaping,
copy-on-write `fork` and `vfork` (`USER-COW-OK`, `USER-VFORK-OK`,
`VFORK-EXEC-RAN`), signal delivery and dispositions (`USER-SIGNAL-OK`),
the user C library (`USER-LIB-OK`), and timer IRQs
through the serial log.

`python build.py shell` boots something else entirely: a real static
`busybox` (musl, i386) as PID 1, so the guest comes up in `ash` instead of
Haku's own test userland. The image is a separate FAT12 volume
(`build/haku_shell.img`) holding `BUSYBOX` and, for the scripted run, a
one-line `INIT.CMD`; the kernel prefers `disk/BUSYBOX` over the initramfs
when it is there and hands it `sh -c "…"`. busybox is 1 MiB and belongs to
someone else, so it is cached under `build/` and never committed —
`tools/getbusybox.py` fetches the Alpine `busybox-static` build on first use
and `--busybox=PATH` points the build at a local copy instead (offline
machines, CI). `--cmd=TEXT` replaces the boot command line, and
`shell --check` runs the fixed script
(`mkdir -p /bin`, `--install -s /bin`, `export PATH=/bin`, then `uname`,
`ls /bin | wc -l`, `ls /`, `cat /proc/version`) and asserts
`SHELL-BOOT`, `SHELL-OK`, `UNAME:haku`, `APPS:<count ≥ 100>`, `HakuKernel`,
and `SHELL-DONE`. Today it reports 306 applets resolved through `$PATH`.

### Data image lifecycle

`build/haku_fs.img` is the ATA data disk and is treated as user data: an
existing image is reused untouched, so files created in a previous QEMU
session survive the next build. It is (re)created from `kernel/arch/x86/user/user.asm`,
`kernel/arch/x86/user/exec_target.asm`, `kernel/arch/x86/user/child.asm`,
`kernel/arch/x86/user/vfork_target.asm`, the
user library in `user/lib/`, and `user/tests/libtest.c` only when missing or
with `--fresh`. If any test-program source is newer than a reused image,
the build warns that its test ELFs on disk may be stale. `headless` always
starts from a fresh image (its fault-injection tests mutate the disk) and,
after the serial-log check passes, re-verifies the post-run image on the
host side.

`python build.py inspect` (default: `build/haku_fs.img`) lists the root
directory and FAT summary without touching the image; `--chains` also prints
cluster chains, `--cat NAME.EXT` / `--dump NAME.EXT OUT.BIN` extract a file,
and `--check` enforces the kernel's mount invariants (BPB sanity, mirrored
FATs, exact chains, no orphans) with a nonzero exit on any damage.
The build is one Python script on purpose — `cmd.exe` quoting of spaced
paths (e.g. `C:\Program Files\...`) is fragile, while `subprocess` argument
lists just work.

### Continuous integration and releases

`.github/workflows/ci.yml` runs on every push to `main`, every pull request,
and manual dispatch. It uses `ubuntu-latest`, installs NASM, Clang/LLD, and
QEMU from apt, runs `python build.py headless` through the
`clang-cl`/`lld-link` path in `build.py`, and uploads `haku.img`, the data
image, serial/QEMU logs, and any crash screenshots as artifacts even when the
boot check fails. A missing marker, an unexpected `*-FAIL` marker, or
`KERNEL PANIC` fails the job.

A second job, `musl`, builds `user/tests/musl_probe.c` with `musl-gcc
-static` and boots it as `/init` with
`python build.py probe --init=build/musl_probe --expect=MUSL-PROBE-OK`. This
is the only check that runs an outside C library on top of the kernel, so it
is what proves the ABI claims above: musl's own `_start`, auxiliary-vector
parsing, `%gs`-based TLS, `writev`-backed stdio, and file/directory wrappers
all have to work for the probe to print `MUSL-PROBE-OK`. The probe binary is
built outside Haku's toolchain and packed into the initramfs as `/init`; the
`probe` mode boots it without the keyboard injection the headless suite needs
and asserts `HAKU-BOOT-OK`, `INITRAMFS-UNPACK-OK`, `INIT-READ-OK name=init`,
`USERMODE-ENTER`, and the requested marker.

`.github/workflows/release.yml` runs for `v*` tags. It repeats the headless
check, deletes the mutated test image, rebuilds a pristine
`build/haku_fs.img`, and publishes `Haku-<tag>.zip` containing `haku.img`,
`haku_fs.img`, `README.md`, `LICENSE`, and `SHA256SUMS.txt`:

```bash
git tag v0.2.0
git push origin v0.2.0
```

Slow runners can tune headless timing with `HAKU_BOOT_DELAY` (seconds before
the injected key, default 1) and `HAKU_HEADLESS_TIMEOUT` (seconds to wait for
guest shutdown, default 30).

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
| `0x7C00` | boot sector (512 B) |
| `0x10000–0x80000` | kernel load window (up to 448 KiB, linked `/BASE:0x10000 /ALIGN:16`) |
| `0x7000–0x7240` | E820 map from stage2 (INT 15h, count at `0x6FFC`) |
| `0x9000–0xC000` | page directory + initial tables (8 MiB supervisor identity map) |
| `0x1000–0x4000` | x86-64 boot: PML4, PDPT and PD (1 GiB identity map, 2 MiB pages) |
| `0x400000–0x500000` | direct-map page tables (one per 4 MiB of mapped RAM) |
| `0xC0000000–0xC0400000` | supervisor framebuffer window (one page-directory entry) |
| `0xC0400000–0xFFFFFFFF` | supervisor direct map of physical `0–1020 MiB`, sized from E820 at boot |
| `0x100000–0x400000` | kernel heap (kmalloc/kfree, 16 B aligned) |
| `0x90000` | bootstrap kernel stack top (grows down) |
| low E820 frames (often 4–8 MiB) | per-process 8 KiB kernel stacks, accessed through the direct map |
| `0x40000000` | user program code/data virtual base (per-process mapping) |
| `0x50000000–0x54000000` | user `brk` heap region (lazily mapped) |
| `0x60000000–0x70000000` | anonymous `mmap` region (lazily mapped) |
| `0xBFFFE000–0xC0000000` | user stack (two pages initially, grows downward on fault up to 1 MiB) |
| `0xB8000` | VGA text buffer |

### x86-64: boot path, memory and interrupts (work in progress)

`python build.py x64` produces `haku64.img` from the same stage1/stage2
floppy, with a 64-bit kernel entry instead of the 32-bit one. Stage 2 still
leaves the CPU in 32-bit protected mode; `kernel/arch/x86_64/boot/entry64.asm`
takes over from there, enables PAE and `EFER.LME`, installs a PML4/PDPT/PD
identity map of the first 1 GiB with 2 MiB pages, and far-jumps to a 64-bit
code segment.

From `kernel64_main` the 64-bit kernel brings up, in order:

| Step | What it builds | Marker |
|---|---|---|
| E820 | physical inventory from the map stage2 left at `0x7000` | `HAKU64-E820-OK` |
| paging | supervisor physmap of all RAM at `0xFFFF800000000000`, 2 MiB pages | `HAKU64-PAGING-OK` |
| frames | E820-backed frame allocator, bitmaps sized from the detected limit | `HAKU64-FRAME-OK` |
| heap | 8 MiB free-list `kmalloc64`/`kfree64` window | `HAKU64-HEAP-OK` |
| GDT/TSS | kernel and user segments, a 64-bit TSS, IST1/IST2 stacks | `HAKU64-GDT-OK` |
| IDT/PIC | 256 gates, 8259 remapped to `0x20`/`0x28`, IRQ0 unmasked | `HAKU64-IDT-OK` |
| exceptions | `INT3`, `#PF` and `#DE` provoked and resumed | `HAKU64-EXC-OK` |
| timer | 100 Hz PIT, plus a bounded wait and an `HLT` wake-up | `HAKU64-TIMER-OK` |
| self-test | physmap alias, alloc/free, a 4 KiB mapping, heap coalescing | `HAKU64-SELFTEST-OK` |

The physmap is why the 64-bit build has no equivalent of the 32-bit
1020 MiB ceiling: one page directory covers a GiB with 2 MiB pages, so
mapping 5 GiB costs five directories and no page tables. A 4 GiB guest
reports `physmap=5120MiB` and a 5120 MiB frame limit; the 32-bit image on
the same machine stops at 1020 MiB.

Interrupts follow the 32-bit model with 64-bit frames: `isr64.asm` has one
stub per vector, pushes a zero in the error-code slot for the vectors the CPU
does not supply one for, and hands a `regs64_t` to `isr64_dispatch`. Two
things are new in long mode. Gates are 16 bytes and carry an IST index, so
`#DF` runs on IST1 and NMI on IST2 — a double fault is usually the *result*
of a bad kernel stack, and without a switch it could not be reported. And the
TSS exists purely to describe `RSP0` and those stacks, since long mode has no
hardware task switching.

Because MSVC x64 has no inline assembly, `CLI`/`STI`/`LIDT`/`LGDT`/`LTR`,
port I/O, `CR2` and `PAUSE` all live in `isr64.asm` as small callable
functions. The same file holds three exception probes (`int3_probe64`,
`probe_load64`, `probe_div64`). Recovering from a fault means skipping the
faulting instruction, so instead of decoding it each probe exports a label
for the instruction *after* the one that faults and the handler sets `RIP` to
it — no hardcoded instruction lengths, and the test fails if the handler's
frame is wrong.

`gdt64_load` replaces the boot GDT with `LGDT` and reloads only the data
segments. CS is deliberately not reloaded: the new table keeps the boot code
descriptor byte-identical at selector `0x08`, so the cached descriptor stays
valid. Index 1 must therefore remain the kernel code descriptor.

The 64-bit path still has no scheduler, syscalls, or userland, and `haku.img`
remains the default image; ROADMAP.md (Milestone 8) lists the remaining
stages. The 64-bit sources are excluded from the 32-bit link line, so both
targets build from one tree.

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
1020 MiB the 32-bit address space can address. `frame_usable_count()` reports
the E820-usable pages inside that window, and every tracked frame is
allocatable: a permanent supervisor direct map at `0xC0400000` addresses
physical `0–1020 MiB`, and its PDEs are copied into each user page directory.
The bitmaps, the direct-map page tables, and the per-frame reference counters
are all sized at boot (heap allocations and low RAM), so the kernel image does
not carry a worst-case array. The self-test chain-allocates past the old
8 MiB identity boundary, writes/reads a high frame through `PHYS_TO_VIRT`,
and probes the last usable frame, which fails if the map stops short of the
installed RAM. Freeing validates alignment, usability, and allocation state
so reserved pages cannot accidentally be released.

### Processes and preemptive scheduling

`kernel/kernel/scheduler.c` starts the initial process from `HAKU.ELF`; a successful
`spawn` creates a child process from a root-filesystem ELF, `fork` creates one
as a copy of the caller, and `waitpid`/`wait4` suspends the parent until the
requested child exits. Exit status is returned in the traditional high-byte
wait status format; a successful wait reaps the child record. The current
spawn interface accepts a pathname only and starts the child with empty
argument and environment vectors; the child inherits a copy of the parent's
descriptor table (standard descriptors and any open files, each sharing the
parent's open file description). `waitpid` accepts a specific child PID or
`-1` (any child) and currently supports only options `0`; `wait4` follows the
Linux call, accepting `WNOHANG` and writing a zeroed `struct rusage` (the
per-process accounting described below is not yet wired into it).
`exit_group` ends the calling process like `exit`. `clone` is accepted only
for the `SIGCHLD` form (`fork` with no sharing flags) and returns `EINVAL` for
the thread-creating flags, stack pointer, or parent-tid arguments, so no
half-supported threading model is exposed. A forked child gets a private copy
of the address space (code, data, stacks, heap, and mappings), a copy of the
caller's saved register frame with `eax` zeroed, and its own descriptor table;
the parent sees the child PID. Pages are shared copy-on-write: each frame
carries a reference count, both address spaces keep the same read-only
mapping, and the first write on either side takes a protection fault that
hands the writer a private copy (the kernel also breaks the share before
writing through the direct map on the caller's behalf). `vfork` goes further
and shares the address space outright: the child runs in the parent's space
and on its stack while the parent stays suspended, and the parent resumes
with the child PID as soon as the child calls `execve` or exits. `execve`
replaces a process image from the root filesystem.
Each process has its own address space and an 8 KiB ring-0 stack; the stack
is allocated as physical frames and addressed through the kernel direct map.
`gdt_set_kernel_stack` updates `TSS.esp0` at every task switch so the next
ring-3 interrupt or syscall enters on the correct stack. The 100 Hz PIT
scheduler saves and restores complete interrupt frames through `iret`.
User exceptions and `exit` mark a process for reaping; its address space and
kernel stack are reclaimed after execution has moved to another stack. When
no user process is runnable, the scheduler runs a dedicated kernel idle
context.

`kernel/arch/x86/kernel/gdt.c` installs flat ring-0/ring-3 code and data descriptors plus a
32-bit TSS. `kernel/arch/x86/mm/paging.c` gives each process supervisor-only kernel
identity and direct-map entries and maps its user program and two-page user
stack with user permissions.

`kernel/arch/x86/user/user.asm` is built as an ELF32 image and entered via `iret` at CPL 3.
It tests `spawn` (`0x1000`, Haku-specific), `fork` (2), `wait4` (114), and
`waitpid` (7), checks that a forked child really shares pages copy-on-write,
runs `vfork` both to a plain `exit` and across an `execve`, as well as
`execve` (11), which loads another ELF32 image, builds an aligned
initial user stack containing `argc`, `argv`, `envp`, and a terminating
`AT_NULL` auxiliary-vector entry, and replaces the calling process without
changing its PID or open file descriptors. Each vector is limited to 64
strings; their strings are limited to 4096 combined bytes including NUL
terminators and must fit the two-page initial stack. Larger inputs return
`E2BIG`, and invalid user pointers return `EFAULT`. The first process starts
on a stack built the same way, with `argv[0]` set to the program name, so a
binary written for Linux finds its arguments where its startup code expects
them. The auxiliary vector carries `AT_PHDR`, `AT_PHENT`, and `AT_PHNUM`
(where the program headers are, which a static libc walks to find its own
`PT_TLS` segment), `AT_PAGESZ`, `AT_CLKTCK`, `AT_BASE`, `AT_FLAGS`,
`AT_ENTRY`, the identity words, `AT_SECURE`, and `AT_RANDOM` pointing at 16
seed bytes. Haku's own images start their first segment at a page-aligned
file offset, so the header table is not part of any segment; the loader maps
a copy on a page just below the stack and points `AT_PHDR` there, which is
what keeps the vector meaningful for every image.

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

### Per-process memory and resource accounting

`paging_map_user_page` and `paging_unmap_user_page` maintain per-address-space
counters for present user pages, non-directory user page tables, and a
mapped-page high-water mark. ELF loading, `brk`, `mmap`, and `munmap` all go
through those helpers, so the counters follow every mapping change without a
table walk. The scheduler attributes each 100 Hz timer tick to the interrupted
context by its CS privilege level, counts syscalls at the `int 0x80` entry,
and records a voluntary switch when a task blocks, sleeps, or waits and an
involuntary switch when the timer moves a runnable task aside. `execve`
preserves the process's peak and resource totals while replacing the current
image's memory counters with those of the new address space.

`getusage` (0x1001, Haku-specific) copies a 64-byte `haku_usage_t` to a
validated user buffer. It reports current and peak mapped pages, page-table
and kernel-stack pages, current `brk` and anonymous `mmap` bytes, and derived
resident/kernel/total byte counts, along with the pid, user/kernel timer
ticks, syscall count, and voluntary/involuntary context switches.
`resident_bytes` covers mapped user pages; `kernel_bytes` covers the user page
tables, the page directory, and the 8 KiB kernel stack; `total_bytes` is their
sum. A NULL or unmapped buffer returns `EFAULT`. Accounting is per process,
not per thread (the kernel has no threads), there is no demand paging
apart from the user stack, and ticks have 10 ms granularity.

### User-mode system calls

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
with pid 0 targets the caller and pid -1 targets every other process,
because Haku has no scheduler process groups; any other negative pid gives
`ESRCH`.
`fork` and
`vfork` reset the dispositions to the default and keep the blocked set;
`execve` resets the dispositions and keeps both the blocked set and the
pending signals. The C library wrappers live in `user/lib/signal.c` behind
`include/haku/signal.h`, and the ring-3 self-test covers delivery, blocking,
`SIG_IGN`, `SA_SIGINFO`, `SA_RESTORER`, the pid/tid forms, the default
fatal disposition, and the `EINTR` wake-up (`USER-SIGNAL-OK`). Haku has no
scheduler process-group model, no job control (the stop signals end the
process), no `sigqueue`, and one thread per process, so a thread id is a
process id.

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
`/dev/console`) answers the same requests, and `TIOCGPGRP`/`TIOCSPGRP`
report and accept the caller's pid as the foreground process group while
`TIOCNOTTY` is accepted. Haku has no scheduler process groups, so `getpgrp`,
`getpgid`, `getsid` and `setsid` report the caller's pid and `setpgid`
accepts only the caller's own group — enough for an interactive `ash` to
turn job control on instead of announcing that it cannot access a tty. `poll` reports readiness per descriptor (a
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
`USER-INITRAMFS-OK` is the boot-namespace check described under "The boot
namespace" below: it runs as a process started from the unpacked archive, so
a broken mount table or a missing `/init` fails the build before anything
else can pass.

### Supported POSIX subset

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
  `getdents`, `getdents64`, `poll`, `select`, and
  `chmod`/`chown`/`fchmod`/`fchown` (accepted,
  nothing to change).
- **Time:** `gettimeofday`, `nanosleep`.
- **Haku-specific:** `spawn`, `getusage`, `futex`.

Deliberately absent: scheduler process groups (the group and session calls
report pid-as-group so a shell can enable job control, but stop signals still
end a process), threads (`clone` rejects the thread flags), hard links,
file-backed mappings, `socket`, `mount` (the boot namespace is fixed at
boot), and networking.
Where a call exists but cannot be honoured — permission bits, record locks,
terminal modes — it succeeds
without changing state and the limitation is documented above instead of
returning a surprise error.

### Manual testing and image recovery

`python build.py run` boots the floppy in a QEMU window with the same disks
as the automated run, so the whole flow can be driven by hand: the injected
key only exists in `headless`, so press any key yourself at the read prompt.
`build/serial.log` is only written by `headless`; in a windowed run the same
text appears on the VGA/framebuffer console.

A data image that a test corrupted can be thrown away safely: delete
`build/haku_fs.img` or run `python build.py --fresh` (add `headless` to also
run the tests), and the image is rebuilt from the test programs. Before
discarding one, `python build.py inspect --check` says whether it is really
damaged — it verifies the BPB, both FAT mirrors, every cluster chain, and
orphans, and exits nonzero on any inconsistency.

### User C library

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
  with a `%c/%s/%d/%u/%x/%X/%p` subset (no floats, 512-byte output buffer).
- `user/lib/crt0.asm` supplies `_start`: it calls `main(void)` and exits with
  the returned status.

`build.py` compiles the library and `user/tests/libtest.c` with the same
`cl.exe` (or `clang-cl`) used for the kernel, links a fixed-base PE32 at
`0x40000000` with no CRT, and `tools/pe2elf.py` repacks its RX/R/RW sections
into ELF32 `PT_LOAD` segments. The image is installed as `LIB.ELF`;
`NEXT.ELF` spawns it, requires a clean exit status, and the run prints
`USER-LIB-OK`. There is no C runtime, dynamic linker, standard header set,
floating-point formatting, or thread-safe allocator, and data-image filenames
remain 8.3. The user ELFs are regenerated on every build, but an existing
`build/haku_fs.img` only receives `LIB.ELF` after `--fresh` or `headless`.

### Terminal, display, and input

`kernel/drivers/terminal.c` is the text-I/O layer. Output paths (`klog`, the
exception handlers, and the console device) call `terminal_putc` /
`terminal_write`, which render to the active display backend and mirror every
byte to COM1 with CRLF line endings. A group of bytes is written under one
hold of the serial port's IRQ-save lock, and the klog assembles a whole log
line before handing it over, so a kernel log line and a user write never land
inside each other: the markers the headless check looks for stay contiguous
in `build/serial.log`. The console device writes in 256-byte chunks so the
interrupt-off window stays short (the UART runs at 38400 baud, so a chunk
takes about 70 ms). Input is a 128-byte single-producer /
single-consumer ring owned by the terminal: `kbd.c` translates set-1 scancodes
and calls `terminal_input_push`, while `terminal_getchar`/`terminal_read` park
on the terminal's wait queue until IRQ1 wakes them. Neither the keyboard
driver nor the syscall layer knows about the other.

Display backends implement the shared `display_ops_t` table in
`kernel/drivers/display.h` (`name`, `clear`, `putc`, `status`). The VGA text
backend is selected at boot and stays the fallback; when the bootloader
hands over a usable VBE mode, `framebuffer_init` succeeds and the boot path
switches the active backend with `display_use(&framebuffer_display)`. Backend
state is protected with IRQ-save spinlocks because the timer redraws the
status line from interrupt context.

### Logging, panic, and crash reports

`kernel/kernel/klog.c` provides five severities (`DEBUG`, `INFO`, `WARN`,
`ERROR`, `CRIT`) with an `INFO` default threshold. `klog_log` and
`klog_begin` add the tag and honour `klog_set_level`; the plain
`klog_write`/`klog_prefix` path stays unconditional for boot markers and
test output. `klog_self_test` proves that `DEBUG` is suppressed at the
default threshold and appears after lowering it (`KLOG-LEVELS-OK`).

`kernel/kernel/panic.c` is the fatal path. It disables interrupts, prints
`KERNEL PANIC: <reason>`, and calls `crash_report` (`kernel/kernel/crash.c`),
which dumps `CR0`/`CR2`/`CR3`, the current pid, the exception frame
(`vector`, `err`, `eip`, `cs`, `eflags`), the general registers, segment
registers, and the interrupted stack. Ring-0 stacks are dumped directly;
ring-3 stacks are dumped only after `paging_user_range_valid` confirms they
are mapped, so a bad user stack cannot fault the reporter. The CPU is then
parked in `cli; hlt`. Boot self-test failures (`PAGING`, `HEAP`, `FRAME`,
`GDT`, `usermode`) and fatal kernel exceptions use the same handler.

Contained ring-3 faults use the same report but terminate only the faulting
process, and the first one logs `CRASH-REPORT-OK`. QEMU captures the report
in `build/serial.log`; `build.py` also passes `-d int,cpu_reset` and writes
`build/qemu.log`, and a headless run fails if `KERNEL PANIC` appears. To
exercise the panic path without changing code, boot with a QEMU monitor and
send `nmi` while the idle loop is running: the serial log then shows
`KERNEL PANIC: NMI` followed by the `PANIC:` register and stack dump.

### ATA PIO and FAT12

`kernel/drivers/ata.c` probes the legacy primary and secondary ATA channels
(`0x1F0`/`0x3F6` and `0x170`/`0x376`) at both master and slave positions,
ignores floating buses and ATAPI devices, and keeps every usable disk in a
device list. Devices are identified with IDENTIFY; LBA28 is used when
available and LBA48 addressing is the fallback. Identify, read, write, and
cache-flush commands use bounded polling PIO: a failed or timed-out command
is retried after a software reset (SRST) up to a fixed number of attempts,
and every write is followed by FLUSH CACHE before it is reported complete
(a drive that rejects the flush keeps the accepted data and only logs a
warning). PIO polling is retained deliberately: the initial FAT mount runs
before the PIC and PIT are initialized, while IRQ completion requires
remapping/unmasking IRQ14/15 and DMA completion adds PCI bus-master
programming and physical-buffer constraints. The synchronous path keeps boot
deterministic and headless-testable; AHCI, PCI native-mode controllers, and
ATAPI are not supported. The boot self-tests prove the recovery path
(`ATA-RECOVERY-OK`: a software reset followed by an identical sector reread)
and cross-channel I/O (`ATA-MULTI-OK`: the second device answers a sector
read), and the headless run checks the scan itself (`ATA-SCAN-OK devices=2`).
`kernel/fs/fat12/fat12.c` mounts a
FAT12 superfloppy from any detected ATA disk (the first disk that carries a
valid BPB wins, so the volume may live on the secondary channel or on a
slave) and supports creating, reading, writing, truncating and
seeking 8.3-named files anywhere in the tree, plus `mkdir`/`rmdir` of
single-cluster subdirectories with real `.`/`..` entries. Paths are
`/`-separated and resolved from the root; `.` and `..` are handled during
resolution. Cluster allocation, release, mirrored FAT updates and
volume/chain bounds checks are shared by files and directories.
`kernel/fs/vfs/vfs.c` supplies per-process descriptors and the user `open`, `close`,
`read`, `write`, and `lseek` interface through a filesystem-independent
operations vector (`vfs_fs_ops`: mount/open/close/read/write/seek/unlink/
rename/mkdir/rmdir/stat/lstat/fstat, each returning 1 or a `-VFS_E*` errno).
VFS never names a concrete filesystem; each backend registers the vector and
the VFS binds it to a directory path in a mount table. Path resolution picks
the longest matching prefix, so `/disk/HAKU.ELF` reaches the FAT12 backend as
`HAKU.ELF` while `/dev/null` reaches the device backend as `null`; the mount
root itself is `"."`, the same token the syscall layer hands the VFS after
canonicalizing `/`. A mount is read-only when it is created with
`VFS_MOUNT_RDONLY`, and VFS then returns `EROFS` for every mutating operation
instead of calling the backend, including writes through a descriptor that
was opened before the remount. A mount point cannot be unlinked, renamed, or
removed (`EBUSY`), a `rename` that would cross two mounts reports `EXDEV`, and
a mount point directory must exist in the parent filesystem so it shows up in
a listing. Disk I/O is serialized
against the preemptive scheduler; file contents and directory/FAT metadata
are written through to the ATA disk. The current VFS supports up to five
simultaneously open regular files per process (descriptors 3-7; `0`/`1`/`2`
are the console) and the `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`,
`O_EXCL`, `O_TRUNC`, and `O_APPEND` flags.

FAT12 remains intentionally small as a *data-disk* format: no partition
tables, long filenames, permissions, journaling, FAT16, or FAT32 (see the
next section for why). Directories are one cluster (16 entries) and do not
grow; moving a directory
across parents with `rename` leaves its `..` entry pointing at the old
parent, so directories should be renamed in place. The userland root is the
RAM filesystem described below, which has none of those limits. The QEMU
headless test creates `P1.TXT`, exercises `DIR1/T.TXT` with `./DIR1/../DIR1`
paths, and verifies multi-sector writes, reads, seeks, close, and reopen.

#### Why FAT12 with 8.3 names, and not something bigger

Long filenames, partition tables, and FAT16/FAT32 are each a deliberate
non-goal for the data disk, not an oversight. The reasoning, recorded so the
decision does not have to be re-derived:

- **Long filenames (VFAT LFN).** LFN entries are a chain of hidden records
  with a checksum tying them to the 8.3 name, ordered in reverse and
  contiguous; every create, rename, unlink, and directory-growth path would
  have to maintain that chain, and a torn write leaves an orphaned fragment.
  Haku's userland root is moving to an in-memory filesystem with real long
  names and permission bits (see `ROADMAP.md` milestone 7), which leaves FAT12
  as an interchange format for small data disks where 8.3 names are enough.
  Paying LFN complexity in the interchange format buys nothing the userland
  filesystem does not already provide.
- **Partition tables.** The kernel owns the whole disk image: the FAT12 volume
  starts at LBA 0. Adding an MBR would mean parsing a partition table, keeping
  a per-partition byte offset through every cluster calculation, and choosing a
  boot partition — for a disk no other operating system shares. If Haku ever
  has to coexist with another OS on one physical disk, this becomes worth
  doing, and the offset layer is the natural place to add it.
- **FAT16/FAT32.** The current images are 1.44 MB, where FAT12 is the correct
  encoding (a 12-bit entry per cluster). FAT16 becomes relevant only for data
  disks above roughly 16 MiB at 4 KiB clusters, and FAT32 brings long names,
  FSInfo, and mirroring rules with it. Both would be new backends with their
  own on-disk structures rather than a flag on the existing one.

Any of these can be revisited, but only together with interoperability tests:
before such a change lands, `mtools`/`fsck.fat` must be able to read, write,
and check the resulting image in CI, and `tools/lsfat12.py` must agree with
them. A filesystem that only Haku can read is not a filesystem Haku can
export data through, and the roadmap treats that gate as mandatory rather than
optional.

`tools/mkfat12.py` puts `HAKU.ELF`, the exec target `NEXT.ELF`, and the
spawn/wait child `CHILD.ELF` on a generated 1.44 MB FAT12 image, together with
the initramfs archive `INITRD` (no extension, because the volume only stores
8.3 names). QEMU
attaches this image as the primary-master IDE disk while the existing boot
floppy still starts the kernel; the build also attaches a generated
1.44 MB spare disk as secondary master so the headless run exercises
device scanning across both legacy channels. The kernel
loads the initial ELF at startup; `execve` uses VFS to load subsequent ELF
files into a fresh address space before replacing the current image.

#### The boot namespace: initramfs, ramfs, `/dev`, and `/proc`

The root of the userland namespace is the RAM filesystem, not the FAT12
volume. At boot the kernel mounts `ramfs` on `/`, creates the `/disk` mount
point directory, mounts the FAT12 data disk there, reads `disk/INITRD`, and
unpacks the cpio archive into the RAM root. `/init` from that archive runs as
PID 1; the FAT volume stays reachable at `/disk`, and an image without an
archive still boots the old way with FAT12 as the root.

- **`ramfs`** (`kernel/fs/ramfs`) is the writable root: long names (48 bytes
  per component), permission bits, timestamps from the PIT, nested
  directories, and symbolic links with a hop limit of 8. Nodes come from a
  fixed 512-entry table and file data from the kernel heap (256 KiB per
  file, which is why the busybox shell loads its 1 MiB binary from the FAT12
  disk and not from an initramfs),
  so nothing survives a reboot — which is exactly what an initramfs needs.
  Unlinking a file that is still open keeps the data alive until the last
  descriptor closes, the same promise FAT12 makes.
- **`devfs`** (`kernel/fs/devfs`) is a fixed table of `/dev/null`, `/dev/zero`,
  `/dev/urandom`, `/dev/random`, `/dev/console`, and `/dev/tty`. These are
  ordinary descriptors on the normal read/write path; `seek` reports `ESPIPE`
  and the entries cannot be unlinked or renamed.
- **`procfs`** (`kernel/fs/procfs`) generates `/proc/version`, `/proc/uptime`,
  and `/proc/meminfo` when the file is opened, so a `cat` sees current values.
  It is read-only: writes report `EACCES`, and the entries cannot be removed.

The archive is cpio "newc" (`070701`), built by `tools/mkcpio.py` from the
same ELF images the FAT volume carries. It holds `/init`, the programs the
exec tests use, and empty `dev`, `proc`, and `disk` directories so the mount
points appear in a listing. `INITRAMFS-UNPACK-OK entries=… files=… bytes=…`
reports what was unpacked, and the ring-3 self-test proves the rest from user
mode (`USER-INITRAMFS-OK`): the root listing, a name longer than 8.3, a file
two directories deep, a symlink that `lstat` reports as a link while `stat`
and `open` follow it, `/dev/null` returning end of file, `/dev/zero`
returning zeros, `/proc/version` naming the kernel, and `/disk/HAKU.ELF`
starting with the ELF magic.

`lstat` is a real no-follow call: the backend's optional `lstat` operation
describes the last component itself (ramfs walks with `follow_last = 0`),
`AT_SYMLINK_NOFOLLOW` is honored by `fstatat64` and `statx`, and a backend
without links answers exactly like `stat`. `symlink` and `readlink` are
available to user mode; `readlink` truncates to the caller's buffer instead
of failing, like Linux.

### Running a real userland: busybox and musl

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
  process-group calls report the caller's pid, so the shell starts without
  `can't access tty; job control turned off` (the scripted check runs
  `sh -i -c` to assert exactly that setup). Stop signals still end a process.

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

### Locking

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

### Sector cache

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

### ELF32 program loading

`kernel/arch/x86/elf/elf.c` loads static ELF32 little-endian i386 `ET_EXEC` images read
from FAT12. It validates the ELF header and program-header bounds, accepts
non-overlapping readable `PT_LOAD` segments, checks address arithmetic and
file ranges, and rejects dynamic/interpreted images and writable-executable
segments. Each segment is mapped according to its ELF permissions; the loader
zeroes `p_memsz - p_filesz` for BSS and creates a two-page user stack.

Each loaded image receives a fresh page directory containing supervisor-only
kernel identity/direct mappings plus only that image's user pages. The scheduler
activates each process's CR3 on dispatch and returns to the kernel page
directory for its idle context. Exited process mappings and page tables are
released after switching away from their kernel stacks. Dynamic linking,
relocations, shared libraries, and ELF files larger than the current 256 KiB
loader buffer are not supported.

### Why `link.exe` + `tools/pe2bin.py`?

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

## Project layout

```
Haku/
  kernel/
    arch/x86/
      boot/             stage1.asm + stage2.asm: 16-bit boot sector and CHS
                        kernel loader (up to 448 KiB) → A20 → GDT → protected mode
      kernel/           entry.asm + isr.asm: 32-bit entry, interrupt stubs,
                        GDT/TSS load, ring-3 transition + syscalls
                        cpu/gdt/idt/pic/pit/kbd: port I/O, descriptor tables,
                        PIC/PIT, and the PS/2 keyboard driver
      mm/               paging.c/.h: i386 page tables (identity + sized direct map + user)
                        frame.c/.h: E820-backed physical page inventory
      elf/              elf.c/.h: validated ELF32 PT_LOAD loader + user spaces
      user/             user.asm, child.asm, fault.asm, exec_target.asm,
                        vfork_target.asm: ELF32 test programs (syscall,
                        spawn/waitpid, protection faults, execve/vfork)
    arch/x86_64/
      boot/             entry64.asm: PAE + EFER.LME + PML4/PDPT/PD, far jump
                        into a 64-bit code segment, then call into C
      kernel/           boot64.c: bring-up sequence (E820 → physmap → frames
                        → heap → GDT/TSS → IDT/PIC → timer → self-tests);
                        haku64.h: shared types, layout, asm declarations;
                        serial64.c: COM1 output + memset/memcpy;
                        bootmem64.c: bump allocator for early tables;
                        gdt64.c: GDT, TSS and the IST stacks;
                        idt64.c: 256 gates + exception/IRQ dispatch;
                        isr64.asm: stubs, CLI/STI/LIDT/LGDT/LTR, port I/O,
                        CR2, exception probes; pic64.c: 8259 remap, masks,
                        EOI; timer64.c: 100 Hz PIT tick counter
      mm/               paging64.c: four-level paging + physmap;
                        frame64.c: E820 frame allocator (no 1 GiB ceiling);
                        heap64.c: 8 MiB free-list kernel heap
    drivers/            ata.c/.h: primary/secondary ATA (master/slave) LBA28/LBA48
                        polling PIO with reset/retry recovery and cache flush
                        terminal.c/.h: text-I/O layer (output + input ring)
                        display.c/.h: shared display backend API
                        vga_text.c/.h: VGA text backend (fallback)
                        framebuffer.c/.h: VBE framebuffer text backend
                        serial.c/.h: COM1 output backend
    fs/
      fat12/            fat12.c/.h: writable FAT12 files, subdirectories, paths
                        fat12_vfs.c/.h: FAT12 vfs_fs_ops backend + errno mapping
      ramfs/            ramfs.c/.h: in-memory root (long names, modes, links)
      devfs/            devfs.c/.h: /dev/null, zero, urandom, console, tty
      procfs/           procfs.c/.h: generated /proc/version, uptime, meminfo
      initramfs/        cpio.c/.h: cpio "newc" unpacker into the VFS
      vfs/              vfs.c/.h: fs-independent descriptors, mount table
      test/             fs_test.c/.h: kernel-side filesystem self-tests
    kernel/             kernel_main.c: kernel startup
                        scheduler.c/.h: scheduler + per-process usage counters
                        sync.c/.h: mutex/spinlock/IRQ-save locks + rules
                        usermode.c/.h: user mappings, syscall ABI, brk/mmap/munmap
                        getusage
                        klog.c/.h: timestamped, leveled terminal logging
                        panic.c/.h: fatal handler + halted state message
                        crash.c/.h: register/MMU/stack crash reports
                        heap.c/.h: kernel kmalloc/kfree allocator
  user/
    lib/              errno.c, fsabi.c, memory.c, mman.c, signal.c, stdio.c,
                      string.c, unistd.c, crt0.asm, syscall.asm: user C library
    tests/            libtest.c: C library self-test installed as LIB.ELF
                      musl_probe.c: portable probe built by the CI musl job
  include/haku/         abi.h + abi.inc: shared user/kernel ABI constants
                        types/errno/string/stdlib/unistd/fcntl/stat/stdio/
                        signal/mman/dirent/poll/termios/resource.h: user
                        library headers, plus haku.h
  tools/pe2bin.py      PE image → flat binary (+ reports ENTRY= for the bootloader)
  tools/mkimg.py       boot floppy size/signature checks
  tools/mkfat12.py     FAT12 image builder for the user ELF
  tools/mkcpio.py      cpio "newc" initramfs builder (INITRD on the FAT volume)
  tools/pe2elf.py      fixed-base PE32 to ELF32 (RX/R/RW) for user C
  .github/workflows/  ci.yml: hosted headless build/test + artifacts
                      release.yml: tag-driven bootable image release
  build.py             the whole build: cl/link/nasm/qemu orchestration
  build/               generated files (stage*.bin, *.obj/pe, haku.img, ...)
  haku.img             bootable floppy image
  build/haku_fs.img    1.44 MB FAT12 ATA disk image with HAKU/NEXT/LIB ELFs
                      and the INITRD initramfs archive
  build/ata_spare.img  1.44 MB blank ATA disk on the secondary channel (scan test)
```

## Troubleshooting

- **`not a 32-bit image`** — you used the x64 `cl.exe`; build.py picks
  `…\bin\Hostx64\x86\cl.exe` automatically, don't override it.
- **unresolved external (e.g. `_memset`)** — a CRT helper leaked in; add it
  to `kernel.c` (or simplify the code that triggered it).
- **NASM win32 drops all `global` symbols without `section .text`** — code
  assembles fine but the .obj has no definitions (found by dumping symbols).
  Every NASM-for-link file needs an explicit section directive.
- **MSVC prepends `_`, NASM doesn't** — write bare names in C (`isr0`,
  `idt_load`) to meet NASM's `_isr0`, `_idt_load`. Spelling `_isr0` in C
  asks the linker for `__isr0`.
- **QEMU triple-faults / reboots** — run `python build.py headless` and read
  `build/serial.log`; no `HAKU-BOOT-OK` means the kernel never started
  (check disk load / GDT / entry address). A `KERNEL PANIC:` line is followed
  by the crash report and `build/qemu.log` has the exception trace.
- **NASM not found** — install it, then open a fresh terminal (PATH refresh).
- **kernel exceeds 448 KiB** — the stage-2 loader deliberately stops below
  the bootstrap stack. Move to a memory-backed or later-stage loading design
  before increasing this limit.

## Project documentation

- [Roadmap](ROADMAP.md) — current status, milestones, and acceptance criteria.
- [Contributing](CONTRIBUTING.md) — development setup and pull request guidance.
- [Security policy](SECURITY.md) — private vulnerability reporting guidance.
- [Support](SUPPORT.md) — where to ask questions and report reproducible issues.
- [Code of Conduct](CODE_OF_CONDUCT.md) — expectations for project spaces.
- [Agent instructions](AGENTS.md) — repository-wide coding and validation rules.
