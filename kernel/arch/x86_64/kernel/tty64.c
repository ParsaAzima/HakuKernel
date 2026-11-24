/* Haku x86-64 console line discipline: canonical input for one terminal.
 *
 * kbd64.c owns the IRQ1 queue of decoded characters. This file is the policy
 * on top of it: a process that reads the console gets whole lines, not the
 * bytes as they arrive. Enter commits the line, erase (0x08 and the reported
 * VERASE 0x7F) removes the character before the cursor, and ^D ends the line:
 * an empty line is end-of-file while a partial line is delivered without its
 * newline. Every accepted byte is echoed as it is consumed, which is why
 * erase also paints the usual backspace-space-backspace.
 *
 * The reported termios sets ISIG as well, so ^C is not text: it discards the
 * line and queues SIGINT for the console's foreground group (the caller's own
 * group when no foreground group is stored, which is the same "not set means
 * everyone" rule the read path uses). ^\ and ^Z are still ordinary bytes;
 * turning them into SIGQUIT and SIGTSTP is future work, not something this
 * file pretends to do.
 *
 * 0xE0-prefixed keys arrive as K64_KEY_* markers above 0x7F and are consumed
 * here, never handed to a reader: Up/Down walk a bounded ring of committed
 * lines, Left/Right/Home/End move an insertion point inside the line, and
 * Page Up/Down move the framebuffer console's scrollback view. A line being
 * edited when history is first entered is staged, so Down eventually brings
 * it back. History is not cleared by ^C, ^D, TCFLSH or a short read; only the
 * kernel's reset path drops the staging state.
 *
 * One line is buffered at a time. Once a line is waiting for the reader the
 * pump stops, so type-ahead stays in the keyboard ring instead of overwriting
 * the line the reader has not taken yet. There is one console and, by the
 * foreground-group rule, one reader at a time; the line state is therefore
 * plain statics rather than a second lock. The keyboard ring itself keeps its
 * IRQ-safe lock, so a producer never sees a torn queue.
 */
#include "haku64.h"
#include "fbcon.h"

/* The reported VERASE is 0x7F. A PC backspace arrives as 0x08, so both are
 * treated as erase, exactly like a serial terminal that sends DEL. */
#define TTY64_BS     0x08
#define TTY64_DEL    0x7F
#define TTY64_VINTR  0x03
#define TTY64_VEOF   0x04

#define TTY64_LINE_MAX 512u
#define TTY64_HISTORY_MAX 32u

static char tty_line[TTY64_LINE_MAX];
static u32 tty_len;              /* bytes in the line being edited */
static u32 tty_pos;              /* bytes of a finished line already read */
static int tty_done;             /* a finished line (or ^D) is waiting */

/* Committed lines, oldest first in ring order. The newline is stored in the
 * line but not in history: a recalled entry is an editable line again. */
static char tty_history[TTY64_HISTORY_MAX][TTY64_LINE_MAX];
static u32 tty_hist_len[TTY64_HISTORY_MAX];
static u32 tty_hist_start;
static u32 tty_hist_count;
static u32 tty_hist_cursor;      /* 0 = editing the live line, n = n back */
static char tty_staged[TTY64_LINE_MAX];
static u32 tty_staged_len;

/* Insertion point while editing and the number of characters currently
 * painted since the start of the line. The start cell (row and column) is
 * captured from the first key of each line so a redraw can walk back to it
 * even when the text wrapped over several rows. */
static u32 tty_edit;
static u32 tty_shown;
static u32 tty_start_row, tty_start_col; /* 1-based row, 0-based column */
static int tty_started;

void tty64_reset(void) {
    tty_len = 0;
    tty_pos = 0;
    tty_done = 0;
    tty_edit = 0;
    tty_shown = 0;
    tty_started = 0;
    tty_hist_cursor = 0;
    tty_staged_len = 0;
}

int tty64_pending_line(void) {
    return tty_done;
}

/* Echo one byte of input processing. User output bypasses the kernel-log
 * display latch, because what a person typed belongs on the screen. */
static void tty_echo(char c) {
    console64_user_putc(c);
}

static void tty_capture_start(void) {
    u32 row, col;

    if (tty_started) return;
    console64_user_cursor(&row, &col);
    tty_start_row = row;
    tty_start_col = col ? col - 1u : 0u;
    tty_started = 1;
}

/* Walk back to the first cell of the edit line. The number of rows to climb
 * comes from the character count and the grid width, not from an absolute
 * row: if the redraw scrolled the screen, the anchor and the cursor moved
 * together, so the relative distance is still correct. */
