# Terminal, display, and input

[Back to documentation index](README.md)

The console backends, keyboard input, the kernel log display latch, and
panic and crash reporting.

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
`kernel/drivers/display.h` (`name`, `clear`, `putc`, `status`, `cursor`). The
VGA text backend is selected at boot and stays the fallback; when the
bootloader hands over a usable VBE mode, `framebuffer_init` succeeds and the
boot path switches the active backend with `display_use(&framebuffer_display)`.
Backend state is protected with IRQ-save spinlocks because the timer redraws
the status line from interrupt context while the console is still booting (the
redraw stops once userland owns the console; see the heartbeat note below).

Console output is filtered for control codes: a CSI sequence (`ESC '[' params
final`) is consumed instead of drawn, so an escape sequence never appears as
text. The console answers the one query a program waits for, the
cursor-position report `ESC[6n` that busybox's line editor sends before every
prompt: `terminal_emit` reads the 1-based cursor from the active backend
(`display_cursor`) and pushes `ESC[<row>;<col>R` into the terminal input ring,
so the editor sees the reply the way it would see typed bytes. The prompt then
renders as `$ ` instead of `$ [6n`. Both backends also implement `\b` (move the
cursor left) and `\r` (return to column 1), which is what the line editor uses
to erase and repaint. Other CSI sequences are ignored rather than executed:
there is no cursor addressing, colour, or scrolling region.

## Logging, panic, and crash reports

`kernel/kernel/klog.c` provides five severities (`DEBUG`, `INFO`, `WARN`,
`ERROR`, `CRIT`) with an `INFO` default threshold. `klog_log` and
`klog_begin` add the tag and honour `klog_set_level`; the plain
`klog_write`/`klog_prefix` path stays unconditional for boot markers and
test output. `klog_self_test` proves that `DEBUG` is suppressed at the
default threshold and appears after lowering it (`KLOG-LEVELS-OK`).

The timer heartbeat is boot progress, not a service: `pit_tick` prints
`T+<n>s: timer heartbeat` once a second and updates the status counter in the
top-right corner. The heartbeat follows the same console rule as every other
kernel log (see below): the line goes to COM1 once userland owns the console,
and the corner counter stops at the same point. The serial log keeps the
timeline, which is what the headless check asserts with its `T+1s`, `T+2s`,
and `T+3s` markers. A console that never started a user task still shows the
heartbeat.

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
