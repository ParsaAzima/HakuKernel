# Architecture

[Back to documentation index](README.md)

Boot chain, memory map, the long-mode work in progress, the IDT, and the
physical frame allocator.

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
| spaces | two PML4s map one address to different frames; CR3 switches them | `HAKU64-VM-OK` |
| tasks | two 16 KiB kernel stacks preempted by IRQ0 | `HAKU64-PROC-OK` |
| ring 3 | an `IRETQ` frame with `CS=0x1B`/`SS=0x23` enters user mode | `HAKU64-USER-OK` |
| syscall | the DPL-3 `INT 0x80` gate runs `write` and `exit` | `HAKU64-SYSCALL-OK` |
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
port I/O, `CR2`/`CR3`, `PAUSE`, `FXSAVE`/`FXRSTOR` and the CR4/CR0 bits that
enable SSE all live in `isr64.asm` as small callable functions. The same file
holds three exception probes (`int3_probe64`, `probe_load64`, `probe_div64`).
Recovering from a fault means skipping the faulting instruction, so instead of
decoding it each probe exports a label for the instruction *after* the one
that faults and the handler sets `RIP` to it — no hardcoded instruction
lengths, and the test fails if the handler's frame is wrong.

`gdt64_load` replaces the boot GDT with `LGDT` and reloads only the data
segments. CS is deliberately not reloaded: the new table keeps the boot code
descriptor byte-identical at selector `0x08`, so the cached descriptor stays
valid. Index 1 must therefore remain the kernel code descriptor.

Processes reuse that frame rather than adding a second switch mechanism.
`kernel/arch/x86_64/kernel/task64.c` saves the `regs64_t` pointer and, when
the round-robin scan picks another task, returns a frame that lives on the
incoming task's kernel stack; the stub moves `RSP` to it, pops and `IRETQ`s.
New tasks start from a frame assembled on top of a private 16 KiB kernel
stack, placed so the entry point sees the same stack alignment a `CALL`
would produce. IRQ0 drives the switch, so nothing has to yield. Because the
x86-64 ABI puts SSE2 in the baseline, `sched64_init` sets `CR4.OSFXSR` and
`CR4.OSXMMEXCPT` and every switch `FXSAVE`s the outgoing task and `FXRSTOR`s
the incoming one; unlike the 32-bit build, this path cannot pretend the
vector registers are unused.

An address space is a PML4 that copies the two kernel slots — the 1 GiB
identity map and the physmap — and owns PML4 slot 1, starting at
`0x0000008000000000` in the lower canonical half. `paging64_create_space`
takes the root from the frame allocator, and `paging64_map_space` allocates
the three intermediate levels under a caller-supplied root, so the kernel's
own tables are never edited by a user mapping. `paging64_switch_space` writes
`CR3`; the self-test maps one virtual address to two frames, writes through
both and checks the physmap and identity map still resolve. `RSP0` is
repointed at the incoming task's kernel stack on every switch, because that
is what the CPU loads when a ring-3 trap enters the kernel.

Ring-3 entry is an ordinary `IRETQ` from a frame whose `CS` is `0x1B` and
whose `SS` is `0x23`; the same instruction that returns from an interrupt
performs the privilege change. The system call gate is vector `0x80`, the one
DPL-3 IDT entry. `sys64_dispatch` takes the number in `RAX`, arguments in
`RDI`/`RSI`/`RDX` and returns in `RAX`, validates that the buffer pointer is
inside the user half before printing it, and implements `write` and `exit`
(`K64_SYS_WRITE`/`K64_SYS_EXIT`). `INT 0x80` is slower than
`SYSCALL`/`SYSRET` but needs no `STAR`/`LSTAR`/`FMASK` setup, no `SWAPGS`
and no second entry stub; the roadmap keeps the Linux-compatible syscall
table for a later stage.

The 64-bit path still has no ELF64 loader or userland, and `haku.img` remains
the default image; [the roadmap](../ROADMAP.md) (Milestone 8) lists the
remaining stages. The 64-bit sources are excluded from the 32-bit link line,
so both targets build from one tree.

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
