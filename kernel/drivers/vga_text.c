/* Haku VGA text display backend.
 *
 * A plain 80x25 text screen: no overlay, no reserved rows. Used from task and
 * interrupt context, so cursor state is protected with an IRQ-save lock.
 */
#include "vga_text.h"
#include "sync.h"

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static uint8_t vga_row = 0;
static uint8_t vga_col = 0;
static uint8_t vga_color = 0x0F; /* bright white on black */
static spinlock_t vga_lock = SPINLOCK_INIT;

static void vga_scroll(void) {
    memmove((void *)VGA, (void *)(VGA + VGA_COLS),
            (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));
    uint16_t blank = (uint16_t)((vga_color << 8) | ' ');
    for (int x = 0; x < VGA_COLS; x++)
        VGA[(VGA_ROWS - 1) * VGA_COLS + x] = blank;
    vga_row = VGA_ROWS - 1;
}

static void vga_clear(uint8_t color) {
    uint16_t blank = (uint16_t)((color << 8) | ' ');
    irqflags_t flags = spin_lock_irqsave(&vga_lock);
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA[i] = blank;
    vga_row = 0;
    vga_col = 0;
    vga_color = color;
    spin_unlock_irqrestore(&vga_lock, flags);
}

static void vga_putc(char c) {
    irqflags_t flags = spin_lock_irqsave(&vga_lock);
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS) vga_scroll();
        spin_unlock_irqrestore(&vga_lock, flags);
        return;
    }
    if (c == '\r') {
        vga_col = 0;
        spin_unlock_irqrestore(&vga_lock, flags);
        return;
    }
    if (c == '\b') {
        /* Move left only; the line editor overwrites with a space. */
        if (vga_col > 0) vga_col--;
        spin_unlock_irqrestore(&vga_lock, flags);
        return;
    }
    VGA[vga_row * VGA_COLS + vga_col] =
        (uint16_t)((vga_color << 8) | (uint8_t)c);
    if (++vga_col >= VGA_COLS) {
        vga_col = 0;
        if (++vga_row >= VGA_ROWS) vga_scroll();
    }
    spin_unlock_irqrestore(&vga_lock, flags);
}

/* 1-based text cursor, used to answer a terminal cursor-position report. */
static void vga_cursor(uint32_t *row, uint32_t *col) {
    irqflags_t flags = spin_lock_irqsave(&vga_lock);
    *row = (uint32_t)vga_row + 1u;
    *col = (uint32_t)vga_col + 1u;
    spin_unlock_irqrestore(&vga_lock, flags);
}

const display_ops_t vga_text_display = {
    "vga-text",
    vga_clear,
    vga_putc,
    vga_cursor,
};
