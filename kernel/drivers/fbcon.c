/* Haku framebuffer console: the shared text renderer for both ports.
 *
 * The 32-bit and 64-bit kernels discover the same VBE mode from stage 2 and
 * must draw it identically, so the renderer lives here and knows nothing
 * about page tables: an arch glue file maps the aperture, describes the mode
 * and calls fbcon_init(). Everything after that -- glyphs, scrolling, the
 * status line, the cursor -- is this file.
 *
 * A one-byte shadow of the character grid is kept so the cursor underline can
 * be erased by repainting the cell underneath it instead of by guessing. That
 * is the only state that is not derivable from pixels, and it is bounded by
 * FBCON_MAX_COLS x FBCON_MAX_ROWS; a mode larger than that still renders, it
 * just keeps the cursor as a plain overlay.
 */
#include "fbcon.h"
#include "sync.h"

/* 256x72 cells at 8x16 covers 2048x1152; the largest mode stage 2 selects is
 * 1024x768 (128x46 cells). The shadow is static because the renderer runs
 * from interrupt context and must never allocate. */
#define FBCON_MAX_COLS 256u
#define FBCON_MAX_ROWS 72u

static uint8_t fbcon_shadow[FBCON_MAX_COLS * FBCON_MAX_ROWS];

static int fb_ready;
static int fb_shadowed;
static uint8_t *fb_base;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;
static uint32_t fb_bytes_per_pixel;
static uint32_t fb_cols;
static uint32_t fb_rows;          /* total text rows on the screen */
static uint32_t fb_text_rows;     /* rows the scrolling text area uses */
static uint32_t fb_status_y;      /* first pixel row of the status bar */
static uint32_t fb_cell_w;
static uint32_t fb_cell_h;
static uint32_t fb_cell_stride;
static const haku_font_t *fb_font;
static uint32_t fb_cursor_x;
static uint32_t fb_cursor_y;
static uint32_t fb_fg;
static uint32_t fb_bg;
static uint32_t fb_status_fg;
static uint32_t fb_status_bg;
static uint32_t fb_cursor_fg;
static uint32_t fb_signature;
/* The status strings are copied in, because a caller's buffer may be a stack
 * frame that is gone by the time a later clear has to repaint the bar. */
#define FBCON_STATUS_MAX 96u
static char fb_status_left[FBCON_STATUS_MAX];
static char fb_status_right[FBCON_STATUS_MAX];
static uint32_t fb_r_size, fb_r_pos;
static uint32_t fb_g_size, fb_g_pos;
static uint32_t fb_b_size, fb_b_pos;
static spinlock_t fb_lock = SPINLOCK_INIT;

/* --- colour ------------------------------------------------------------ */

/* Scale an 8-bit channel into the mask the mode reported (Linux keeps the
 * mask size and the field position separately, like screen_info). */
static uint32_t channel(uint32_t value, uint32_t size, uint32_t pos) {
    if (!size) return 0;
    if (size > 8) size = 8;
    return ((value >> (8u - size)) << pos);
}

static uint32_t compose(uint32_t r, uint32_t g, uint32_t b) {
    return channel(r, fb_r_size, fb_r_pos) |
           channel(g, fb_g_size, fb_g_pos) |
           channel(b, fb_b_size, fb_b_pos);
}

/* --- pixels ------------------------------------------------------------ */

static inline void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    uint8_t *pixel;

    if (x >= fb_width || y >= fb_height) return;
    pixel = fb_base + y * fb_pitch + x * fb_bytes_per_pixel;
    if (fb_bytes_per_pixel == 4) {
        *(uint32_t *)pixel = color;
    } else {
        /* 24 bpp: the pixel is stored blue, green, red */
        pixel[0] = (uint8_t)(color & 0xFFu);
        pixel[1] = (uint8_t)((color >> 8) & 0xFFu);
        pixel[2] = (uint8_t)((color >> 16) & 0xFFu);
    }
}

/* One colour over an axis-aligned rectangle. The bounds are clamped once and
 * the scanline walk sits outside the pixel loop, so a full-screen clear is one
 * pass of stores and not a bounds-checked call per pixel. */
