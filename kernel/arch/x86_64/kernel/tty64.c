/* Haku x86-64 console line discipline: canonical input for one terminal.
 *
 * kbd64.c owns the IRQ1 queue of decoded characters. This file is the policy
 * on top of it: a process that reads the console gets whole lines, not the
 * bytes as they arrive. Enter commits the line, erase (0x08 and the reported
 * VERASE 0x7F) removes the last character, and ^D ends the line: an empty line
 * is end-of-file while a partial line is delivered without its newline. Every
 * accepted byte is echoed as it is consumed, which is why erase also paints
 * the usual backspace-space-backspace.
 *
 * The reported termios sets ISIG as well, so ^C is not text: it discards the
 * line and queues SIGINT for the console's foreground group (the caller's own
 * group when no foreground group is stored, which is the same "not set means
 * everyone" rule the read path uses). ^\ and ^Z are still ordinary bytes;
 * turning them into SIGQUIT and SIGTSTP is future work, not something this
 * file pretends to do.
 *
 * One line is buffered at a time. Once a line is waiting for the reader the
 * pump stops, so type-ahead stays in the keyboard ring instead of overwriting
 * the line the reader has not taken yet. There is one console and, by the
 * foreground-group rule, one reader at a time; the line state is therefore
 * plain statics rather than a second lock. The keyboard ring itself keeps its
 * IRQ-safe lock, so a producer never sees a torn queue.
 */
#include "haku64.h"

/* The reported VERASE is 0x7F. A PC backspace arrives as 0x08, so both are
 * treated as erase, exactly like a serial terminal that sends DEL. */
#define TTY64_BS     0x08
#define TTY64_DEL    0x7F
#define TTY64_VINTR  0x03
#define TTY64_VEOF   0x04

#define TTY64_LINE_MAX 512u
static char tty_line[TTY64_LINE_MAX];
static u32 tty_len;              /* bytes in the line being edited */
static u32 tty_pos;              /* bytes of a finished line already read */
static int tty_done;             /* a finished line (or ^D) is waiting */

void tty64_reset(void) {
    tty_len = 0;
    tty_pos = 0;
    tty_done = 0;
}

int tty64_pending_line(void) {
    return tty_done;
}

/* Echo one byte of input processing. User output bypasses the kernel-log
 * display latch, because what a person typed belongs on the screen. */
static void tty_echo(char c) {
    console64_user_putc(c);
}

/* Consume queued keys into the line buffer. Stops as soon as a line is
 * finished (the rest waits in the keyboard ring), and never runs longer than
 * the ring it drains, so a hostile typist cannot turn this into unbounded
 * work. Returns how many keys were consumed; `interrupted` is set when ^C
 * discarded the line, so the read can fail with EINTR and let the pending
 * SIGINT be delivered on the way back to user mode instead of parking. */
u32 tty64_pump(int id, int *interrupted) {
    u32 consumed = 0;
    int ch;

    if (interrupted) *interrupted = 0;
    if (tty_done) return 0;
    while ((ch = kbd64_read()) >= 0) {
        consumed++;
        /* Enter is the only newline the PS/2 driver produces; CR is mapped
         * here so a caller that reports ICRNL sees the same behaviour. */
        if (ch == '\r') ch = '\n';

        if (ch == TTY64_BS || ch == TTY64_DEL) {
            if (tty_len) {
                tty_len--;
                tty_echo('\b');
                tty_echo(' ');
                tty_echo('\b');
            }
            continue;
        }

        if (ch == TTY64_VEOF) {
            /* ^D closes the line. An empty line is the zero-length EOF
             * read; a partial line is delivered without a newline. */
            tty_pos = 0;
            tty_done = 1;
            tty_echo('^');
            tty_echo('D');
            break;
        }

        if (ch == TTY64_VINTR) {
            u32 pgrp = sched64_console_fg();

            if (!pgrp) pgrp = sched64_pgrp(id);
            tty_echo('^');
            tty_echo('C');
            tty_echo('\r');
            tty_echo('\n');
            tty_len = 0;
            tty_pos = 0;
            if (pgrp) (void)sched64_signal_pgrp(pgrp, K64_SIGINT);
            if (interrupted) *interrupted = 1;
            break;
        }

        if (ch == '\n') {
            if (tty_len + 1u < TTY64_LINE_MAX) tty_line[tty_len++] = '\n';
            tty_echo('\r');
            tty_echo('\n');
            tty_pos = 0;
            tty_done = 1;
            break;
        }

        /* A line longer than the buffer drops the excess instead of growing
         * without a bound. The reader still gets a full, valid line. */
        if (tty_len + 1u < TTY64_LINE_MAX) {
            tty_line[tty_len++] = (char)ch;
            tty_echo((char)ch);
        }
    }
    return consumed;
}

/* Hand out bytes of the finished line. Returns 1 when this call produced a
 * read result (including the zero-length EOF result), 0 when no line is
 * waiting. A short count leaves the rest of the line for the next read, which
 * is what a canonical terminal promises. */
int tty64_take_line(void *dst, u32 count, u32 *copied) {
    u32 n;

    *copied = 0;
    if (!tty_done) return 0;

    n = tty_len - tty_pos;
    if (n > count) n = count;
    if (n) memcpy(dst, tty_line + tty_pos, n);
    tty_pos += n;
    *copied = n;

    if (tty_pos >= tty_len) {
        tty_done = 0;
        tty_len = 0;
        tty_pos = 0;
    }
    return 1;
}

/* TCFLSH: discard queued keys and the line being edited. Both loops are
 * bounded by the keyboard ring, so this cannot spin. */
void tty64_flush(void) {
    while (kbd64_read() >= 0) { }
    tty64_reset();
}
