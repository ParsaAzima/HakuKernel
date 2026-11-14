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
