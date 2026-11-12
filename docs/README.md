# Haku documentation

[Back to the project README](../README.md)

Haku is a freestanding 32-bit x86 operating system: a NASM bootloader, a C
kernel built with MSVC or clang-cl, and a static musl/busybox userland running
at ring 3. These pages describe how it is built and how it works in detail.

## Topics

| Document | Covers |
|---|---|
| [Building and running](building.md) | Windows prerequisites, every `build.py` target, the data image lifecycle, CI, releases, and manual test runs. |
| [Architecture](architecture.md) | Boot chain, memory map, the x86-64 work in progress, the IDT, SMP bring-up, Haku modules, and the physical frame allocator. |
| [Processes and scheduling](processes.md) | `spawn`/`fork`/`vfork`/`execve`, copy-on-write address spaces, the 100 Hz scheduler, job control, and resource accounting. |
| [System calls and the POSIX subset](syscalls.md) | The i386 syscall ABI, the syscall table, memory-management calls, the implemented POSIX surface, and the self-tests that cover them. |
| [Userland](userland.md) | The freestanding user C library, busybox/musl on top of the ABI, and the shell image. |
| [Terminal, display, and input](terminal.md) | Console backends, keyboard input, the kernel log display latch, and panic/crash reporting. |
| [Storage and filesystems](storage.md) | ATA PIO, FAT12, the VFS layer, the boot namespace, and why the filesystem is deliberately small. |
| [Kernel internals](kernel-internals.md) | Locking primitives, the sector cache, ELF32 program loading, Haku module packing/loading, and the `link.exe` + `tools/pe2bin.py` build strategy. |
| [Project layout](project-layout.md) | Where sources, build outputs, and generated artifacts live. |
| [Troubleshooting](troubleshooting.md) | The build and boot failures that come up most often, and what causes them. |

## Project pages

- [README](../README.md) — what Haku is and how to start it.
- [Roadmap](../ROADMAP.md) — current status, milestones, and acceptance criteria.
- [Contributing](../CONTRIBUTING.md) — development setup and pull request guidance.
- [Security policy](../SECURITY.md) — private vulnerability reporting guidance.
- [Support](../SUPPORT.md) — where to ask questions and report reproducible issues.
- [Code of Conduct](../CODE_OF_CONDUCT.md) — expectations for project spaces.
- [Agent instructions](../AGENTS.md) — repository-wide coding and validation rules.