static void fb_fill(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                    uint32_t color) {
    uint32_t x_end = x0 + w;
    uint32_t y_end = y0 + h;

    if (!fb_base || x0 >= fb_width || y0 >= fb_height) return;
    if (x_end > fb_width) x_end = fb_width;
    if (y_end > fb_height) y_end = fb_height;
    for (uint32_t y = y0; y < y_end; y++) {
        uint8_t *row = fb_base + y * fb_pitch;

        if (fb_bytes_per_pixel == 4u) {
            uint32_t *pixel = (uint32_t *)row;
            for (uint32_t x = x0; x < x_end; x++) pixel[x] = color;
        } else {
            /* 24 bpp: the pixel is stored blue, green, red. */
            for (uint32_t x = x0; x < x_end; x++) {
                uint8_t *p = row + x * 3u;
                p[0] = (uint8_t)(color & 0xFFu);
                p[1] = (uint8_t)((color >> 8) & 0xFFu);
                p[2] = (uint8_t)((color >> 16) & 0xFFu);
            }
        }
    }
}

/* The signature block is the handshake the headless screendump checks for.
 * It lives in the unused second status row at the far right, so neither the
 * text area nor the status text can overwrite it. */
static void fb_draw_signature(void) {
    if (fb_status_y + fb_cell_h + 2u <= fb_height && fb_width >= 16u)
        fb_fill(fb_width - 16u, fb_status_y + fb_cell_h, 16u, 2u,
                fb_signature);
}

/* --- glyphs ------------------------------------------------------------ */

/* Paint one cell in the requested colours. `ch` selects the glyph; the row
 * bytes come from the font's stride, so a 16-pixel-wide cut works without a
 * second code path. */
static void fb_draw_glyph(uint32_t col, uint32_t row, char ch,
                          uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = fb_font->glyphs +
        (uint32_t)(uint8_t)ch * fb_cell_h * fb_cell_stride;
    uint32_t x0 = col * fb_cell_w;
    uint32_t y0 = row * fb_cell_h;

    if (x0 + fb_cell_w > fb_width || y0 + fb_cell_h > fb_height) return;
    for (uint32_t gy = 0; gy < fb_cell_h; gy++) {
        const uint8_t *bits = glyph + gy * fb_cell_stride;
        for (uint32_t gx = 0; gx < fb_cell_w; gx++) {
            uint32_t on = (bits[gx >> 3] >> (7u - (gx & 7u))) & 1u;
            fb_put_pixel(x0 + gx, y0 + gy, on ? fg : bg);
        }
    }
}

static void fb_draw_cell(uint32_t col, uint32_t row, char ch) {
    fb_draw_glyph(col, row, ch, fb_fg, fb_bg);
}

static void fb_shadow_set(uint32_t col, uint32_t row, char ch) {
    if (fb_shadowed) fbcon_shadow[row * fb_cols + col] = (uint8_t)ch;
}

static char fb_shadow_get(uint32_t col, uint32_t row) {
    if (!fb_shadowed) return ' ';
    return (char)fbcon_shadow[row * fb_cols + col];
}

/* --- cursor ------------------------------------------------------------ */

/* A two-pixel underline under the cell, in the status colour: visible on a
 * dark console and gone the moment the cell is repainted. */
static void fb_draw_cursor(void) {
    uint32_t y0 = fb_cursor_y * fb_cell_h + fb_cell_h - 2u;
    uint32_t x0 = fb_cursor_x * fb_cell_w;

    for (uint32_t y = y0; y < y0 + 2u; y++)
        for (uint32_t x = x0; x < x0 + fb_cell_w; x++)
            fb_put_pixel(x, y, fb_cursor_fg);
}

/* Repaint the cell the underline covers, so moving the cursor leaves no
 * pixels behind. Without a shadow there is nothing to repaint from: the
 * underline then simply stays until that cell is written again. */
static void fb_erase_cursor(void) {
    if (!fb_shadowed) return;
    fb_draw_cell(fb_cursor_x, fb_cursor_y,
                 fb_shadow_get(fb_cursor_x, fb_cursor_y));
}

/* --- status bar -------------------------------------------------------- */

static void fb_status_copy(char *destination, const char *source) {
    uint32_t i = 0;

    if (source)
        while (source[i] && i + 1u < FBCON_STATUS_MAX) {
            destination[i] = source[i];
            i++;
        }
    destination[i] = '\0';
}

/* Repaint the bar -- tinted background, both strings, signature block -- from
 * the cached copies. Called with fb_lock held from output, status and clear;
 * fbcon_init calls it once before fb_ready, when no other context can reach
 * the renderer yet, so a repaint never loses what the last caller set. */
