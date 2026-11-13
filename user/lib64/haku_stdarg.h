/* Minimal C varargs for the x86-64 user library.
 *
 * The user library is freestanding and built with /NODEFAULTLIB, so it does
 * not include the hosted <stdarg.h>: that header lives in the toolchain's
 * own include directory, which this build does not add (the kernel build has
 * the same rule). Instead this file provides the two implementations the
 * supported compilers use on x86-64:
 *
 *   MSVC (cl.exe)  - the undocumented-but-stable __va_start intrinsic; this
 *                    is exactly what the shipped <vadefs.h> expands to.
 *   clang-cl       - the __builtin_va_list family.
 *
 * Both assume the 64-bit ABI where a va_list advances by eight bytes per
 * argument. Only printf/snprintf use it, and only with integer, pointer and
 * string arguments; there is no floating-point support.
 */
#ifndef HAKU_STDARG_H
#define HAKU_STDARG_H

#if defined(__clang__)
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dst, src) __builtin_va_copy(dst, src)
#elif defined(_MSC_VER)
typedef char *va_list;
void __cdecl __va_start(va_list *, ...);
#define va_start(ap, last) ((void)__va_start(&(ap), last))
#define va_arg(ap, type) (*(type *)((ap += 8) - 8))
#define va_end(ap) ((void)0)
#define va_copy(dst, src) ((dst) = (src))
#else
#error "haku_stdarg.h: unsupported compiler for the x86-64 user library"
#endif

#endif
