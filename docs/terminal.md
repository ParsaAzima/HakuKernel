# Terminal, display, and input

[Back to documentation index](README.md)

The console backends, keyboard input, the kernel log display latch, and
panic and crash reporting.

`kernel/drivers/terminal.c` is the text-I/O layer (32-bit port). Output paths
(`klog`, the exception handlers, and the console device) call `terminal_putc` /
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
`kernel/drivers/display.h` (`name`, `clear`, `putc`, `status`, `cursor`). The
VGA text backend is selected at boot and stays the fallback; when the
bootloader hands over a usable VBE mode, the arch glue
(`kernel/drivers/framebuffer.c` on 32 bits, `kernel/arch/x86_64/kernel/fb64.c`
on 64) maps the aperture and calls `fbcon_init`, and the boot path switches the
active backend with `display_use(&fbcon_display)`. Both arches read the mode
from the same record (`kernel/drivers/fbhandover.h`, written by stage 2 at
physical `0x6FC0`) and run the same renderer, so the two screens are the same
pixels. Backend state is protected with IRQ-save spinlocks because a timer
interrupt redraws the status bar: the 32-bit heartbeat stops when userland owns
the console, while the 64-bit bar keeps its uptime ticking (see the notes
below).

`kernel/drivers/fbcon.c` owns the framebuffer console: a 256x72 shadow of the
character grid (so the cursor underline can be erased by repainting the cell
underneath it), glyph blitting through the font descriptor, scrolling of the
text area only, and a status bar. The font is Terminus 8x16 bold, embedded as
generated C data (`kernel/drivers/font_terminus.c`, produced from
`ter-u16b.bdf` by `tools/mkfont.py`, SIL OFL 1.1, see
`LICENSES/Terminus-OFL.txt`). Bitmaps are 256 glyphs in code page 437 order, so
the byte the console prints indexes the glyph directly; the renderer is
generic over the cell size, and `fbcon_init` rejects a font whose width is not
a multiple of 8. Two rows at the bottom are reserved for the status bar:
`display_status(left, right)` draws the two halves (either may be `0`) on a
tinted band, and the hidden 16x2 signature block the headless screendump checks
lives in the second of those rows. The renderer copies both strings, so a
later `clear` repaints the bar instead of losing it. The 32-bit PIT puts
`T+<n>s` on the right; the 64-bit console puts the uptime there from IRQ0.

Console output is filtered for control codes on the 32-bit console: a CSI
sequence (`ESC '[' params final`) is consumed instead of drawn, so an escape
sequence never appears as text. The console answers the one query a program
waits for, the cursor-position report `ESC[6n` that busybox's line editor sends
before every prompt: `terminal_emit` reads the 1-based cursor from the active
backend (`display_cursor`) and pushes `ESC[<row>;<col>R` into the terminal
input ring, so the editor sees the reply the way it would see typed bytes. The
prompt then renders as `$ ` instead of `$ [6n`. Both backends also implement
`\b` (move the cursor left) and `\r` (return to column 1), which is what the
line editor uses to erase and repaint. Other CSI sequences are ignored rather
than executed: there is no cursor addressing, colour, or scrolling region.

## The 64-bit console

The x86-64 port has no terminal layer yet, so its console is two drivers and a
queue rather than a line discipline.

`kernel/arch/x86_64/kernel/console64.c` owns the output tee. `k64_putc` writes
COM1 first and the display second, once `console64_init` has selected a
backend: a fault on the screen side must not cost the headless check its log.
Before that the boot path only writes serial, because the display is still
whatever firmware left behind. `console64_init` runs right after
`heap64_init` -- the framebuffer needs page tables and a frame allocator --
and reports `HAKU64-FB-OK` with the grid and font, or `HAKU64-FB-SKIP` when the
VBE handover is missing and VGA text stays active.

The aperture is mapped into its own PML4 slot (`K64_FB_VIRT`, uncached,
PCD+PWT, 4 MiB window, 4 KiB pages) and that slot is copied into every process
page table: `write()` paints from a system call, which runs on the caller's
`CR3`. The frames the aperture covers are reserved out of the frame allocator
first, because firmware may place the framebuffer inside RAM.

`kernel/arch/x86_64/kernel/kbd64.c` decodes set-1 scancodes from IRQ1 with the
same US-layout tables and shift/caps handling as `kernel/drivers/kbd.c`, and
pushes the character into a 128-byte ring. The ring drops the newest byte when
it is full (losing a keystroke beats overwriting one the reader has not seen),
and the `0xE0` prefix is ignored, so the arrow keys and the keypad do nothing
yet -- exactly as on the 32-bit side. IRQ1 is unmasked only after a decode-only
self-test proves the tables (`HAKU64-KBD-OK`), which keeps the check
independent of whether the headless run types anything.

Console `read()` pops the queue and echoes what it took, so typing is visible;
there is no erase key and no line buffering. When the queue is empty the task
parks with `-EAGAIN` on `K64_CONSOLE_WAIT_ADDR` instead of spinning, and IRQ1
wakes it. The park goes through `sched64_park_until`, which re-runs the
"is there a byte?" test with the task lock held: a keystroke that arrives
between the empty check and the state change is therefore never lost. A caller
sees `-EAGAIN` after a real switch away and retries, which is what
`user/tests/init64.c` does while it waits for the `x` the headless run types
(`HAKU64-KEY-READY` then `HAKU64-KEY-OK`).

## Logging, panic, and crash reports

`kernel/kernel/klog.c` provides five severities (`DEBUG`, `INFO`, `WARN`,
`ERROR`, `CRIT`) with an `INFO` default threshold. `klog_log` and
`klog_begin` add the tag and honour `klog_set_level`; the plain
`klog_write`/`klog_prefix` path stays unconditional for boot markers and
test output. `klog_self_test` proves that `DEBUG` is suppressed at the
default threshold and appears after lowering it (`KLOG-LEVELS-OK`).

The timer heartbeat is boot progress, not a service: `pit_tick` prints
`T+<n>s: timer heartbeat` once a second and puts `T+<n>s` on the right half of
the status bar. The heartbeat follows the same console rule as every other
kernel log (see below): the line goes to COM1 once userland owns the console,
and the 32-bit heartbeat stops updating the bar at the same point. The serial log
keeps the timeline, which is what the headless check asserts with its `T+1s`,
`T+2s`, and `T+3s` markers. A console that never started a user task still
shows the heartbeat.

Kernel log lines are a boot-time channel on the screen. `usermode_run` calls
`klog_mirror_display(0)` right before `scheduler_start`, so from the first
user task onward every klog line (heartbeat, exec notes, ring-3 fault reports,
the kernel-side FS tests that run after userland exits) goes to COM1 only and
never draws over the shell. The serial log stays complete. `panic()` is the
one exception: it enables the mirror again before printing `KERNEL PANIC`,
because a dead kernel has to leave its reason on the screen. The mirror is a
one-way latch, so a shell that exits or a crash does not bring kernel output
back.

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
