; =============================================================================
; MyOS v0.1 — interrupt stubs (NASM -> COFF for win32, ELF for linux)
;
; Uniform stack frame per vector: [dummy error code unless CPU pushed one]
; + vector number. isr_common saves everything, calls the C dispatcher
; with a regs_t*, restores and irets. All gates are interrupt gates,
; so IF is already clear — the leading cli is belt and braces.
;
; Symbol naming: on Windows MSVC C functions are `_name`; on Linux they
; are `name`. GSYM() picks the right prefix from the -DWIN32 define that
; build.py passes to nasm on Windows.
; =============================================================================

[bits 32]

%ifdef WIN32
  %define GSYM(name) _ %+ name
%else
  %define GSYM(name) name
%endif

global GSYM(idt_load)
global GSYM(cpu_sti)
global GSYM(cpu_cli)
extern GSYM(isr_handler)
extern GSYM(irq_handler)

section .text

GSYM(idt_load):
    mov eax, [esp+4]
    lidt [eax]
    ret

GSYM(cpu_sti):
    sti
    ret

GSYM(cpu_cli):
    cli
    ret

; ---- CPU exceptions ---------------------------------------------------------
%macro ISR_NOERR 1
global GSYM(isr%1)
GSYM(isr%1):
    cli
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global GSYM(isr%1)
GSYM(isr%1):
    cli
    push %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---- hardware IRQs: vectors 32..47 ------------------------------------------
%macro IRQ 2
global GSYM(irq%1)
GSYM(irq%1):
    cli
    push 0
    push %2
    jmp irq_common
%endmacro

%assign i 0
%rep 16
IRQ i, i+32
%assign i i+1
%endrep

; ---- catch-all for vectors 48..255 ------------------------------------------
global GSYM(isr_default)
GSYM(isr_default):
    cli
    push 0
    push 255
    jmp isr_common

; ---- common entry -----------------------------------------------------------
isr_common:
    pusha
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp
    call GSYM(isr_handler)
    add esp, 4
    pop eax
    mov ds, ax
    popa
    add esp, 8
    iret

irq_common:
    pusha
    mov ax, ds
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push esp
    call GSYM(irq_handler)
    add esp, 4
    pop eax
    mov ds, ax
    popa
    add esp, 8
    iret
