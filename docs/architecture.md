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
| RTC | CMOS wall clock sampled at boot, Unix epoch for `CLOCK_REALTIME` | `HAKU64-RTC-OK` |
| files | FAT12 on an ATA disk, behind an eight-slot page cache; `open`, `read`, `write`, `lseek`, `close`, `mkdir` | `HAKU64-FS-OK`, `HAKU64-FILE-OK`, `HAKU64-DISK-OK` |
| spaces | two PML4s map one address to different frames; CR3 switches them | `HAKU64-VM-OK` |
| tasks | two 16 KiB kernel stacks preempted by IRQ0 | `HAKU64-PROC-OK` |
| fork | a child gets a copied address space, two observers of one `MAP_SHARED` page | `HAKU64-FORK-OK` |
| ring 3 | an `IRETQ` frame with `CS=0x23`/`SS=0x1B` enters user mode | `HAKU64-USER-OK` |
| syscall | `SYSCALL`/`SYSRET` plus the DPL-3 `INT 0x80` fallback: `read`, `write`, `open`, `close`, `lseek`, `writev`, `mmap`, `munmap`, `mkdir`, `fork`, `wait4`, `kill`, `getpid`, `getppid`, `clock_gettime`, `exit` | `HAKU64-SYSCALL-OK`, `HAKU64-SYSCALL64-OK`, `HAKU64-ABI-OK` |
| NX | a ring-3 fetch from a mapped non-executable page faults and ends the task | `HAKU64-NX-OK` |
| ELF64 | malformed images refused, then an embedded static executable runs | `HAKU64-ELF-REJECT-OK`, `HAKU64-ELF-OK`, `HAKU64-ELF-RUN-OK` |
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
valid. Index 1 must therefore remain the kernel code descriptor, and indices 3
and 4 hold the user *data* then user *code* descriptors — the order `SYSRET`
derives from `STAR[63:48]` (`SS = base + 8`, `CS = base + 16`).

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

`entry64.asm` also sets `EFER.NXE`, and every leaf `paging64_map_space`
installs carries the matching bit: executable segments clear bit 63 while
data, BSS, stack and non-`PROT_EXEC` mappings set it, so code pages cannot be
written and writable pages cannot be fetched. A ring-3 `#PF` used to halt the
machine; `idt64.c` now ends the task instead, because there is no signal
model yet: `sched64_fault` records the vector and `CR2` and switches to the
next runnable task, while a kernel fault stays fatal. The NX self-test jumps
into a writable page and asserts vector 14 with `CR2` at that page. Frame
extraction in the paging code uses `K64_PHYS_MASK` rather than
`K64_PAGE_MASK`, so the NX bit can never leak into a physical address.

Ring-3 entry is an ordinary `IRETQ` from a frame whose `CS` is `0x23` and
whose `SS` is `0x1B`; the same instruction that returns from an interrupt
performs the privilege change. System calls use `SYSCALL`: `syscall64_init`
programs `STAR` (kernel segments from `0x08`, `SYSRET` base `0x10`), `LSTAR`
and `FMASK`, and sets `EFER.SCE`. `syscall64_entry` saves the user `RSP`,
switches to the task's kernel stack (the `TSS.RSP0` mirror
`gdt64_rsp0_value`), builds the same `regs64_t` frame the interrupt stubs
build, and calls `sys64_dispatch`. It returns with `SYSRET` when the
dispatcher hands back a ring-3 frame and through the shared `IRETQ` tail when
the scheduler hands back a kernel frame. `INT 0x80` at vector `0x80` remains
as a DPL-3 fallback, and the test program exercises both entry points.

