/* libhakumod: the freestanding C runtime a Haku module links against.
 *
 * A module image is packed from one or more COFF objects, so the compiler's
 * own support routines have to come from somewhere in that image: this file
 * provides the string routines and the 64-bit integer helpers i386 code
 * calls, and runtime.asm provides the stack probes and the 64-bit shift
 * helpers whose calling convention (count in cl, value in edx:eax) C cannot
 * express. The packer lays out only
 * the sections the module's entry point can reach, so a module that never
 * divides a 64-bit value does not carry the division helper.
 *
 * Everything here uses 32-bit operations: a 64-bit multiply or divide would
 * compile into a call to the very helper being defined. Floating point is
 * deliberately absent — the kernel does not save x87 or SSE state across a
 * task switch, so a module must not touch either.
 */
#include "haku/module.h"

/* ---- string routines ------------------------------------------------- */

void *memset(void *destination, int value, unsigned int count) {
    unsigned char *bytes = (unsigned char *)destination;
    unsigned char byte = (unsigned char)value;
    haku_u32 word = (haku_u32)byte * 0x01010101u;
    while (count && ((haku_u64)bytes & 3u)) {
        *bytes++ = byte;
        count--;
    }
    while (count >= 4) {
        *(haku_u32 *)bytes = word;
        bytes += 4;
        count -= 4;
    }
    while (count--) *bytes++ = byte;
    return destination;
}

void *memcpy(void *destination, const void *source, unsigned int count) {
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;
    if ((((haku_u64)to ^ (haku_u64)from) & 3u) == 0) {
        while (count && ((haku_u64)to & 3u)) {
            *to++ = *from++;
            count--;
        }
        while (count >= 4) {
            *(haku_u32 *)to = *(const haku_u32 *)from;
            to += 4;
            from += 4;
            count -= 4;
        }
    }
    while (count--) *to++ = *from++;
    return destination;
}

void *memmove(void *destination, const void *source, unsigned int count) {
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;
    if (to < from) return memcpy(destination, source, count);
    if (to > from) {
        to += count;
        from += count;
        while (count--) *--to = *--from;
    }
    return destination;
}

unsigned int strlen(const char *text) {
    const char *end = text;
    while (*end) end++;
    return (unsigned int)(end - text);
}