static void fb_draw_status_locked(void) {
    uint32_t left_len = (uint32_t)strlen(fb_status_left);
    uint32_t right_len = (uint32_t)strlen(fb_status_right);
    uint32_t start;

    fb_fill(0, fb_status_y, fb_width, fb_height - fb_status_y, fb_status_bg);
    if (left_len > fb_cols) left_len = fb_cols;
    for (uint32_t i = 0; i < left_len; i++)
        fb_draw_glyph(i, fb_text_rows, fb_status_left[i],
                      fb_status_fg, fb_status_bg);
    /* The right string owns the end of the bar, so it wins a collision. */
    if (right_len > fb_cols - left_len) right_len = fb_cols - left_len;
    start = fb_cols - right_len;
    for (uint32_t i = 0; i < right_len; i++)
        fb_draw_glyph(start + i, fb_text_rows, fb_status_right[i],
                      fb_status_fg, fb_status_bg);
    fb_draw_signature();
}

/* --- scrolling --------------------------------------------------------- */

static void fb_scroll(void) {
    uint32_t row_bytes = fb_pitch * fb_cell_h;
    /* Move the text area up by one row; only the text area is copied so the
     * status bar below it is never touched. */
    uint32_t total = row_bytes * (fb_text_rows - 1u);
    uint8_t *dst = fb_base;
    uint8_t *src = dst + row_bytes;

    memmove(dst, src, total);
    for (uint32_t y = (fb_text_rows - 1u) * fb_cell_h;
         y < fb_text_rows * fb_cell_h; y++)
        for (uint32_t x = 0; x < fb_width; x++)
            fb_put_pixel(x, y, fb_bg);

    if (fb_shadowed) {
        memmove(fbcon_shadow, fbcon_shadow + fb_cols,
                fb_cols * (fb_text_rows - 1u));
        memset(fbcon_shadow + fb_cols * (fb_text_rows - 1u), ' ',
               fb_cols);
    }
    fb_cursor_y = fb_text_rows - 1u;
    fb_cursor_x = 0;
}

/* --- output ------------------------------------------------------------ */

static void fb_newline(void) {
    fb_cursor_x = 0;
    if (++fb_cursor_y >= fb_text_rows) fb_scroll();
}

static void fb_advance(void) {
    if (++fb_cursor_x >= fb_cols) fb_newline();
}

static void fb_putc_locked(char c) {
    fb_erase_cursor();

    if (c == '\n') {
        fb_newline();
    } else if (c == '\r') {
        fb_cursor_x = 0;
    } else if (c == '\b') {
        /* Move left only; the line editor overwrites with a space. */
        if (fb_cursor_x > 0) fb_cursor_x--;
    } else if (c == '\t') {
        /* Advance to the next 8-column stop, like the VGA text console. */
        do {
            fb_shadow_set(fb_cursor_x, fb_cursor_y, ' ');
            fb_draw_cell(fb_cursor_x, fb_cursor_y, ' ');
            fb_advance();
        } while (fb_cursor_x % 8u && fb_cursor_x != 0);
    } else {
        fb_shadow_set(fb_cursor_x, fb_cursor_y, c);
        fb_draw_cell(fb_cursor_x, fb_cursor_y, c);
        fb_advance();
    }

    fb_draw_cursor();
}

/* --- display_ops_t ----------------------------------------------------- */

static void fbcon_clear(uint8_t color) {
    irqflags_t flags;

    (void)color;
    if (!fb_ready) return;
    flags = spin_lock_irqsave(&fb_lock);
    fb_fill(0, 0, fb_width, fb_height, fb_bg);
    if (fb_shadowed)
        memset(fbcon_shadow, ' ', fb_cols * fb_rows);
    fb_draw_status_locked();
    fb_cursor_x = 0;
    fb_cursor_y = 0;
    fb_draw_cursor();
    spin_unlock_irqrestore(&fb_lock, flags);
}

static void fbcon_putc(char c) {
    irqflags_t flags;

    if (!fb_ready) return;
    if (c == '\0') return;
    flags = spin_lock_irqsave(&fb_lock);
    fb_putc_locked(c);
    spin_unlock_irqrestore(&fb_lock, flags);
}

/* Set the two halves of the status bar. A NULL half leaves that end as it is,
 * and both NULL is a no-op, so the ports can update only what they own. */
