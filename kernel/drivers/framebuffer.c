/* Haku framebuffer driver: text console on a VBE linear framebuffer.
 *
 * stage 2 discovers a linear-framebuffer mode with the VBE BIOS calls and
 * leaves a handover record at physical 0x6FC0 (see boot/stage2.asm). This
 * driver maps that aperture into the supervisor window PAGING_FB_WINDOW,
 * copies the 8x16 VGA font that stage 2 saved at 0x9000, and renders the
 * console. If the handover is missing or unusable the driver stays
 * inactive and the console keeps the VGA text buffer as its fallback.
 */
#include "framebuffer.h"
#include "klog.h"
#include "frame.h"
#include "paging.h"
#include "sync.h"

#define FB_HANDOVER_FIELDS 11

typedef struct {
    uint32_t magic;
    uint32_t physical_base;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint16_t red;         /* size | position << 8 */
    uint16_t green;
    uint16_t blue;
    uint16_t mode;
    uint32_t size;        /* pitch * height */
} fb_handover_t;

static uint8_t fb_font[FB_FONT_BYTES];
static int fb_ready;
static uint32_t fb_base;          /* virtual address of pixel (0, 0) */
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;
static uint32_t fb_bytes_per_pixel;
static uint32_t fb_cols;
static uint32_t fb_text_rows;     /* rows used by the scrolling text area */
static uint32_t fb_status_y;      /* first row of the status bar */
static uint32_t fb_cursor_x;
static uint32_t fb_cursor_y;
static uint32_t fb_fg;
static uint32_t fb_bg;
static uint32_t fb_status_fg;
static uint32_t fb_signature;     /* signature block colour for the tests */
static uint32_t fb_r_size;
static uint32_t fb_r_pos;
static uint32_t fb_g_size;
static uint32_t fb_g_pos;
static uint32_t fb_b_size;
static uint32_t fb_b_pos;
static spinlock_t fb_lock = SPINLOCK_INIT;

/* --- colour helpers --------------------------------------------------- */

/* Scale an 8-bit channel into the mask the mode reported (Linux keeps
 * the mask size and the field position separately, like screen_info). */
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

/* --- pixel helpers ---------------------------------------------------- */

static void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    uint32_t offset;
    if (x >= fb_width || y >= fb_height) return;
    offset = y * fb_pitch + x * fb_bytes_per_pixel;
    if (fb_bytes_per_pixel == 4) {
        *(uint32_t *)(fb_base + offset) = color;
    } else {
        /* 24 bpp: the pixel is stored blue, green, red */
        uint8_t *pixel = (uint8_t *)(fb_base + offset);
        pixel[0] = (uint8_t)(color & 0xFFu);
        pixel[1] = (uint8_t)((color >> 8) & 0xFFu);
        pixel[2] = (uint8_t)((color >> 16) & 0xFFu);
    }
}

static void fb_fill(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                    uint32_t color) {
    for (uint32_t y = y0; y < y0 + h && y < fb_height; y++)
        for (uint32_t x = x0; x < x0 + w && x < fb_width; x++)
            fb_put_pixel(x, y, color);
}

/* The signature block is the handshake the headless screendump checks for.
 * It lives in the unused second status row at the far right, so neither the
 * text area nor the status text can overwrite it. */
static void fb_draw_signature(void) {
    if (fb_status_y + FB_CHAR_HEIGHT + 2u <= fb_height && fb_width >= 16u)
        fb_fill(fb_width - 16u, fb_status_y + FB_CHAR_HEIGHT, 16u, 2u,
                fb_signature);
}

/* --- text -------------------------------------------------------------- */

static void fb_draw_glyph(uint32_t col, uint32_t row, char c) {
    const uint8_t *glyph = &fb_font[(uint8_t)c * FB_CHAR_HEIGHT];
    uint32_t x0 = col * FB_CHAR_WIDTH;
    uint32_t y0 = row * FB_CHAR_HEIGHT;

    for (uint32_t gy = 0; gy < FB_CHAR_HEIGHT; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < FB_CHAR_WIDTH; gx++) {
            uint32_t on = (bits >> (7u - gx)) & 1u;
            fb_put_pixel(x0 + gx, y0 + gy, on ? fb_fg : fb_bg);
        }
    }
}

