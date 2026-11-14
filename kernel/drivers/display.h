/* Haku display backends: the shared text-output API.
 *
 * A backend owns one screen. The VGA text buffer is always available and is
 * selected first; the framebuffer driver registers itself as the active
 * backend once it has mapped a mode. The terminal talks only to this
 * interface, so adding another display does not touch input or syscalls.
 */
#ifndef HAKU_DISPLAY_H
#define HAKU_DISPLAY_H

#include "cpu.h"

typedef struct display_ops {
    const char *name;
    void (*clear)(uint8_t color);
    void (*putc)(char c);
    /* Status bar: `left` is drawn at the start of the bar, `right` at its
     * end, and either may be NULL. A backend without room for a bar may
     * overlap it with the text area. */
    void (*status)(const char *left, const char *right);
    /* 1-based text cursor position, for the terminal's cursor report. */
    void (*cursor)(uint32_t *row, uint32_t *col);
} display_ops_t;

/* Select the VGA text backend and clear the screen. */
void display_init(void);
/* Make a backend current (called after its probe succeeded). */
void display_use(const display_ops_t *ops);
const display_ops_t *display_active(void);
const char *display_name(void);

void display_putc(char c);
void display_clear(uint8_t color);
/* Set the status bar. Either side may be NULL to leave that end alone; a
 * NULL pair is a no-op. */
void display_status(const char *left, const char *right);
/* Text cursor as a 1-based (row, column) pair. Returns 0 when the active
 * backend does not track one. */
int display_cursor(uint32_t *row, uint32_t *col);

#endif
