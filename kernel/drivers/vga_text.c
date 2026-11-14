/* Haku VGA text display backend.
 *
 * Used from task and interrupt context (the timer redraws the status line
 * from IRQ0), so cursor state is protected with an IRQ-save lock.
 */
#include "vga_text.h"
#include "sync.h"

static volatile uint16_t *const VGA = (uint16_t *)0xB8000;
static uint8_t vga_row = 0;
static uint8_t vga_col = 0;
static uint8_t vga_color = 0x0F; /* bright white on black */
static spinlock_t vga_lock = SPINLOCK_INIT;

/* The status text is copied in: the caller's buffer may be gone by the time a
 * later clear has to repaint row 0. */
#define VGA_BAR_MAX (VGA_COLS + 1)
static char vga_bar_left[VGA_BAR_MAX];
static char vga_bar_right[VGA_BAR_MAX];

static void vga_bar_copy(char *destination, const char *source) {
    size_t i = 0;

    if (source)
        while (source[i] && i + 1u < VGA_BAR_MAX) {
            destination[i] = source[i];
            i++;
        }
    destination[i] = '\0';
}

/* Paint row 0 from the cached copies. The VGA text buffer has no reserved
 * row, so the bar shares it with the log; the right side wins a collision.
 * Caller holds vga_lock. */
static void vga_bar_paint_locked(void) {
    size_t left_len = strlen(vga_bar_left);
    size_t right_len = strlen(vga_bar_right);
    uint16_t attribute = (uint16_t)0x0E << 8;   /* yellow on black */

    for (size_t i = 0; i < VGA_COLS; i++)
        VGA[i] = (uint16_t)(attribute | ' ');
    if (!left_len && !right_len) return;
    if (left_len > VGA_COLS) left_len = VGA_COLS;
    if (right_len > VGA_COLS) right_len = VGA_COLS;
    if (left_len + right_len > VGA_COLS) left_len = VGA_COLS - right_len;
    for (size_t i = 0; i < left_len; i++)
        VGA[i] = (uint16_t)(attribute | (uint8_t)vga_bar_left[i]);
    for (size_t i = 0; i < right_len; i++)
        VGA[VGA_COLS - right_len + i] =
            (uint16_t)(attribute | (uint8_t)vga_bar_right[i]);
}

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
    vga_bar_paint_locked();
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

/* Set the two halves of the status bar on row 0. A NULL half keeps the text
 * that end already had, matching the framebuffer backend. */
static void vga_status(const char *left, const char *right) {
    irqflags_t flags;

    if (!left && !right) return;
    flags = spin_lock_irqsave(&vga_lock);
    if (left) vga_bar_copy(vga_bar_left, left);
    if (right) vga_bar_copy(vga_bar_right, right);
    vga_bar_paint_locked();
    spin_unlock_irqrestore(&vga_lock, flags);
}

const display_ops_t vga_text_display = {
    "vga-text",
    vga_clear,
    vga_putc,
    vga_status,
    vga_cursor,
};