static void fb_scroll(void) {
    uint32_t row_bytes = fb_pitch * FB_CHAR_HEIGHT;
    /* Move the text area up by one row; only the text area is copied so
     * the status bar below it is never touched. */
    uint32_t total = row_bytes * (fb_text_rows - 1u);
    uint8_t *dst = (uint8_t *)fb_base;
    uint8_t *src = dst + row_bytes;

    memmove(dst, src, total);
    for (uint32_t y = (fb_text_rows - 1u) * FB_CHAR_HEIGHT;
         y < fb_text_rows * FB_CHAR_HEIGHT; y++)
        for (uint32_t x = 0; x < fb_width; x++)
            fb_put_pixel(x, y, fb_bg);
    fb_cursor_y = fb_text_rows - 1u;
    fb_cursor_x = 0;
}

/* --- the VGA font ----------------------------------------------------- */

/* The classic IBM 8x16 VGA font. Its 'A' glyph is used as a signature
 * when the BIOS ROM area is searched for the table, the way Linux copies
 * the font out of the video BIOS in copy_font(). */
static const uint8_t fb_font_signature[FB_CHAR_HEIGHT] = {
    0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xFE,
    0xC6, 0xC6, 0xC6, 0xC6, 0x00, 0x00, 0x00, 0x00
};

/* Search the BIOS ROM area in physical memory for the 8x16 font table.
 * The table is copied into the driver so it stays valid after the boot
 * code is gone. Returns 1 when the font was found. */
static int fb_find_font(uint8_t *dest) {
    uint32_t glyph_offset = 0x41u * FB_CHAR_HEIGHT;   /* the 'A' glyph */
    uint32_t start = 0xC0000u;                        /* option ROM area */
    uint32_t end = 0xE0000u;

    for (uint32_t base = start; base < end; base++) {
        const uint8_t *probe =
            (const uint8_t *)PHYS_TO_VIRT(base + glyph_offset);
        if (memcmp(probe, fb_font_signature, FB_CHAR_HEIGHT) != 0)
            continue;
        memcpy(dest, (const void *)PHYS_TO_VIRT(base), FB_FONT_BYTES);
        return 1;
    }
    return 0;
}

/* --- public API -------------------------------------------------------- */

int framebuffer_active(void) {
    return fb_ready;
}

uint32_t framebuffer_width(void) {
    return fb_width;
}

uint32_t framebuffer_height(void) {
    return fb_height;
}

void framebuffer_putc(char c) {
    irqflags_t flags;

    if (!fb_ready) return;
    flags = spin_lock_irqsave(&fb_lock);
    if (c == '\n') {
        fb_cursor_x = 0;
        if (++fb_cursor_y >= fb_text_rows) fb_scroll();
        spin_unlock_irqrestore(&fb_lock, flags);
        return;
    }
    if (c == '\r') {
        fb_cursor_x = 0;
        spin_unlock_irqrestore(&fb_lock, flags);
        return;
    }
    if (c == '\b') {
        /* Move left only; the line editor overwrites with a space. */
        if (fb_cursor_x > 0) fb_cursor_x--;
        spin_unlock_irqrestore(&fb_lock, flags);
        return;
    }
    fb_draw_glyph(fb_cursor_x, fb_cursor_y, c);
    if (++fb_cursor_x >= fb_cols) {
        fb_cursor_x = 0;
        if (++fb_cursor_y >= fb_text_rows) fb_scroll();
    }
    spin_unlock_irqrestore(&fb_lock, flags);
}

/* Draw a short status string right-aligned in the status bar. */
void framebuffer_status(const char *text) {
    irqflags_t flags;
    size_t len;
    uint32_t start;

    if (!fb_ready || !text) return;
    len = strlen(text);
    if (len > fb_cols) len = fb_cols;
    start = fb_cols - (uint32_t)len;
    flags = spin_lock_irqsave(&fb_lock);
    for (size_t i = 0; i < len; i++) {
        const uint8_t *glyph = &fb_font[(uint8_t)text[i] * FB_CHAR_HEIGHT];
        uint32_t x0 = (start + (uint32_t)i) * FB_CHAR_WIDTH;
        for (uint32_t gy = 0; gy < FB_CHAR_HEIGHT; gy++) {
            uint8_t bits = glyph[gy];
            for (uint32_t gx = 0; gx < FB_CHAR_WIDTH; gx++) {
                uint32_t on = (bits >> (7u - gx)) & 1u;
                fb_put_pixel(x0 + gx, fb_status_y + gy, on ? fb_status_fg : fb_bg);
            }
        }
    }
    spin_unlock_irqrestore(&fb_lock, flags);
}

