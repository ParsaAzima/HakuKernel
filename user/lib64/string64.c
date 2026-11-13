/* Haku x86-64 libc: freestanding string routines.
 *
 * The 32-bit implementations in user/lib/string.c are already pointer-width
 * neutral, so this file is the same code compiled with 64-bit size_t. It is
 * a separate translation unit rather than a shared source so the two
 * architectures never disagree about a type in the public header.
 */
#include <haku/abi64.h>

void *memcpy(void *dst, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (count--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (count--) *d++ = *s++;
    } else if (d > s) {
        d += count;
        s += count;
        while (count--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int value, size_t count) {
    unsigned char *d = (unsigned char *)dst;
    while (count--) *d++ = (unsigned char)value;
    return dst;
}

int memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    while (count--) {
        if (*a != *b) return (int)*a - (int)*b;
        a++;
        b++;
    }
    return 0;
}

void *memchr(const void *block, int value, size_t count) {
    const unsigned char *p = (const unsigned char *)block;
    while (count--) {
        if (*p == (unsigned char)value) return (void *)p;
        p++;
    }
    return NULL;
}

size_t strlen(const char *text) {
    size_t length = 0;
    while (text[length]) length++;
    return length;
}

char *strcpy(char *dst, const char *src) {
    char *result = dst;
    while ((*dst++ = *src++) != 0) {
    }
    return result;
}

char *strncpy(char *dst, const char *src, size_t count) {
    char *result = dst;
    while (count && *src) {
        *dst++ = *src++;
        count--;
    }
    while (count--) *dst++ = 0;
    return result;
}

char *strcat(char *dst, const char *src) {
    char *result = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++) != 0) {
    }
    return result;
}

char *strncat(char *dst, const char *src, size_t count) {
    char *result = dst;
    while (*dst) dst++;
    while (count && *src) {
        *dst++ = *src++;
        count--;
    }
    *dst = 0;
    return result;
}

int strcmp(const char *left, const char *right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count) {
    while (count && *left && *left == *right) {
        left++;
        right++;
        count--;
    }
    if (count == 0) return 0;
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

char *strchr(const char *text, int ch) {
    char wanted = (char)ch;
    for (;;) {
        if (*text == wanted) return (char *)text;
        if (*text == 0) return NULL;
        text++;
    }
}

char *strrchr(const char *text, int ch) {
    const char *found = NULL;
    char wanted = (char)ch;
    for (;;) {
        if (*text == wanted) found = text;
        if (*text == 0) return (char *)found;
        text++;
    }
}

char *strstr(const char *text, const char *needle) {
    size_t length;
    if (!*needle) return (char *)text;
    length = strlen(needle);
    while (*text) {
        if (*text == *needle && !strncmp(text, needle, length))
            return (char *)text;
        text++;
    }
    return NULL;
}
