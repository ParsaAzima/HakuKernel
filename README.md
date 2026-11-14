<h1 align="center">
 Haku v0.2.0 — a tiny x86-64 operating system in C
</h1>

<p align="center">
  <img src="https://img.shields.io/badge/Language-C-A8B9CC?style=for-the-badge&logo=c&logoColor=white" alt="C" />
  <img src="https://img.shields.io/badge/Assembly-NASM-333333?style=for-the-badge" alt="NASM" />
  <img src="https://img.shields.io/badge/Architecture-x86--64%20%2F%20i386-0071C5?style=for-the-badge" alt="x86-64 and i386" />
  <img src="https://img.shields.io/badge/Kernel-Bare%20Metal-222222?style=for-the-badge" alt="Bare Metal" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Compiler-MSVC%20%2F%20Clang--CL-5C2D91?style=for-the-badge&logo=visualstudio&logoColor=white" alt="MSVC and Clang-CL" />
  <img src="https://img.shields.io/badge/Linker-MS%20LINK%20%2F%20LLD-68217A?style=for-the-badge" alt="Linker" />
  <img src="https://img.shields.io/badge/Build-Python-3776AB?style=for-the-badge&logo=python&logoColor=white" alt="Python" />
  <img src="https://img.shields.io/badge/Emulator-QEMU-FF6600?style=for-the-badge&logo=qemu&logoColor=white" alt="QEMU" />
</p>

<p align="center">
  <a href="https://github.com/ParsaAzima/HakuKernel/actions/workflows/ci.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/ParsaAzima/HakuKernel/ci.yml?branch=main&style=for-the-badge&label=CI" alt="CI status" />
  </a>
  <img src="https://img.shields.io/badge/Filesystem-FAT12-4CAF50?style=for-the-badge" alt="FAT12" />
  <img src="https://img.shields.io/badge/ABI-x86--64%20%2F%20i386%20Syscalls-455A64?style=for-the-badge" alt="x86-64 and i386 syscall ABI" />
</p>

A NASM bootloader loads a C kernel, switches the CPU to long mode and runs
ELF64 programs at ring 3 behind the Linux x86-64 syscall ABI. The same source
tree still builds the original 32-bit kernel (`python build.py --arch=i386`),
which is where the static musl/busybox userland, the user C library and the
shell live. There is no C runtime inside the kernel and no hosted library
under it: MSVC (or clang-cl) and NASM produce freestanding objects that
`link.exe`/`lld-link` combine into a flat boot image, and QEMU boots it.

The practical target is a small, honest POSIX-*inspired* subset that a real
static Linux binary can use. The strongest evidence for that is the CI job
that boots static musl programs, and the `python build.py shell` target, which
brings up a 1 MiB static **busybox** as PID 1 and lands in `ash` with job
control and pipelines working.

## Status

- **Boot** — BIOS floppy image, two-stage 16-bit loader (Linux-style A20, GDT,
  protected mode), E820 map, 448 KiB kernel window.
- **Memory** — paging with a supervisor direct map, E820-sized frame allocator
  and bitmaps, 4 KiB pages, `kmalloc`/`kfree` heap, copy-on-write `fork`, W^X
  enforced by the loader on i386 without PAE.
- **Processes** — 100 Hz preemptive scheduler, per-process address spaces and
  8 KiB kernel stacks, `spawn`/`fork`/`vfork`/`execve`/`wait4`, real process
  groups and sessions, `kill(-pgid)`, foreground group on the console, signals,
  and `clone`-created threads that share one address space and descriptor table.
- **SMP** — ACPI MADT discovery, a local-APIC driver and INIT-SIPI trampolines
  that bring the application processors online on both kernels, with atomic
  spinlocks and a private GDT/TSS per processor. On x86-64 every CPU also
  schedules: a GS-based per-CPU block, a local-APIC timer per CPU, per-CPU idle
  tasks, reschedule IPIs and TLB shootdown, so tasks run in parallel. On i386
  the APs park with interrupts off and the bootstrap CPU keeps scheduling.
- **Syscalls** — the i386 numbers for a working file/process/memory/time/signal
  subset, validated user pointers, `-errno` results, and the 64-byte i386
  `struct stat` family that glibc and musl expect.
