; libhakumod's x86-64 assembly half.
;
; The 32-bit runtime carries the stack probes *and* the 64-bit shift helpers
; (count in cl, value in edx:eax). Long mode has both halves of a 64-bit
; value in one register, so the compiler shifts inline and only the stack
; probe remains:
;
;   __chkstk      MSVC's probe. The caller loads the frame size in RAX and
;                 subtracts it itself after the call returns, so this routine
;                 touches one byte in every page the future frame will use and
;                 leaves RSP, RAX and every callee-saved register untouched.
;   ___chkstk_ms  clang's spelling of the same contract; provided so a module
;                 built by either compiler links.
;
; A frame smaller than one page cannot skip a guard page, so it needs no
; probe at all. Frames larger than a page are a design mistake on the 8 KiB
; kernel stack, but a module that makes one should fault on the guard page
; rather than run off the stack silently.

bits 64

section .text

global __chkstk
global ___chkstk_ms

__chkstk:
___chkstk_ms:
    cmp     rax, 4096
    jb      .done
    mov     r11, rsp
    sub     r11, rax
    and     r11, ~0xFFF
.probe:
    test    byte [r11], 0
    add     r11, 4096
    cmp     r11, rsp
    jb      .probe
.done:
    ret
