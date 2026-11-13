; =============================================================================
; Haku v0.1 — interrupt stubs (NASM -> COFF object for link.exe)
;
; Each CPU exception gets a tiny stub that pushes a uniform frame:
;   [dummy error code unless the CPU pushed a real one] + vector number,
; then jumps to isr_common. IRQs (vectors 32..47) go to irq_common instead.
; isr_common saves everything, calls the C dispatcher with a regs_t pointer,
; restores state and irets. All IDT gates are interrupt gates, so IF is
; already clear on entry — hence the leading cli is just belt and braces.
; =============================================================================

[bits 32]

global _idt_load, _cpu_sti, _cpu_cli
global _syscall128
global _apic_timer, _apic_resched, _apic_tlb
extern _isr_handler, _irq_handler, _syscall_handler
extern _apic_irq_handler
extern _scheduler_signal_deliver

section .text

_idt_load:                       ; void idt_load(uint32_t idtr_addr)
    mov eax, [esp+4]
    lidt [eax]
    ret

_cpu_sti:
    sti
    ret

_cpu_cli:
    cli
    ret

; uint32_t cpu_atomic_exchange(volatile uint32_t *address, uint32_t value)
; xchg with a memory operand is implicitly locked, so two CPUs running this
; at once still see one indivisible exchange. Returns the old word in EAX.
global _cpu_atomic_exchange
_cpu_atomic_exchange:
    mov ecx, [esp+4]
    mov eax, [esp+8]
    xchg [ecx], eax
    ret

; void cpu_pause(void) — spin-wait hint, no architectural side effect.
global _cpu_pause
_cpu_pause:
    pause
    ret

; The first privilege transition also needs a safe ring-0 stack.
global _gdt_load, _tss_load, _scheduler_enter_frame
_gdt_load:                       ; void gdt_load(uint32_t gdtr_addr)
    mov eax, [esp+4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ss, ax
    mov ax, 0x38                  ; ring-0 data: base = this CPU's block
    mov gs, ax
    cld                           ; kernel C code always runs with DF clear
    jmp 0x08:.reload_cs
.reload_cs:
    ret

_tss_load:                       ; void tss_load(uint16_t selector)
    mov ax, [esp+4]
    ltr ax
    ret

section .text
; ---- CPU exceptions: dummy error code except where the CPU pushes one -----
; Error-code vectors (Intel SDM): 8, 10, 11, 12, 13, 14, 17.
%macro ISR_NOERR 1
global _isr%1
_isr%1:
    cli
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global _isr%1
_isr%1:
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

; ---- hardware IRQs: vectors 32..47 -----------------------------------------
%macro IRQ 2
global _irq%1
_irq%1:
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
global _isr_default
_isr_default:
    cli
    push 0
    push 255
    jmp isr_common

global _syscall128
_syscall128:
    push 0
    push 128
    jmp syscall_common

; ---- common entry ------------------------------------------------------------
%macro SAVE_SEGMENTS 0
    pusha
    xor eax, eax
    mov ax, ds
    push eax
    xor eax, eax
    mov ax, es
    push eax
    xor eax, eax
    mov ax, fs
    push eax
    xor eax, eax
    mov ax, gs
    push eax
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    ; GS is the per-CPU block in ring 0. Reloading it unconditionally would
    ; zero the base when the interrupted context was already ring 0 (the
    ; interrupt gate does not change segment registers), so only a frame
    ; that came from ring 3 switches to the kernel selector. CS sits 60
    ; bytes above the saved GS slot: popa (32) + four segments (16) + the
    ; stub's vector/error words (8) + eip (4).
    test byte [esp+60], 3
    jz %%gs_ready
    mov ax, 0x38
    mov gs, ax
%%gs_ready:
    cld                           ; C code must not inherit user DF state
%endmacro

%macro RESTORE_SEGMENTS 0
    pop eax
    mov gs, ax
    pop eax
    mov fs, ax
    pop eax
    mov es, ax
    pop eax
    mov ds, ax
    popa
%endmacro

; Deliver any pending signal before the interrupted context resumes. The
; frame sits at the top of this kernel stack, so pushing the argument
; below it cannot clobber anything, and the call may switch tasks (a
; fatal default disposition ends the process), so it returns the frame
; to resume.
%macro DELIVER_SIGNALS 0
    push eax
    call _scheduler_signal_deliver
    add esp, 4
%endmacro

; ---- local-APIC interrupts ---------------------------------------------------
; Vectors 0x40..0x42 sit above the 8259's remapped range. They are ordinary
; interrupt gates, so IF is already clear on entry; the C dispatcher
; acknowledges the unit and decides whether the frame changed hands.
%macro APIC_ENTRY 2
global %1
%1:
    cli
    push 0
    push %2
    jmp apic_common
%endmacro

APIC_ENTRY _apic_timer,   0x40
APIC_ENTRY _apic_resched, 0x41
APIC_ENTRY _apic_tlb,     0x42

apic_common:
    SAVE_SEGMENTS
    push esp
    call _apic_irq_handler
    add esp, 4
    DELIVER_SIGNALS
    mov esp, eax                 ; the timer/resched handler may switch tasks
    RESTORE_SEGMENTS
    add esp, 8
    iret

_scheduler_enter_frame:         ; void scheduler_enter_frame(regs_t *frame)
    mov eax, [esp+4]
    mov esp, eax
    RESTORE_SEGMENTS
    add esp, 8
    iret

isr_common:
    SAVE_SEGMENTS
    push esp                      ; regs_t* argument
    call _isr_handler
    add esp, 4                    ; drop argument
    DELIVER_SIGNALS
    mov esp, eax                 ; user exception may terminate its task
    RESTORE_SEGMENTS
    add esp, 8                    ; drop vector number + error code
    iret

irq_common:
    SAVE_SEGMENTS
    push esp
    call _irq_handler
    add esp, 4
    DELIVER_SIGNALS
    mov esp, eax                 ; IRQ dispatcher may select another task
    RESTORE_SEGMENTS
    add esp, 8
    iret

syscall_common:
    SAVE_SEGMENTS
    push esp
    sti                           ; blocking syscalls need hardware IRQs
    call _syscall_handler
    cli
    add esp, 4
    DELIVER_SIGNALS
    mov esp, eax                 ; exit may switch directly to another task
    RESTORE_SEGMENTS
    add esp, 8
    iret