/* display.h clear backend: the framebuffer owns its own palette. */
static void fb_console_clear(uint8_t color) {
    irqflags_t flags;

    (void)color;
    if (!fb_ready) return;
    flags = spin_lock_irqsave(&fb_lock);
    fb_fill(0, 0, fb_width, fb_height, fb_bg);
    fb_draw_signature();
    fb_cursor_x = 0;
    fb_cursor_y = 0;
    spin_unlock_irqrestore(&fb_lock, flags);
}

/* 1-based text cursor, used to answer a terminal cursor-position report. */
static void framebuffer_cursor(uint32_t *row, uint32_t *col) {
    irqflags_t flags;

    if (!fb_ready) { *row = 1; *col = 1; return; }
    flags = spin_lock_irqsave(&fb_lock);
    *row = fb_cursor_y + 1u;
    *col = fb_cursor_x + 1u;
    spin_unlock_irqrestore(&fb_lock, flags);
}

const display_ops_t framebuffer_display = {
    "framebuffer",
    fb_console_clear,
    framebuffer_putc,
    framebuffer_status,
    framebuffer_cursor,
};

int framebuffer_init(void) {
    const fb_handover_t *handover =
        (const fb_handover_t *)(uint32_t)FB_HANDOVER_PHYS;
    uint32_t virtual_base;

    if (fb_ready) return 1;
    if (handover->magic != FB_HANDOVER_MAGIC) return 0;
    if (handover->bpp != 32u && handover->bpp != 24u) return 0;
    if (!handover->width || !handover->height) return 0;
    if (handover->width > 4096u || handover->height > 4096u) return 0;
    if (!handover->physical_base || !handover->pitch) return 0;

    fb_r_size = handover->red & 0xFFu;
    fb_r_pos = (handover->red >> 8) & 0xFFu;
    fb_g_size = handover->green & 0xFFu;
    fb_g_pos = (handover->green >> 8) & 0xFFu;
    fb_b_size = handover->blue & 0xFFu;
    fb_b_pos = (handover->blue >> 8) & 0xFFu;
    if (!fb_r_size) { fb_r_size = 8; fb_r_pos = 16; }
    if (!fb_g_size) { fb_g_size = 8; fb_g_pos = 8; }
    if (!fb_b_size) { fb_b_size = 8; fb_b_pos = 0; }

    /* The aperture must not overlap the tracked frame pool. */
    frame_reserve_range(handover->physical_base,
                        handover->physical_base + handover->size);

    virtual_base = paging_map_framebuffer(handover->physical_base,
                                           handover->size);
    if (!virtual_base) return 0;

    fb_base = virtual_base;
    fb_width = handover->width;
    fb_height = handover->height;
    fb_pitch = handover->pitch;
    fb_bytes_per_pixel = handover->bpp / 8u;
    fb_cols = fb_width / FB_CHAR_WIDTH;
    if (!fb_cols) return 0;
    /* Two rows are reserved for the status bar at the bottom. */
    fb_text_rows = fb_height / FB_CHAR_HEIGHT;
    if (fb_text_rows > 2u) fb_text_rows -= 2u;
    if (!fb_text_rows) return 0;
    fb_status_y = fb_text_rows * FB_CHAR_HEIGHT;

    fb_fg = compose(0xC0u, 0xC0u, 0xC0u);
    fb_bg = compose(0x00u, 0x00u, 0x00u);
    fb_status_fg = compose(0x40u, 0xC0u, 0xFFu);
    fb_signature = compose(0x20u, 0xA0u, 0xFFu);

    /* Copy the 8x16 font out of the BIOS ROM area (Linux's copy_font()). */
    if (!fb_find_font(fb_font)) return 0;

    fb_cursor_x = 0;
    fb_cursor_y = 0;
    fb_ready = 1;

    klog_prefix();
    klog_write("fb: ");
    klog_dec(fb_width);
    klog_write("x");
    klog_dec(fb_height);
    klog_write(" pitch=");
    klog_dec(fb_pitch);
    klog_write(" bpp=");
    klog_dec(fb_bytes_per_pixel);
    klog_write(" font=8x16\n");


    fb_fill(0, 0, fb_width, fb_height, fb_bg);
    fb_draw_signature();
    return 1;
}