static void fbcon_status(const char *left, const char *right) {
    irqflags_t flags;

    if (!fb_ready || (!left && !right)) return;
    flags = spin_lock_irqsave(&fb_lock);
    if (left) fb_status_copy(fb_status_left, left);
    if (right) fb_status_copy(fb_status_right, right);
    fb_draw_status_locked();
    spin_unlock_irqrestore(&fb_lock, flags);
}

/* 1-based text cursor, used to answer a terminal cursor-position report. */
static void fbcon_cursor(uint32_t *row, uint32_t *col) {
    irqflags_t flags;

    if (!fb_ready) { *row = 1; *col = 1; return; }
    flags = spin_lock_irqsave(&fb_lock);
    *row = fb_cursor_y + 1u;
    *col = fb_cursor_x + 1u;
    spin_unlock_irqrestore(&fb_lock, flags);
}

const display_ops_t fbcon_display = {
    "framebuffer",
    fbcon_clear,
    fbcon_putc,
    fbcon_status,
    fbcon_cursor,
};

/* --- queries ----------------------------------------------------------- */

int fbcon_active(void) { return fb_ready; }
uint32_t fbcon_width(void) { return fb_width; }
uint32_t fbcon_height(void) { return fb_height; }
uint32_t fbcon_columns(void) { return fb_cols; }
uint32_t fbcon_rows(void) { return fb_text_rows; }
const char *fbcon_font_name(void) { return fb_font ? fb_font->name : "none"; }

/* --- init -------------------------------------------------------------- */

int fbcon_init(const fbcon_mode_t *mode) {
    if (fb_ready) return 1;
    if (!mode || !mode->base || !mode->font) return 0;
    if (mode->bytes_per_pixel != 4u && mode->bytes_per_pixel != 3u) return 0;
    if (!mode->width || !mode->height || !mode->pitch) return 0;
    if (!mode->font->width || !mode->font->height || !mode->font->stride)
        return 0;
    if (mode->font->width % 8u || !mode->font->glyphs) return 0;

    fb_base = (uint8_t *)mode->base;
    fb_width = mode->width;
    fb_height = mode->height;
    fb_pitch = mode->pitch;
    fb_bytes_per_pixel = mode->bytes_per_pixel;
    fb_font = mode->font;
    fb_cell_w = mode->font->width;
    fb_cell_h = mode->font->height;
    fb_cell_stride = mode->font->stride;

    fb_cols = fb_width / fb_cell_w;
    fb_rows = fb_height / fb_cell_h;
    if (!fb_cols || fb_rows < 3u) return 0;
    /* Two rows are reserved for the status bar at the bottom. */
    fb_text_rows = fb_rows - 2u;
    fb_status_y = fb_text_rows * fb_cell_h;

    fb_r_size = mode->red_size;     fb_r_pos = mode->red_pos;
    fb_g_size = mode->green_size;   fb_g_pos = mode->green_pos;
    fb_b_size = mode->blue_size;    fb_b_pos = mode->blue_pos;
    if (!fb_r_size) { fb_r_size = 8; fb_r_pos = 16; }
    if (!fb_g_size) { fb_g_size = 8; fb_g_pos = 8; }
    if (!fb_b_size) { fb_b_size = 8; fb_b_pos = 0; }

    /* A light console grey reads better than pure white on a bitmap font, and
     * the cursor shares the status colour so the two looked related. */
    fb_fg = compose(0xD0u, 0xD0u, 0xD0u);
    fb_bg = compose(0x08u, 0x08u, 0x10u);
    fb_status_fg = compose(0x40u, 0xC0u, 0xFFu);
    fb_status_bg = compose(0x14u, 0x16u, 0x28u);
    fb_cursor_fg = compose(0x40u, 0xC0u, 0xFFu);
    fb_signature = compose(0x20u, 0xA0u, 0xFFu);

    fb_shadowed = fb_cols <= FBCON_MAX_COLS && fb_rows <= FBCON_MAX_ROWS;

    fb_cursor_x = 0;
    fb_cursor_y = 0;
    fb_ready = 1;

    memset(fbcon_shadow, ' ', sizeof(fbcon_shadow));
    fb_fill(0, 0, fb_width, fb_height, fb_bg);
    fb_draw_status_locked();
    fb_draw_cursor();
    return 1;
}
