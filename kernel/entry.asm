; =============================================================================
; MyOS v0.1 — 32-bit kernel entry stub (NASM -> COFF for win32, ELF for linux)
;
; Linked FIRST on the link command line so the image starts here.
; On Windows MSVC prepends '_' to C symbols; NASM's bare `_entry` matches
; that. On Linux GCC uses bare names; `entry` matches that.
;
; On Linux the flat binary is produced with `objcopy -O binary`, which
; does NOT emit the .bss section (NOBITS). So we zero .bss ourselves here
; before calling into C — otherwise every uninitialised static (IDT,
; kbd ring buffer, ...) starts as garbage.
; =============================================================================

[bits 32]

%ifdef WIN32
  %define ENTRY _entry
  %define KMAIN _kernel_main
%else
  %define ENTRY entry
  %define KMAIN kernel_main
%endif

global ENTRY
extern KMAIN

section .text

ENTRY:
%ifndef WIN32
    ; ---- zero .bss (Linux only — objcopy drops NOBITS sections) ----
    extern __bss_start
    extern __bss_end
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    shr ecx, 2          ; dword count
    xor eax, eax
    rep stosd
%endif

    call KMAIN
.hang:
    hlt
    jmp .hang
