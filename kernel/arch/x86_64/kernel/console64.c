/* Haku x86-64 console: one byte to the screen and one to COM1.
 *
 * Before this runs, k64_putc only writes the serial port: the display is
 * still whatever firmware left behind, and the boot path must not scribble on
 * it until it knows which backend is usable. console64_init() selects one --
 * the VBE framebuffer when stage 2 handed one over, the VGA text buffer
 * otherwise -- and from then on every printed byte is teed.
 *
 * The headless check reads COM1, so the tee must never drop the serial side:
 * that is the invariant this file exists to keep in one place.
 */
#include "haku64.h"
#include "display.h"
#include "fbcon.h"
#include "vga_text.h"

static int console_up;

/* Print the selected backend and its geometry, the way the 32-bit port does,
 * so the serial log names what the screen is showing. */
static void report_backend(int framebuffer) {
    if (framebuffer) {
        k64_puts("HAKU64-FB-OK: vbe linear framebuffer ");
        k64_dec(fbcon_width());
        k64_puts("x");
        k64_dec(fbcon_height());
        k64_puts(" grid=");
        k64_dec(fbcon_columns());
        k64_puts("x");
        k64_dec(fbcon_rows());
        k64_puts(" font=");
        k64_puts(fbcon_font_name());
        k64_puts("\r\n");
    } else {
        k64_puts("HAKU64-FB-SKIP: no usable linear framebuffer,"
                 " VGA text retained\r\n");
    }
    k64_puts("HAKU64-TERMINAL-OK: console=");
    k64_puts(display_name());
    k64_puts(" ");
    k64_dec(console64_columns());
    k64_puts("x");
    k64_dec(console64_rows());
    k64_puts(" serial=com1\r\n");
}

void console64_init(void) {
    int framebuffer;

    if (console_up) return;
    display_init();
    framebuffer = fb64_init();
    if (framebuffer) display_use(&fbcon_display);
    console_up = 1;
    report_backend(framebuffer);
    console64_status();
}

/* --- status bar -------------------------------------------------------- */

/* Small formatters: the bar is the only place that needs a number inside a
 * string, and there is no printf on this port. */
static char *status_text(char *out, const char *text) {
    while (*text) *out++ = *text++;
    return out;
}

static char *status_number(char *out, u64 value, u32 width) {
    char digits[20];
    u32 count = 0;

    do {
        digits[count++] = (char)('0' + (u32)(value % 10u));
        value /= 10u;
    } while (value && count < sizeof(digits));
    while (count < width && count < sizeof(digits))
        digits[count++] = '0';
    while (count) *out++ = digits[--count];
    return out;
}

/* The status bar names the machine on the left and ticks the uptime on the
 * right. It is the one piece of the screen that changes with no syscall, so
 * the timer interrupt refreshes it once a second (idt64.c); everything else
 * is painted by the console read/write paths. */
void console64_status(void) {
    /* Bounded by construction: the prefix, two small numbers and the longest
     * font name are well under 64 bytes; the right side is "up HH:MM:SS". */
    char left[64];
    char right[32];
    const char *font = fbcon_active() ? fbcon_font_name() : "vga-text";
    u64 seconds, hours, minutes;
    char *out;

    if (!console_up) return;

    out = status_text(left, "Haku64 x86-64  ");
    out = status_number(out, console64_columns(), 0);
    *out++ = 'x';
    out = status_number(out, console64_rows(), 0);
    *out++ = ' ';
    *out++ = ' ';
    out = status_text(out, font);
    *out = '\0';

    seconds = timer64_ticks() / (u64)(timer64_hz() ? timer64_hz() : 1u);
    hours = seconds / 3600u;
    minutes = (seconds / 60u) % 60u;
    seconds %= 60u;
    out = status_text(right, "up ");
    if (hours) {
        out = status_number(out, hours, 0);
        *out++ = ':';
    }
    out = status_number(out, minutes, 2);
    *out++ = ':';
    out = status_number(out, seconds, 2);
    *out = '\0';

    display_status(left, right);
}

/* The one output path. Serial first, because a fault in the display path must
 * not cost the headless check its log: every marker the build asserts is on
 * COM1 before the screen sees the byte. */
void k64_putc(char c) {
    serial_putc64(c);
    if (console_up) display_putc(c);
}

void k64_puts(const char *text) {
    if (!text) return;
    if (!console_up) {
        /* Boot path: the display has no backend yet, so take the assembly
         * loop rather than one call per character. */
        serial_write64(text);
        return;
    }
    while (*text) k64_putc(*text++);
}

int console64_ready(void) {
    return console_up;
}

u32 console64_columns(void) {
    return fbcon_active() ? fbcon_columns() : (u32)VGA_COLS;
}

u32 console64_rows(void) {
    return fbcon_active() ? fbcon_rows() : (u32)VGA_ROWS;
}

const char *console64_name(void) {
    return display_name();
}