- **Modules** — `haku module`: a packed `.hkm` container (built on the host by
  `tools/mkhakumod.py` from NASM ELF32 or COFF i386/amd64 objects) loaded with
  `init_module`, relocated against an eleven-entry kernel export table, and
  unloaded with `delete_module`. A module can register a `/dev` character
  device or a filesystem backend and mount it at its own prefix; the kernel
  ties both to the module slot, refuses the unload while a descriptor is open
  (`-EBUSY`), and detaches everything before the image is freed. C modules are
  first-class: `include/haku/module.h` declares the services and `libhakumod`
  supplies the string and 64-bit helpers the compiler calls, with the packer
  dropping whatever a module does not reach. Modules are trusted ring-0 code.
  Both kernels load versioned containers; the x86-64 loader provides logging,
  ticks and the kernel heap, while the device/filesystem registration calls
  answer `-ENOSYS` until a 64-bit VFS exists.
- **Storage** — ATA PIO on both legacy channels with reset/retry recovery, a
  writable FAT12 driver, a VFS with per-process descriptors, `ramfs` root,
  `initramfs` (cpio newc), `/dev`, and a generated `/proc`.
- **Console** — one framebuffer renderer shared by both ports, drawing the
  Terminus 8x16 font as plain full-screen text over a VBE mode with a VGA text
  fallback, PS/2 keyboard, serial mirror, CSI sequence handling with DSR
  replies, kernel-log display latch.
- **Userland** — freestanding C library (`user/lib`), busybox/musl as PID 1,
  headless self-tests for every subsystem listed above.
- **x86-64** — the primary target: long-mode entry, four-level paging with a
  physmap, frame allocator, heap, GDT/TSS with IST stacks, IDT/PIC, timer, RTC
  wall clock, round-robin scheduler, ring 3 with `SYSCALL`/`SYSRET` and the
  Linux syscall convention, NX enforcement, an ELF64 loader, a FAT12 disk with
  file writes behind a page cache that file-backed `mmap` maps from,
  `fork`/`wait4` with copy-on-write pages and default-action signals,
  `dup`/`dup2` over a shared file
  description, `nanosleep`, `execve` of an ELF64 file from the disk with
  `argc`/`argv`/`envp` on the new stack, threads: `clone` with
  `CLONE_VM`/`CLONE_FILES`/`CLONE_THREAD`, `gettid`, and `futex` wait/wake,
  the ported i386 syscall table on the x86-64 ABI (`stat`/`statx`,
  `getdents64`, `rename`/`unlink`/`truncate`, `chdir`/`getcwd`/`openat`,
  `pread`/`pwrite`, `mprotect`/`brk`/`madvise`, `poll`/`select`, `fcntl`/
  `ioctl`/`dup3`, `uname`/`times`/`getrusage`/`getrlimit` and the identity,
  `prctl` and `getrandom` calls), a shared VBE framebuffer console drawing the
  Terminus font as plain text, a PS/2 keyboard whose queue a blocking
  console `read()` drains, a module loader for version 2
  containers, and a 64-bit userland built on `user/lib64`: a freestanding C
  library (string/memory, `printf`, `malloc`, files, `fork`, `mmap`, `errno`)
  whose self-test the kernel loads from the FAT12 volume as `/INIT.ELF`.
  No VFS/devfs, console device, shell, pipes or signal handlers yet.

## Quick start

Requirements: Visual Studio 2022 with **Desktop development with C++** (both
the x86 and x64 `cl.exe`), NASM, QEMU (i386 and x86-64), and Python 3. CI uses
clang-cl/lld-link on Ubuntu with the same `build.py`.

```cmd
python build.py                 :: build haku64.img only (x86-64)
python build.py run             :: build + boot in a QEMU window
python build.py headless        :: build + boot headless, verify
python build.py shell           :: boot a busybox ash shell (32-bit image)
python build.py shell --check   :: scripted shell run, assert its markers
python build.py probe --init=X  :: boot an external ELF32 as /init (32-bit)
python build.py headless --arch=i386   :: the full 32-bit headless suite
```

`headless` prints `X64 CHECK PASSED` after the serial log shows the boot,
storage, process, syscall, memory and module self-test markers.
`headless --arch=i386` prints `HEADLESS CHECK PASSED` for the 32-bit suite.
`shell --check` prints `SHELL CHECK PASSED` after a forked foreground pipeline
and a PATH lookup run inside `sh -i -c`. All exit nonzero on a missing marker,
an unexpected `*-FAIL`, or `KERNEL PANIC`.

`build/haku_fs.img` and `build/haku64_fs.img` are the ATA data disks. The
32-bit image is user data: it is reused untouched unless you pass `--fresh`,
and `python build.py inspect --check` verifies its BPB, FAT mirrors, and
cluster chains without modifying it. Report a damaged image rather than
deleting it out of hand. The x64 seed disk is rebuilt on every run.