static void tty_goto_start(void) {
    u32 cols = console64_columns();
    u32 rows;

    if (!tty_started || !cols) return;
    rows = (tty_start_col + tty_shown) / cols;
    if (rows) console64_user_cursor_up(rows);
    console64_user_cursor_col(tty_start_col);
}

/* Repaint the line from its start, erase any tail left by a longer version,
 * then walk back and redraw up to the insertion point to place the cursor.
 * Every echo also cancels a scrollback view, through console64_user_putc. */
static void tty_refresh(void) {
    u32 i;

    tty_capture_start();
    tty_goto_start();
    for (i = 0; i < tty_len; i++) tty_echo(tty_line[i]);
    for (i = tty_len; i < tty_shown; i++) tty_echo(' ');
    tty_goto_start();
    for (i = 0; i < tty_edit; i++) tty_echo(tty_line[i]);
    tty_shown = tty_len;
}

static char *tty_hist_slot(u32 logical) {
    return tty_history[(tty_hist_start + logical) % TTY64_HISTORY_MAX];
}

/* Remember a committed line. Consecutive duplicates collapse, like a real
 * shell, and the oldest entry is dropped when the ring fills. */
static void tty_hist_push(void) {
    u32 slot, last;

    if (!tty_len) return;
    if (tty_hist_count) {
        last = (tty_hist_start + tty_hist_count - 1u) % TTY64_HISTORY_MAX;
        if (tty_hist_len[last] == tty_len &&
            !memcmp(tty_history[last], tty_line, tty_len)) return;
    }
    if (tty_hist_count == TTY64_HISTORY_MAX)
        tty_hist_start = (tty_hist_start + 1u) % TTY64_HISTORY_MAX;
    else
        tty_hist_count++;
    slot = (tty_hist_start + tty_hist_count - 1u) % TTY64_HISTORY_MAX;
    memcpy(tty_history[slot], tty_line, tty_len);
    tty_hist_len[slot] = tty_len;
}

/* Walk history one entry at a time. Index 0 is the line in progress; the
 * first Up stages it, and the matching Down restores it. Index n loads the
 * n-th most recent committed line. */
static void tty_hist_recall(int older) {
    u32 idx;

    if (!tty_hist_count) return;
    if (older) {
        if (!tty_hist_cursor) {
            tty_staged_len = tty_len;
            if (tty_len) memcpy(tty_staged, tty_line, tty_len);
        }
        if (tty_hist_cursor == tty_hist_count) return;
        tty_hist_cursor++;
    } else {
        if (!tty_hist_cursor) return;
        tty_hist_cursor--;
    }

    if (!tty_hist_cursor) {
        tty_len = tty_staged_len;
        if (tty_len) memcpy(tty_line, tty_staged, tty_len);
    } else {
        idx = tty_hist_count - tty_hist_cursor;
        tty_len = tty_hist_len[idx];
        if (tty_len) memcpy(tty_line, tty_hist_slot(idx), tty_len);
    }
    tty_edit = tty_len;
    tty_refresh();
}

/* Finish the current line. History is captured before the newline is added,
 * so Enter on a recalled entry records the edited text, not a duplicate of
 * the stale newline. */
