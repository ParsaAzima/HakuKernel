/* Haku x86-64 libc: formatted output.
 *
 * The 32-bit printf in user/lib/stdio.c walks the stack by casting
 * &format + 1 to unsigned int*, which only works because every i386 argument
 * is a stack word. x86-64 passes the first four arguments in RCX/RDX/R8/R9,
 * so this file uses the compiler's <stdarg.h> and va_list instead. The
 * format set is the same on purpose: %%, %c, %s, %d, %i, %u, %x, %X and %p,
 * with an optional '0', an optional width and the l/ll/h/hh/z length
 * modifiers. Floating point is not implemented.
 */
#include <haku/abi64.h>
#include "haku_stdarg.h"

#define PRINTF_CAPACITY 1024u

typedef struct {
    char *data;
    size_t used;
    size_t capacity;
} output_t;

static void emit(output_t *out, char ch) {
    if (out->capacity && out->used + 1u < out->capacity)
        out->data[out->used] = ch;
    out->used++;
}

static void emit_spaces(output_t *out, unsigned int count) {
    while (count--) emit(out, ' ');
}

static void emit_unsigned(output_t *out, unsigned long long value,
                          unsigned int base, int upper, unsigned int width,
                          int zero_pad) {
    char digits[32];
    const char *table = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned int count = 0;
    do {
        digits[count++] = table[value % base];
        value /= base;
    } while (value);
    while (width > count) {
        emit(out, zero_pad ? '0' : ' ');
        width--;
    }
    while (count) emit(out, digits[--count]);
}

static const char *format_output(output_t *out, const char *format,
                                 va_list args) {
    const char *cursor;

    for (cursor = format; *cursor; cursor++) {
        unsigned int width = 0;
        int zero_pad = 0;
        int long_count = 0;
        int size_modifier = 0;

        if (*cursor != '%') {
            emit(out, *cursor);
            continue;
        }
        cursor++;
        if (*cursor == '%') {
            emit(out, '%');
            continue;
        }
        if (*cursor == '0') {
            zero_pad = 1;
            cursor++;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            width = width * 10u + (unsigned int)(*cursor - '0');
            if (width > 200u) width = 200u;
            cursor++;
        }
        while (*cursor == 'l') {
            long_count++;
            cursor++;
        }
        if (*cursor == 'z') {
            size_modifier = 1;
            cursor++;
        } else if (*cursor == 'h') {
            cursor++;
            if (*cursor == 'h') cursor++;
        }
        switch (*cursor) {
        case 'c': {
            char ch = (char)va_arg(args, int);
            if (width > 1u) emit_spaces(out, width - 1u);
            emit(out, ch);
            break;
        }
        case 's': {
            const char *text = va_arg(args, const char *);
            size_t length;
            if (!text) text = "(null)";
            length = strlen(text);
            if (width > length)
                emit_spaces(out, width - (unsigned int)length);
            while (*text) emit(out, *text++);
            break;
        }
        case 'd':
        case 'i': {
            long long value;
            unsigned long long magnitude;
            if (long_count >= 2 || size_modifier)
                value = va_arg(args, long long);
            else if (long_count == 1)
                value = (long long)va_arg(args, long);
            else
                value = (long long)va_arg(args, int);
            if (value < 0) {
                emit(out, '-');
                if (width) width--;
                magnitude = 0ull - (unsigned long long)value;
            } else {
                magnitude = (unsigned long long)value;
            }
            emit_unsigned(out, magnitude, 10u, 0, width, zero_pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long value;
            if (long_count >= 2 || size_modifier)
                value = va_arg(args, unsigned long long);
            else if (long_count == 1)
                value = (unsigned long long)va_arg(args, unsigned long);
            else
                value = (unsigned long long)va_arg(args, unsigned int);
            if (*cursor == 'u')
                emit_unsigned(out, value, 10u, 0, width, zero_pad);
            else
                emit_unsigned(out, value, 16u, *cursor == 'X', width,
                              zero_pad);
            break;
        }
        case 'p': {
            unsigned long long value =
                (unsigned long long)va_arg(args, void *);
            emit(out, '0');
            emit(out, 'x');
            emit_unsigned(out, value, 16u, 0, 16u, 1);
            break;
        }
        case 0:
            emit(out, '%');
            cursor--;
            break;
        default:
            emit(out, '%');
            emit(out, *cursor);
            break;
        }
    }
    return cursor;
}

static int finish(output_t *out) {
    if (out->capacity)
        out->data[out->used < out->capacity ? out->used
                                           : out->capacity - 1u] = 0;
    return (int)(out->used < 0x7FFFFFFFu ? out->used : 0x7FFFFFFFu);
}

int putchar(int ch) {
    char byte = (char)ch;
    if (write(1, &byte, 1) != 1) return -1;
    return (unsigned char)byte;
}

int puts(const char *text) {
    size_t length = strlen(text);
    if (write(1, text, length) != (ssize_t)length) return -1;
    if (write(1, "\n", 1) != 1) return -1;
    return (int)(length + 1u);
}

int printf(const char *format, ...) {
    char buffer[PRINTF_CAPACITY];
    output_t out;
    va_list args;
    int length;

    out.data = buffer;
    out.used = 0;
    out.capacity = sizeof(buffer);
    va_start(args, format);
    format_output(&out, format, args);
    va_end(args);
    length = finish(&out);
    if (length > 0 && write(1, buffer, (size_t)length) != length) return -1;
    return length;
}

int snprintf(char *buffer, size_t capacity, const char *format, ...) {
    output_t out;
    va_list args;

    out.data = buffer;
    out.used = 0;
    out.capacity = capacity;
    va_start(args, format);
    format_output(&out, format, args);
    va_end(args);
    return finish(&out);
}
