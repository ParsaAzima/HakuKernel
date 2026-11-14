/* Haku bitmap font descriptions.
 *
 * The framebuffer console renders a fixed cell: one glyph per byte, one row
 * of pixels per byte of glyph data. A font descriptor carries the cell size
 * and the bitmaps, so the renderer never assumes 8x16 and a larger Terminus
 * cut is a data change rather than a code change.
 *
 * Bitmaps are 256 glyphs in code page 437 order, so the byte the terminal
 * prints is the glyph index. Row `r` of glyph `g` starts at
 * `glyphs[(g * height + r) * stride]` and holds the leftmost pixels in the
 * most significant bits, the way the VGA planes and BDF files do.
 */
#ifndef HAKU_FONT_H
#define HAKU_FONT_H

#include "cpu.h"

typedef struct {
    const char *name;
    uint32_t width;        /* cell width in pixels (a multiple of 8) */
    uint32_t height;       /* cell height in pixels (rows of glyph data) */
    uint32_t stride;       /* bytes per glyph row: width / 8 */
    const uint8_t *glyphs; /* 256 * height * stride bytes */
} haku_font_t;

/* Terminus 8x16 bold (kernel/drivers/font_terminus.c, generated from
 * ter-u16b.bdf by tools/mkfont.py). */
extern const haku_font_t terminus_8x16;

#endif