int strcmp(const char *left, const char *right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

/* ---- 64-bit helpers --------------------------------------------------
 *
 * The compiler calls these by their documented runtime names, `__allmul`
 * and so on, and emits those names verbatim: unlike an ordinary C
 * function, a helper reference is not decorated a second time. Defining
 * `__allmul` in C would therefore produce `___allmul`, one underscore too
 * many, so each helper is spelled `_allmul` here and the C decoration puts
 * exactly `__allmul` in the symbol table.
 */

static haku_u32 high_word(haku_u64 value) {
    return (haku_u32)(value >> 32);
}

static haku_u32 low_word(haku_u64 value) {
    return (haku_u32)value;
}

static haku_u64 join_words(haku_u32 high, haku_u32 low) {
    return ((haku_u64)high << 32) | (haku_u64)low;
}

/* Low 64 bits of the product, computed from 16-bit limbs so no partial
 * product ever needs a 64-bit multiply. O(1): sixteen 16x16 multiplies. */
haku_i64 _allmul(haku_i64 left, haku_i64 right) {
    haku_u32 a[4], b[4], product[4];
    haku_u32 l_hi = high_word((haku_u64)left);
    haku_u32 l_lo = low_word((haku_u64)left);
    haku_u32 r_hi = high_word((haku_u64)right);
    haku_u32 r_lo = low_word((haku_u64)right);
    int i, j;

    a[0] = l_lo & 0xFFFFu;
    a[1] = l_lo >> 16;
    a[2] = l_hi & 0xFFFFu;
    a[3] = l_hi >> 16;
    b[0] = r_lo & 0xFFFFu;
    b[1] = r_lo >> 16;
    b[2] = r_hi & 0xFFFFu;
    b[3] = r_hi >> 16;
    product[0] = product[1] = product[2] = product[3] = 0;
    for (i = 0; i < 4; i++) {
        haku_u32 carry = 0;
        for (j = 0; i + j < 4; j++) {
            haku_u32 sum = product[i + j] + a[i] * b[j] + carry;
            product[i + j] = sum & 0xFFFFu;
            carry = sum >> 16;
        }
    }
    return (haku_i64)join_words(product[2] | (product[3] << 16),
                                product[0] | (product[1] << 16));
}

static haku_u64 unsigned_abs(haku_i64 value) {
    return value < 0 ? (haku_u64)0 - (haku_u64)value : (haku_u64)value;
}

/* Restoring division, one bit per iteration (64 iterations, O(1)). The
 * carry out of the remainder shift is what keeps divisors with the top bit
 * set correct; the remainder stays below the divisor otherwise. A zero
 * divisor cannot raise #DE from C, so it yields an all-ones quotient and
 * the numerator as remainder instead of looping forever. */
static void divide64(haku_u64 numerator, haku_u64 denominator,
                    haku_u64 *quotient, haku_u64 *remainder) {
    haku_u32 n_hi = high_word(numerator);
    haku_u32 n_lo = low_word(numerator);
    haku_u32 d_hi = high_word(denominator);
    haku_u32 d_lo = low_word(denominator);
    haku_u32 q_hi = 0;
    haku_u32 q_lo = 0;
    haku_u32 r_hi = 0;
    haku_u32 r_lo = 0;
    int bit;

    if (d_hi == 0 && d_lo == 0) {
        *quotient = ~(haku_u64)0;
        *remainder = numerator;
        return;
    }
    for (bit = 63; bit >= 0; bit--) {
        haku_u32 incoming = bit >= 32 ? (n_hi >> (bit - 32)) & 1u
                                      : (n_lo >> bit) & 1u;
        haku_u32 carry = r_hi >> 31;
        haku_u32 borrow;
        r_hi = (r_hi << 1) | (r_lo >> 31);
        r_lo = (r_lo << 1) | incoming;
        q_hi = (q_hi << 1) | (q_lo >> 31);
        q_lo <<= 1;
        if (carry || r_hi > d_hi || (r_hi == d_hi && r_lo >= d_lo)) {
            borrow = r_lo < d_lo;
            r_lo -= d_lo;
            r_hi = r_hi - d_hi - borrow;
            q_lo |= 1u;
        }
    }
    *quotient = join_words(q_hi, q_lo);
    *remainder = join_words(r_hi, r_lo);
}

haku_u64 _aulldiv(haku_u64 numerator, haku_u64 denominator) {
    haku_u64 quotient;
    haku_u64 remainder;
    divide64(numerator, denominator, &quotient, &remainder);
    return quotient;
}

haku_u64 _aullrem(haku_u64 numerator, haku_u64 denominator) {
    haku_u64 quotient;
    haku_u64 remainder;
    divide64(numerator, denominator, &quotient, &remainder);
    return remainder;
}

haku_i64 _alldiv(haku_i64 numerator, haku_i64 denominator) {
    haku_u64 quotient;
    haku_u64 remainder;
    int negative = (numerator < 0) != (denominator < 0);
    divide64(unsigned_abs(numerator), unsigned_abs(denominator),
             &quotient, &remainder);
    return negative ? -(haku_i64)quotient : (haku_i64)quotient;
}

haku_i64 _allrem(haku_i64 numerator, haku_i64 denominator) {
    haku_u64 quotient;
    haku_u64 remainder;
    divide64(unsigned_abs(numerator), unsigned_abs(denominator),
             &quotient, &remainder);
    return numerator < 0 ? -(haku_i64)remainder : (haku_i64)remainder;
}
