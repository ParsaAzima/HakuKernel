/* Haku framebuffer console: mode description and public entry points.
 *
 * The arch glue owns finding the mode and making the aperture addressable;
 * this module owns everything that is drawn. Both ports feed it the same VBE
 * handover record from stage 2, so the 32-bit and 64-bit consoles are the
 * same pixels.
 */
#ifndef HAKU_FBCON_H
#define HAKU_FBCON_H

#include "cpu.h"
#include "display.h"
#include "font.h"

typedef struct {
    void *base;               /* pixel (0, 0), already mapped by the caller */
    uint32_t pitch;           /* bytes per scanline (may exceed width * bpp) */
    uint32_t width;           /* pixels */
    uint32_t height;          /* pixels */
    uint32_t bytes_per_pixel; /* 3 (24 bpp, stored BGR) or 4 (32 bpp) */
    uint32_t red_size, red_pos;
    uint32_t green_size, green_pos;
    uint32_t blue_size, blue_pos;
    const haku_font_t *font;
} fbcon_mode_t;

/* Take over the screen described by `mode`: clear it, install the font and
 * draw the first cursor. Returns 1 when the console is live, 0 when the mode
 * is unusable (the caller then keeps the VGA text backend). */
int fbcon_init(const fbcon_mode_t *mode);
int fbcon_active(void);

uint32_t fbcon_width(void);
uint32_t fbcon_height(void);
uint32_t fbcon_columns(void);
uint32_t fbcon_rows(void);
const char *fbcon_font_name(void);

/* display.h backend table; select it with display_use() once fbcon_init()
 * succeeded. */
extern const display_ops_t fbcon_display;

#endif
