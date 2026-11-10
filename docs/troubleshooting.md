# Troubleshooting

[Back to documentation index](README.md)

The build and boot failures that come up most often, and what causes them.

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
- **`KERNEL PANIC: Invalid opcode` right after `INT3-RESUMED`** — an object was
  built for a CPU with vector registers while `CR4.OSFXSR` is off. The build
  passes `/arch:IA32` (`cl.exe`) or `-mno-sse -mno-sse2 -mno-mmx`
  (`clang-cl`) for this reason; the crash report's `code eip=` line shows the
  faulting bytes (`0F 0B` is a compiler-generated `ud2`, an SSE prefix such as
  `66 0F` is vector code).
- **`ELF-LOAD-FAIL: image is not a 32-bit x86 executable`** — the binary was
  built for the host, not for Haku. `musl-gcc` on an x86-64 machine targets
  x86-64 unless a cross toolchain is used, so CI builds the musl probe with an
  `i486-linux-musl` compiler and asserts `ELF32`/`Intel 80386` before booting
  it. Other reasons in that line name the exact check that refused the image.
- **NASM not found** — install it, then open a fresh terminal (PATH refresh).
- **kernel exceeds 448 KiB** — the stage-2 loader deliberately stops below
  the bootstrap stack. Move to a memory-backed or later-stage loading design
  before increasing this limit.