static void tty_commit(int add_newline) {
    tty_hist_push();
    tty_hist_cursor = 0;
    tty_staged_len = 0;
    if (add_newline && tty_len + 1u < TTY64_LINE_MAX)
        tty_line[tty_len++] = '\n';
    tty_pos = 0;
    tty_done = 1;
    tty_shown = 0;
    tty_started = 0;
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
        /* Remember where this line began before anything is echoed. */
        tty_capture_start();
        /* Enter is the only newline the PS/2 driver produces; CR is mapped
         * here so a caller that reports ICRNL sees the same behaviour. */
        if (ch == '\r') ch = '\n';

        if (ch == K64_KEY_UP || ch == K64_KEY_DOWN) {
            tty_hist_recall(ch == K64_KEY_UP);
            continue;
        }
        if (ch == K64_KEY_LEFT || ch == K64_KEY_RIGHT ||
            ch == K64_KEY_HOME || ch == K64_KEY_END) {
            u32 target = tty_edit;

            if (ch == K64_KEY_LEFT && tty_edit) target--;
            else if (ch == K64_KEY_RIGHT && tty_edit < tty_len) target++;
            else if (ch == K64_KEY_HOME) target = 0;
            else if (ch == K64_KEY_END) target = tty_len;
            if (target != tty_edit) {
                tty_edit = target;
                tty_refresh();
            }
            continue;
        }
        if (ch == K64_KEY_PGUP || ch == K64_KEY_PGDN) {
            int view = fbcon_scroll_view(ch == K64_KEY_PGUP ? 1 : -1);

            /* After console handover k64_puts is serial-only, so the marker
             * proves the key reached the renderer without drawing over the
             * program that is running. */
            k64_puts(ch == K64_KEY_PGUP ? "HAKU64-SCROLL-UP: offset="
                                        : "HAKU64-SCROLL-DOWN: offset=");
            k64_dec((u64)view);
            k64_puts(" rows=");
            k64_dec((u64)fbcon_scrollback_rows());
            k64_puts("\r\n");
            continue;
        }

        if (ch == TTY64_BS || ch == TTY64_DEL) {
            if (tty_edit) {
                memmove(tty_line + tty_edit - 1u, tty_line + tty_edit,
                        tty_len - tty_edit);
                tty_len--;
                tty_edit--;
                tty_refresh();
            }
            continue;
        }

        if (ch == TTY64_VEOF) {
            /* ^D closes the line. An empty line is the zero-length EOF
             * read; a partial line is delivered without a newline. */
            tty_commit(0);
            tty_echo('^');
            tty_echo('D');
            tty_shown = 2;
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
            tty_edit = 0;
            tty_shown = 0;
            tty_started = 0;
            tty_hist_cursor = 0;
            tty_staged_len = 0;
            if (pgrp) (void)sched64_signal_pgrp(pgrp, K64_SIGINT);
            if (interrupted) *interrupted = 1;
            break;
        }

        if (ch == '\n') {
            tty_commit(1);
            tty_echo('\r');
            tty_echo('\n');
            break;
        }

        /* A line longer than the buffer drops the excess instead of growing
         * without a bound. The insertion point makes this an O(line) memmove
         * per key at worst, bounded by TTY64_LINE_MAX. */
        if (tty_len + 1u < TTY64_LINE_MAX) {
            if (tty_edit < tty_len)
                memmove(tty_line + tty_edit + 1u, tty_line + tty_edit,
                        tty_len - tty_edit);
            tty_line[tty_edit++] = (char)ch;
            tty_len++;
            if (tty_edit == tty_len) {
                tty_echo((char)ch);
                tty_shown = tty_len;
            } else {
                tty_refresh();
            }
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
        tty_len = 0;
        tty_pos = 0;
        tty_done = 0;
        tty_edit = 0;
        tty_shown = 0;
        tty_started = 0;
    }
    return 1;
}

/* TCFLSH: discard queued keys and the line being edited. Both loops are
 * bounded by the keyboard ring, so this cannot spin. History survives. */
void tty64_flush(void) {
    while (kbd64_read() >= 0) { }
    tty64_reset();
}

/* Prove that an edit in the middle of a line that wrapped over several
 * screen rows puts the cursor back at the insertion point and leaves the
 * right text in the line. IRQ1 is still masked here, so the injected keys
 * cannot race a real keystroke; the echo paints a temporary self-test line,
 * and a CRLF at the end leaves the next boot marker on a fresh row. */
int tty64_self_test(void) {
    static char line[TTY64_LINE_MAX];
    u32 copied = 0;
    u32 row, col, expect_row, expect_col;
    int ok = 1;

    tty64_reset();
    tty_capture_start();
    /* The keyboard ring is 128 bytes, so drain it in chunks rather than
     * dropping the tail of a deliberately longer-than-screen line. */
    for (u32 i = 0; i < 130u; i++) {
        kbd64_inject('a');
        if ((i % 64u) == 63u) (void)tty64_pump(1, 0);
    }
    (void)tty64_pump(1, 0);
    kbd64_inject((char)K64_KEY_LEFT);
    kbd64_inject((char)K64_KEY_LEFT);
    kbd64_inject((char)TTY64_BS);
    (void)tty64_pump(1, 0);

    expect_row = tty_start_row +
                 (tty_start_col + tty_edit) / console64_columns();
    expect_col = (tty_start_col + tty_edit) % console64_columns() + 1u;
    console64_user_cursor(&row, &col);
    if (row != expect_row || col != expect_col) ok = 0;
    if (tty_len != 129u || tty_edit != 127u) ok = 0;

    kbd64_inject('\n');
    (void)tty64_pump(1, 0);
    if (!tty64_take_line(line, sizeof(line), &copied) || copied != 130u)
        ok = 0;
    for (u32 i = 0; i < 129u && ok; i++)
        if (line[i] != 'a') ok = 0;
    if (ok && line[129] != '\n') ok = 0;

    while (kbd64_read() >= 0) { }
    tty64_reset();
    console64_user_putc('\r');
    console64_user_putc('\n');
    if (!ok) return 0;
    k64_puts("HAKU64-TTY-REDRAW-OK: wrapped-line edit keeps cursor\r\n");
    return 1;
}