## Documentation

| Document | Covers |
|---|---|
| [docs/](docs/README.md) | Documentation index. |
| [Building and running](docs/building.md) | Prerequisites, every build target, image lifecycle, CI, releases, manual runs. |
| [Architecture](docs/architecture.md) | Boot chain, memory map, IDT, frame allocator, the x86-64 port, and modules. |
| [Processes and scheduling](docs/processes.md) | Process creation, COW forks, scheduler, job control, accounting. |
| [System calls](docs/syscalls.md) | The syscall ABI, the call table, mmap/brk/mprotect, POSIX subset. |
| [Userland](docs/userland.md) | User C library, busybox/musl, the shell image. |
| [x86-64 C library](docs/libc64.md) | `user/lib64`, the Microsoft x64 ABI, the `/INIT.ELF` self-test and its limits. |
| [Terminal and display](docs/terminal.md) | Console backends, input, logging latch, panic/crash reports. |
| [Storage](docs/storage.md) | ATA PIO, FAT12, VFS, boot namespace, design limits. |
| [Kernel internals](docs/kernel-internals.md) | Locking, sector cache, ELF loading, the PE-to-binary build. |
| [Project layout](docs/project-layout.md) | Source tree and generated artifacts. |
| [Troubleshooting](docs/troubleshooting.md) | Common build and boot failures. |
| [Roadmap](ROADMAP.md) | Open milestones, acceptance markers, and the release train. |

## Project layout

```
kernel/arch/x86_64/  long-mode entry and kernel (primary target)
kernel/arch/x86/     boot stages, entry/ISR, GDT/IDT/PIC/PIT, paging, frames, ELF
kernel/drivers/      ATA, terminal, display backends, serial, keyboard
kernel/fs/           VFS, FAT12, ramfs, devfs, procfs, initramfs, fs self-tests
kernel/kernel/       startup, scheduler, syscalls, locking, klog, panic, heap
kernel/module/       .hkm container, 32-bit loader, libhakumod, demo modules
user/lib/            freestanding user C library and CRT (32-bit ABI)
user/lib64/          x86-64 user C library and CRT (Microsoft x64 ABI)
user/tests/          C library self-tests (LIB.ELF, INIT.ELF) and the musl probe
include/haku/        shared ABI (abi.h/abi.inc, abi64.h/abi64.inc), user headers
tools/               pe2bin, pe2elf, pe2elf64, mkimg, mkfat12, mkcpio, mkhakumod
build.py             the whole build, test, and QEMU orchestration
```

## Limitations

Haku does not claim POSIX compatibility. It implements a subset aimed at
statically linked Linux binaries, without demand paging, swap, or networking.
Both images have `clone`/`futex` threads and bring the application processors
online with their own TSS and the shared IDT. On x86-64 the processors also
schedule: each CPU has its own current task, idle task and timer, and wakes
other CPUs with an IPI. Tasks are pinned to the CPU that created them, there
is no load balancing or migration, and the user-space concurrency tests still
run on the bootstrap processor, so a ring-3 task running on an AP is not yet
covered. On i386 the application processors stay parked and every task runs on
the bootstrap processor. The x86-64 kernel also lacks signal handlers, pipes, a
VFS, devfs, a console device, a line discipline and the kernel-log latch, so
its userland is a C library (`user/lib64`) plus the `/INIT.ELF` self-test
rather than a shell; its console and PS/2 keyboard are there so a program can
read a line's worth of keys, and module capability registration answers
`ENOSYS` there. FAT12
has 8.3 names, no timestamps, and no
permission bits. Haku's filesystem cache holds eight paths of at most 128 KiB
each. Everything beyond the implemented subset returns `ENOSYS`
rather than pretending to work.

## Policies

- [Contributing](CONTRIBUTING.md) — development setup and pull request guidance.
- [Security policy](SECURITY.md) — private vulnerability reporting guidance.
- [Support](SUPPORT.md) — where to ask questions and report reproducible issues.
- [Code of Conduct](CODE_OF_CONDUCT.md) — expectations for project spaces.
- [Agent instructions](AGENTS.md) — repository-wide coding and validation rules.

License: see [LICENSE](LICENSE). The Terminus font embedded in
`kernel/drivers/font_terminus.c` keeps its own SIL Open Font License 1.1; see
[LICENSES/Terminus-OFL.txt](LICENSES/Terminus-OFL.txt).