`sys64_dispatch` implements the x86-64 Linux convention — number in `RAX`,
arguments in `RDI`/`RSI`/`RDX`/`R10`/`R8`/`R9`, result in `RAX` or a negative
errno — with the Linux x86-64 numbers for `read`, `write`, `open`, `close`,
`lseek`, `writev`, `mmap`, `munmap`, `mkdir`, `fork`, `wait4`, `kill`,
`getpid`, `getppid`, `clock_gettime` and `exit`/`exit_group`; an
unimplemented number returns `-ENOSYS`. Every user
pointer is resolved through the current space with `paging64_query_space`
before it is dereferenced, and the copies move through the physmap: a ring-0
page fault halts the kernel, so the CPU cannot be relied on to reject a bad
address.

`mmap` accepts anonymous ranges (`fd` must be `-1`) and file-backed ones.
`MAP_FIXED` places a mapping at a page-aligned address inside the anonymous
window and refuses an occupied range instead of replacing it; without it the
per-task bump region from `K64_USER_MMAP_BASE` supplies the address.
`MAP_SHARED` on a file maps the file's own frames, so a store through the
mapping is visible to `read`; `MAP_PRIVATE` copies the pages at map time,
because copy-on-write is not implemented. Mapping past the end of a file
is `-EINVAL` rather than the `SIGBUS` a Linux process would take, and a
page-aligned `offset` selects where in the file the mapping starts. A
writable shared file mapping needs a descriptor opened for writing and is
`-EACCES` otherwise. Because `munmap` may not free a frame the file still
owns, each task carries a small table of mapping ranges recording whether
their frames are owned and whether another space aliases them; a task's
table is consulted page by page, and `fork` copies it.

Files live on a FAT12 volume attached as the primary-master ATA disk. The
64-bit image compiles the 32-bit `kernel/drivers/ata.c` and
`kernel/fs/fat12/fat12.c` unchanged and supplies the seams they expect
(`kernel/arch/x86_64/compat`): port I/O, the `klog_*` surface over `k64_*`,
and a single-CPU spin mutex. `fs64.c` is a page cache in front of that
filesystem: each path gets a slot holding up to 32 frames (128 KiB) and eight
paths are cached at once, which is what lets `mmap` alias a file's frames and
keeps every descriptor of one path looking at the same bytes. `open` accepts
`O_RDONLY`, `O_WRONLY`, `O_RDWR` plus `O_CREAT`, `O_EXCL`, `O_TRUNC` and
`O_APPEND`; `mkdir` creates one 8.3 directory, and paths may contain `.` and
`..` because FAT12 resolves them. Dirty pages are written back when the last
descriptor on a path closes or the owning task exits, so the cache is
write-back, not write-through: a crash can lose the last pages. The limits
are explicit: only 8.3 names, no `unlink`, `rename` or `getdents`, no
symbolic links, at most 32 cached pages per file, and a writable
`MAP_SHARED` mapping requires a writable descriptor. A shared mapping also
relies on its descriptor staying open, because only descriptors pin a cache
slot. `python build.py x64` builds `build/haku64_fs.img` with
`tools/mkfat12.py --empty` on every run,
seeds `HELLO.TXT` and `PATTERN.BIN`, attaches it to QEMU and checks the
resulting image with `tools/lsfat12.py --check`, so a disk write is proven
from the host's view of the volume and not only from the kernel's cache.

`fork` clones the address space page by page: `paging64_clone_space` walks
the parent's user leaves, aliases the frames a mapping record marks shared,
and copies everything else into fresh frames. There is no copy-on-write yet,
so a private page costs its copy at fork time. The child gets a copy of the
parent's syscall frame on its own 16 KiB kernel stack with `RAX=0`, a copy of
the descriptor table (cache slots and positions are shared) and the mapping
table, with shared ranges marked borrow-only. `wait4` blocks until a child is
`DONE`, writes `(status & 0xFF) << 8` or the terminating signal, and reaps
it; a task's space, frames and kernel stack are released only at that point,
which is also what lets the boot test inspect a task after it stopped.
Signals have no handlers: `kill` and a ring-3 fault perform the default
action, terminate, and `wait4` reports the signal number. `SIGCHLD` is not
delivered, there are no signal masks, and a task cannot signal itself
through `kill` — that path goes through `exit` so the dispatcher never
resumes the frame.

