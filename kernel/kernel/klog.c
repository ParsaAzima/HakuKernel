/* Haku klog implementation: serial-mirrored terminal emit with PIT
 * timestamps. The terminal sends each byte to the active display and COM1,
 * so there is a single output path.
 *
 * A log line is assembled here and handed to the terminal in one piece, so
 * a kernel log line and a user write never land inside each other: the
 * markers the headless check looks for stay contiguous in the serial log. */
#include "klog.h"
#include "pit.h"
#include "terminal.h"

static klog_level_t log_threshold = KLOG_INFO;
/* The display mirror. The boot path clears it when the console is handed to
 * userland, so kernel log lines keep going to the serial log without writing
 * over the shell; panic() turns it back on. */
static int mirror_to_display = 1;

static const char *const level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "CRIT"
};

#define KLOG_LINE_MAX 512u
static char line_buffer[KLOG_LINE_MAX];
static uint32_t line_used;

static void line_append_char(char c) {
    if (line_used + 1u < KLOG_LINE_MAX) line_buffer[line_used++] = c;
    if (c == '\n') {
        line_buffer[line_used] = '\0';
        if (mirror_to_display) terminal_write(line_buffer);
        else terminal_write_serial(line_buffer);
        line_used = 0;
    }
}

static void line_append(const char *s) {
    while (*s) line_append_char(*s++);
}

void klog_putc(char c) {
    line_append_char(c);
}

void klog_write(const char *s) {
    line_append(s);
}

void klog_dec(uint32_t v) {
    char rev[10];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < 10);
    while (n--) line_append_char(rev[n]);
}

void klog_hex(uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    line_append("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        line_append_char(digits[(v >> shift) & 0xF]);
}

void klog_hex_raw(uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4)
        line_append_char(digits[(v >> shift) & 0xF]);
}

static void emit_padded(uint32_t v, int width, char pad) {
    char rev[10];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < 10);
    for (int i = n; i < width; i++)
        line_append_char(pad);
    while (n--) line_append_char(rev[n]);
}

void klog_prefix(void) {
    /* Linux "[%5u.%06u] ": space-padded seconds, zero-padded micros. */
    uint32_t ticks = pit_ticks();
    klog_write("[");
    emit_padded(ticks / 100u, 5, ' ');
    klog_write(".");
    emit_padded((ticks % 100u) * 10000u, 6, '0');
    klog_write("] ");
}

/* --- leveled logging --------------------------------------------------- */

void klog_set_level(klog_level_t level) {
    log_threshold = level;
}

/* Route log lines to COM1 only while disabled. usermode_run() disables the
 * mirror when it hands the console to the first user task; panic() enables
 * it again. */
void klog_mirror_display(int enabled) {
    mirror_to_display = enabled ? 1 : 0;
}

klog_level_t klog_get_level(void) {
    return log_threshold;
}

const char *klog_level_name(klog_level_t level) {
    if (level < KLOG_DEBUG || level > KLOG_CRIT) return "NONE";
    return level_names[level];
}

int klog_level_enabled(klog_level_t level) {
    return level >= log_threshold && level <= KLOG_CRIT;
}

int klog_begin(klog_level_t level) {
    if (!klog_level_enabled(level)) return 0;
    klog_prefix();
    klog_write(klog_level_name(level));
    klog_write(": ");
    return 1;
}

void klog_log(klog_level_t level, const char *message) {
    if (klog_begin(level)) klog_write(message);
}

void klog_self_test(void) {
    int hidden = klog_begin(KLOG_DEBUG); /* INFO threshold: must be 0 */
    int shown;

    klog_set_level(KLOG_DEBUG);
    shown = klog_begin(KLOG_DEBUG);
    if (shown) klog_write("KLOG-LEVELS-OK: debug output gated by level\n");
    klog_set_level(KLOG_INFO);

    if (hidden || !shown) {
        klog_log(KLOG_ERROR, "KLOG-LEVELS-FAIL: level filtering broken\n");
    }
}
