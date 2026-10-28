; Haku x86-64 interrupt entry stubs and the handful of instructions MSVC
; cannot express (MSVC x64 has no inline assembly at all).
;
; The 32-bit kernel's isr.asm pushes a frame of 32-bit registers. Here the
; CPU pushes five 8-byte words on a ring-0 interrupt (SS, RSP, RFLAGS, CS,
; RIP) instead of five 4-byte ones, so the saved frame is a regs64_t and the
; C dispatcher reads 64-bit fields. Everything else follows the same model:
; a uniform stub per vector, an error-code slot that is zero-filled for the
; vectors the CPU does not supply one for, and a single common path that
; saves the general registers, calls C, and returns with IRETQ.
;
; Stack frame the C side sees (lowest address first, i.e. in push order
; reversed):
;
;   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax
;   vector error rip cs rflags rsp ss
;
; `vector` and `error` are the two words the stub pushes; rip..ss are the
; CPU's own frame. When the interrupt arrives in ring 0 the CPU does not
; push SS/RSP, so those two slots hold whatever was on the stack — the C
; dispatcher only reports them for a ring-3 frame.

[BITS 64]

global isr64_load_idt
global gdt64_load
global tss64_load
global sti64
global cli64
global read_cr2_64
global cpu_pause64
global outb64
global inb64

global int3_probe64
global probe_load64
global probe_load64_fault
global probe_load64_resume
global probe_div64
global probe_div64_fault
global probe_div64_resume

extern isr64_dispatch

section .text

; ------------------------------------------------------------------ stubs
; Vectors that push an error code: #DF(8) #TS(10) #NP(11) #SS(12) #GP(13)
; #PF(14) #AC(17) #CP(21) #VC(29) #SX(30). Every other vector gets a zero
; pushed so the frame layout never changes.

%macro ISR_NOERR 1
global isr64_%1
isr64_%1:
    push qword 0                 ; no error code: keep the frame uniform
    push qword %1
    jmp isr64_common
%endmacro

%macro ISR_ERR 1
global isr64_%1
isr64_%1:
    push qword %1                ; the CPU already pushed the error code
    jmp isr64_common
%endmacro

%macro IRQ_STUB 1
global irq64_%1
irq64_%1:
    push qword 0
    push qword (32 + %1)
    jmp isr64_common
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
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

global isr64_default
isr64_default:
    push qword 0
    push qword 0xFF
    jmp isr64_common

; ------------------------------------------------------------- common path
isr64_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; The Microsoft x64 ABI wants RSP 16-byte aligned at the CALL, plus 32
    ; bytes of shadow space. RBP holds the frame pointer across the call
    ; because it is callee-saved; the dispatcher returns the frame to pop,
    ; which lets a future context switch hand back a different one.
    mov rbp, rsp
    mov rcx, rsp                 ; regs64_t *frame
    and rsp, ~0xF
    sub rsp, 32
    call isr64_dispatch
    mov rbp, rax
    mov rsp, rbp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                  ; vector + error code
    iretq

; ------------------------------------------------------------- primitives
; void isr64_load_idt(u64 idtr_addr)   [Microsoft x64: address in RCX]
isr64_load_idt:
    lidt [rcx]
    ret

; void gdt64_load(u64 gdtr_addr)
; LGDT only reloads the descriptor-table register: the segment registers keep
; their cached descriptors, so nothing else has to happen here. In particular
; CS is *not* reloaded with a far return — the table gdt64.c builds keeps the
; boot code descriptor byte-for-byte at selector 0x08, so the cached one stays
; valid. (Reloading it anyway means a 64-bit far return, which faults on the
; qemu64 model the test harness boots; it buys nothing, so it is not here.
; The constraint this leaves is that index 1 must stay the kernel code
; descriptor, which gdt64.c documents.)
gdt64_load:
    lgdt [rcx]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    ret

; void tss64_load(u16 selector)   [selector in CX]
tss64_load:
    ltr cx
    ret

; void sti64(void) / void cli64(void)
sti64:
    sti
    ret

cli64:
    cli
    ret

; u64 read_cr2_64(void)
read_cr2_64:
    mov rax, cr2
    ret

; void cpu_pause64(void)
cpu_pause64:
    pause
    ret

; void outb64(u16 port, u8 value)   [port in CX, value in DL]
outb64:
    mov al, dl
    mov dx, cx
    out dx, al
    ret

; u8 inb64(u16 port)   [port in CX]
inb64:
    mov dx, cx
    in al, dx
    movzx eax, al
    ret

; ------------------------------------------------------- exception probes
; Recovering from an exception needs the faulting instruction to be skipped.
; Rather than decoding it, each probe exports a label for the instruction and
; another for the instruction after it, so the C handler can set RIP to an
; address the assembler computed instead of a hardcoded length.

; void int3_probe64(void) — INT3 is a trap: RIP already points past it.
int3_probe64:
    int3
    ret

; u64 probe_load64(u64 address) — faults when the address is unmapped.
probe_load64:
probe_load64_fault:
    mov rax, [rcx]               ; 48 8B 01
probe_load64_resume:
    ret

; u64 probe_div64(u64 a, u64 b) — faults with #DE when b is zero.
; DIV divides RDX:RAX by its operand, so the divisor moves out of RDX first
; (R8 is caller-saved, so clobbering it is free here).
probe_div64:
    mov r8, rdx
    mov rax, rcx
    xor edx, edx                 ; high half of the dividend
probe_div64_fault:
    div r8                       ; 49 F7 F0
probe_div64_resume:
    ret