`clock_gettime` reports `CLOCK_MONOTONIC` as time since boot and
`CLOCK_REALTIME` as the CMOS RTC epoch sampled once at boot plus the ticks
elapsed since that sample; when the RTC cannot be read it falls back to
monotonic time. The full i386 number table stays with the later userland
stage.

The 64-bit path now has an ELF64 loader and a FAT12 data disk, but no
64-bit libc and no bootable root filesystem image; `haku.img` remains the
default image and [the roadmap](../ROADMAP.md) (Milestone 8) lists the
remaining stages. The 64-bit sources are excluded from the 32-bit link line,
so both targets build from one tree.

`kernel/arch/x86_64/elf/elf64.c` follows the 32-bit loader's rules — validate
the whole image before mapping, accept only static `ET_EXEC`, require
readable `PT_LOAD` segments that are not both writable and executable, refuse
segments whose offset is not congruent with their address, and name the first
reason a file was rejected. It maps each segment into a fresh
`paging64_create_space` space, zero-filling the pages the file does not cover,
then allocates a four-page stack ending one page below the top of PML4 slot
1; the gap between the stack and the image is unmapped, so a runaway stack
faults instead of overwriting code. Images must load above
`0x0000008000010000`, which keeps the first 64 KiB of the slot unmapped.
There is no `AT_PHDR` or auxiliary vector yet because nothing walks it; the
loader records the page-aligned start of the first writable segment so the
self-test can read it back.

The test executable is `kernel/arch/x86_64/user/ring3_elf64.asm`: a flat
`[BITS 64]` program that writes `ELF64` through `SYSCALL`, proves the
`SYSRET` return by reaching a second write (`HAKU64-SYSCALL64-OK`), exercises
the six-argument syscall ABI (`writev`, `getpid`, `CLOCK_MONOTONIC` and
`CLOCK_REALTIME`, anonymous `mmap`/`munmap`, `MAP_FIXED` placement,
`MAP_SHARED|MAP_ANONYMOUS`, and the `EBADF`/`EFAULT`/`ENOSYS`/`EINVAL` error
paths over `INT 0x80`), reads `/HELLO.TXT` and prints the bytes it read
(`HAKU64-FILE-OK`), maps that file privately and checks a store does not
reach the file, maps it shared and checks the store does, maps
`/PATTERN.BIN` at a page offset and compares it with a `read` of the same
range, creates `/TMP`, writes `/TMP/NOTE.TXT`, reads it back before and after
`close` (`HAKU64-DISK-OK`), resolves `/TMP/../HELLO.TXT` and `/./HELLO.TXT`,
then maps a shared anonymous page, forks, and requires the child to see the
parent's word and the parent to see the child's store after `wait4`
(`HAKU64-FORK-OK`). Finally it prints `HAKU64-ABI-OK`, stores a magic word in
its BSS page and exits with status 0; every failure exits with the number of
the step that failed, which is what `HAKU64-ELF-RUN-FAIL: exit=` reports.
`tools/mkelf64.py` wraps the flat binary in an
`ET_EXEC`/`EM_X86_64` image with an RX `PT_LOAD` at `0x0000008000400000` and a
zero-filesz RW segment one page above it; `user64_image.asm` embeds the result
in `.rdata`, which `tools/pe2bin.py` copies into the kernel image. The boot
test first feeds `elf64_load` six malformed copies (wrong class, wrong
machine, W+X, bad entry, offset past the file, truncated header) and requires
a reason for each, then loads the valid image, checks that the BSS page is
zero, and runs it as a user task. The magic word (`0x48414B55`) can only be
there if ring 3 reached the store, and the boot test also requires exit
status 0 and the mmap bump pointer to have advanced, so `HAKU64-ELF-RUN-OK`
proves execution rather than a successful mapping.

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
