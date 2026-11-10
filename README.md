<h1 align="center">
 Haku v0.2.0 — a tiny x86 operating system in C
</h1>

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
  <img src="https://img.shields.io/badge/Emulator-QEMU-FF6600?style=for-the-badge&logo=qemu&logoColor=white" alt="QEMU" />
</p>

<p align="center">
  <a href="https://github.com/ParsaAzima/HakuKernel/actions/workflows/ci.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/ParsaAzima/HakuKernel/ci.yml?branch=main&style=for-the-badge&label=CI" alt="CI status" />
  </a>
  <img src="https://img.shields.io/badge/Filesystem-FAT12-4CAF50?style=for-the-badge" alt="FAT12" />
  <img src="https://img.shields.io/badge/ABI-i386%20Syscalls-455A64?style=for-the-badge" alt="i386 syscall ABI" />
</p>

A NASM bootloader loads a C kernel and switches the CPU to 32-bit protected
mode. The kernel sets up paging, interrupts, and a GDT/TSS, then runs ELF32
programs from a FAT12 volume on an ATA disk at ring 3 behind an i386-style
syscall gate. There is no C runtime inside the kernel and no hosted library
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
  groups and sessions, `kill(-pgid)`, foreground group on the console, signals.
- **Syscalls** — the i386 numbers for a working file/process/memory/time/signal
  subset, validated user pointers, `-errno` results, and the 64-byte i386
  `struct stat` family that glibc and musl expect.
- **Storage** — ATA PIO on both legacy channels with reset/retry recovery, a
  writable FAT12 driver, a VFS with per-process descriptors, `ramfs` root,
  `initramfs` (cpio newc), `/dev`, and a generated `/proc`.
- **Console** — VGA text and VBE framebuffer backends, PS/2 keyboard, serial
  mirror, CSI sequence handling with DSR replies, kernel-log display latch.
- **Userland** — freestanding C library (`user/lib`), busybox/musl as PID 1,
  headless self-tests for every subsystem listed above.
- **x86-64** — in progress: long-mode entry, four-level paging with a physmap,
  frame allocator, heap, GDT/TSS with IST stacks, IDT/PIC, timer, RTC wall
  clock, round-robin scheduler, ring 3 with `SYSCALL`/`SYSRET` and the Linux
  syscall convention, NX enforcement, an ELF64 loader, and a read-only
  in-memory file store that file-backed `mmap` maps from. No fork, signals,
  threads, disk filesystem or 64-bit libc yet.

## Quick start

Requirements: Visual Studio 2022 with **Desktop development with C++** (for the
x86 `cl.exe`), NASM, QEMU (i386), and Python 3. CI uses clang-cl/lld-link on
Ubuntu with the same `build.py`.

```cmd
python build.py                 :: build haku.img only (data image preserved)
python build.py run             :: build + boot in a QEMU window
python build.py headless        :: fresh data image + boot headless, verify
python build.py shell           :: boot a busybox ash shell (interactive)
python build.py shell --check   :: scripted shell run, assert its markers
python build.py probe --init=X  :: boot an external ELF as /init, check markers
```

`headless` prints `HEADLESS CHECK PASSED` after the serial log shows the boot,
storage, process, syscall, signal, and memory self-test markers. `shell
--check` prints `SHELL CHECK PASSED` after a forked foreground pipeline and a
PATH lookup run inside `sh -i -c`. Both exit nonzero on a missing marker, an
unexpected `*-FAIL`, or `KERNEL PANIC`.

`build/haku_fs.img` is the ATA data disk and is user data: it is reused
untouched unless you pass `--fresh`, and `python build.py inspect --check`
verifies its BPB, FAT mirrors, and cluster chains without modifying it. Report
a damaged image rather than deleting it out of hand.

## Documentation

| Document | Covers |
|---|---|
| [docs/](docs/README.md) | Documentation index. |
| [Building and running](docs/building.md) | Prerequisites, every build target, image lifecycle, CI, releases, manual runs. |
| [Architecture](docs/architecture.md) | Boot chain, memory map, IDT, frame allocator, x86-64 progress. |
| [Processes and scheduling](docs/processes.md) | Process creation, COW forks, scheduler, job control, accounting. |
| [System calls](docs/syscalls.md) | The syscall ABI, the call table, mmap/brk/mprotect, POSIX subset. |
| [Userland](docs/userland.md) | User C library, busybox/musl, the shell image. |
| [Terminal and display](docs/terminal.md) | Console backends, input, logging latch, panic/crash reports. |
| [Storage](docs/storage.md) | ATA PIO, FAT12, VFS, boot namespace, design limits. |
| [Kernel internals](docs/kernel-internals.md) | Locking, sector cache, ELF loading, the PE-to-binary build. |
| [Project layout](docs/project-layout.md) | Source tree and generated artifacts. |
| [Troubleshooting](docs/troubleshooting.md) | Common build and boot failures. |
| [Roadmap](ROADMAP.md) | Current status, milestones, and acceptance criteria. |

## Project layout

```
kernel/arch/x86/     boot stages, entry/ISR, GDT/IDT/PIC/PIT, paging, frames, ELF
kernel/arch/x86_64/  long-mode entry and bring-up (work in progress)
kernel/drivers/      ATA, terminal, display backends, serial, keyboard
kernel/fs/           VFS, FAT12, ramfs, devfs, procfs, initramfs, fs self-tests
kernel/kernel/       startup, scheduler, syscalls, locking, klog, panic, heap
user/lib/            freestanding user C library and CRT
user/tests/          C library self-test and the portable musl probe
include/haku/        shared ABI (abi.h/abi.inc) and user headers
tools/               pe2bin, pe2elf, mkimg, mkfat12, mkcpio
build.py             the whole build, test, and QEMU orchestration
```

## Limitations

Haku does not claim POSIX compatibility. It implements a subset aimed at
statically linked Linux binaries, on a single CPU, without threads, demand
paging, swap, networking, or a page cache. FAT12 has 8.3 names, no timestamps,
and no permission bits. The x86-64 image is a bring-up milestone, not a usable
system. Everything beyond the implemented subset returns `ENOSYS` rather than
pretending to work.

## Policies

- [Contributing](CONTRIBUTING.md) — development setup and pull request guidance.
- [Security policy](SECURITY.md) — private vulnerability reporting guidance.
- [Support](SUPPORT.md) — where to ask questions and report reproducible issues.
- [Code of Conduct](CODE_OF_CONDUCT.md) — expectations for project spaces.
- [Agent instructions](AGENTS.md) — repository-wide coding and validation rules.

License: see [LICENSE](LICENSE).
