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
- **x64: a ring-3 program jumps to address 0 right after `fork`** — a
  copy-on-write break installed a leaf that names the wrong frame. The new
  physical address has to be cleared out of the reused bits before the copy is
  OR-ed in (`paging64_break_cow`): OR-ing a frame into a leaf that still holds
  the old one produces a third, unrelated address, and the process then reads
  some other frame's contents — typically a zeroed return address, which shows
  up as `HAKU64-USER-FAULT: vector=14 error=0x15 rip=0x0 cr2=0x0`. `error=0x15`
  there is the NX bit plus a present page, because address 0 is inside the
  kernel's identity-map slot that every space borrows; QEMU's `-d int` log
  shows the last `ADDQ` operand and the faulting `CS:RIP`.
- **x64: `#GP` (`vector=13 error=0x0`) at boot, right after the first interrupt
  is enabled** — GS.base is no longer the per-CPU block. In ring 0 the kernel
  reads "this CPU" as `gs:[0]`, so anything that reloads GS with a plain
  `mov gs, ax` resets the hidden base to zero and the next `k64_cpu()` returns
  whatever the IVT holds there (a non-canonical address, hence the `#GP` on
  the first `[gs:0]` load). `gdt64_load` therefore loads DS/ES/SS and leaves
  FS and GS alone; swapping GS must go through `SWAPGS` on the paths that
  cross the ring boundary (see the SWAPGS invariant in
  [architecture](architecture.md#stage-d-per-cpu-scheduling)).
- **i386: `percpu_self()` returns 0, or an interrupt handler reads address 0 as
  its CPU block** — a `mov gs, ax` in a stub, or `gdt_load`'s flat-selector
  reload, reset the hidden base. On 32-bit the per-CPU base is GDT entry 7
  (`CPU_GS`, selector `0x38`), so every path that reloads GS with a plain
  selector must use `0x38` afterwards, and `SAVE_SEGMENTS` must only reload it
  for a frame that came from ring 3 — a ring-0 interrupt keeps the base it
  already had. A kernel task's synthetic frame also stores `CPU_GS`, not
  `KERNEL_DS`, or `RESTORE_SEGMENTS` clears the base on resume.
- **`LNK1164: section alignment greater than section size`** — a static was
  declared `__declspec(align(64))` and the kernel is linked with `/ALIGN:16`
  for the flat image the boot loader expects, so a 64-byte-aligned section is
  rejected. Cache-line-aligned data (the per-CPU blocks) is stored with
  `align(16)` and rounded up once at run time instead
  (`percpu_align` in `percpu64.c`, `percpu_init_bsp` in `percpu.c`).
- **kernel exceeds 448 KiB** — the stage-2 loader deliberately stops below
  the bootstrap stack. Move to a memory-backed or later-stage loading design
  before increasing this limit.
