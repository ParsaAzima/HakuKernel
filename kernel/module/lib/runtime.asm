; libhakumod's assembly half: the small stubs the i386 compiler expects at
; fixed, undecorated symbol names.
;
; Two families live here:
;
;   * Stack probes. The compiler calls one of these when a function's frame
;     is larger than a page, so the guard page below the stack is touched in
;     order instead of being jumped over. Haku's kernel stack is two 4 KiB
;     pages, which means a frame anywhere near this size is already a design
;     mistake, but a probe that faults on the guard page is far easier to
;     diagnose than a frame that silently runs off the bottom.
;
;   * 64-bit shift helpers. `cl` holds the count and the value arrives in
;     edx:eax, which is not a cdecl signature and so cannot be written in C.
;     Both shifts by 64 or more and negative counts follow the hardware
;     convention: the result is zero, or the sign, rather than a masked
;     count. The compiler's own names for these are `__allshl`,
;     `__aullshr`, and `__allshr`; NASM emits a symbol verbatim, so no C
;     decoration gets in the way.
;
;   __chkstk      MSVC's stack probe: probes the new frame and then allocates
;                 it, so the caller emits only `mov eax, size` / `call`.
;   ___chkstk_ms  clang's stack probe: probes only; the caller subtracts the
;                 size after the call returns.
;
; Nothing here touches a callee-saved register.

bits 32

section .text

global __chkstk
global ___chkstk_ms
global __allshl
global __aullshr
global __allshr

; The return address is at [esp]. Pop it, probe from the bottom of the frame
; upward to the current stack, then allocate the frame and return through the
; saved address. Every page the frame will use has been read once.
__chkstk:
    pop     edx                 ; return address
    mov     ecx, esp            ; top of the frame the caller asked for
    sub     ecx, eax            ; lowest byte it will touch
    and     ecx, 0xFFFFF000     ; start at the containing page
.probe:
    test    byte [ecx], 0       ; fault here if the page is not mapped
    add     ecx, 4096
    cmp     ecx, esp
    jb      .probe
    sub     esp, eax
    jmp     edx

; No allocation here: the caller subtracts eax itself. esp is unchanged for
; the caller, so the probe range starts just below the future frame bottom.
___chkstk_ms:
    push    eax
    push    ecx
    mov     ecx, esp
    sub     ecx, eax
    and     ecx, 0xFFFFF000
.probe:
    test    byte [ecx], 0
    add     ecx, 4096
    cmp     ecx, esp
    jb      .probe
    pop     ecx
    pop     eax
    ret

; edx:eax << cl
__allshl:
    cmp     cl, 64
    jb      .split
    xor     eax, eax
    xor     edx, edx
    ret
.split:
    cmp     cl, 32
    jb      .small
    mov     edx, eax            ; low word moves into the high word
    xor     eax, eax
    sub     cl, 32
    shl     edx, cl
    ret
.small:
    shld    edx, eax, cl        ; count 0 is a no-op for shld
    shl     eax, cl
    ret

; edx:eax >> cl, unsigned
__aullshr:
    cmp     cl, 64
    jb      .split
    xor     eax, eax
    xor     edx, edx
    ret
.split:
    cmp     cl, 32
    jb      .small
    mov     eax, edx            ; high word moves into the low word
    xor     edx, edx
    sub     cl, 32
    shr     eax, cl
    ret
.small:
    shrd    eax, edx, cl        ; count 0 is a no-op for shrd
    shr     edx, cl
    ret

; edx:eax >> cl, signed
__allshr:
    cmp     cl, 64
    jb      .split
    sar     edx, 31             ; fill both words with the sign
    mov     eax, edx
    ret
.split:
    cmp     cl, 32
    jb      .small
    mov     eax, edx
    sar     edx, 31
    sub     cl, 32
    sar     eax, cl
    ret
.small:
    shrd    eax, edx, cl
    sar     edx, cl
    ret
