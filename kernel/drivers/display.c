/* Haku display backend selection.
 *
 * Backends are const tables with no allocation and no locking: registers
 * are only written from the boot path before tasks and interrupts start,
 * or from display_putc under the backend's own IRQ-save lock.
 */
#include "display.h"
#include "vga_text.h"

static const display_ops_t *active;

void display_init(void) {
    if (active) return;
    active = &vga_text_display;
    display_clear(0x1F); /* white on blue, classic */
}

void display_use(const display_ops_t *ops) {
    if (ops && ops->putc && ops->clear) active = ops;
}

const display_ops_t *display_active(void) {
    return active;
}

const char *display_name(void) {
    return active ? active->name : "none";
}

void display_putc(char c) {
    if (active) active->putc(c);
}

void display_clear(uint8_t color) {
    if (active) active->clear(color);
}

void display_status(const char *left, const char *right) {
    if (active && active->status) active->status(left, right);
}

int display_cursor(uint32_t *row, uint32_t *col) {
    if (!active || !active->cursor) return 0;
    active->cursor(row, col);
    return 1;
}
