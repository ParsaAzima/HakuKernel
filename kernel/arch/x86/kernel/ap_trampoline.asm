; =============================================================================
; Haku 32-bit AP trampoline (NASM, copied to physical 0x8000 at boot)
;
; An application processor wakes up in real mode at (SIPI vector << 12):0000
; with no GDT and no paging. This blob takes it real -> protected -> C:
;
;   1. flat segments, a stack at 0x8F00, load the embedded GDT,
;   2. set CR0.PE, far jump to the 32-bit code selector,
;   3. load the kernel CR3 passed in the parameter block, set CR0.PG,
;   4. switch to the AP's direct-mapped stack and call smp_ap_entry(cpu).
;
; The blob is position-dependent at 0x8000: labels are addressed as
; AP_BASE + (label - start), so the assembled bytes work wherever the linker
; put them and the C side copies [start, end) to 0x8000. It must stay inside
; one page: the parameters live at 0x8F00 at the top of that page.
; =============================================================================

[bits 16]

global _ap_trampoline_start
global _ap_trampoline_end
global _smp_memory_barrier

%define AP_BASE        0x8000
%define AP_STACK_TOP   0x8F00
%define AP_PARAM_CR3   (AP_BASE + 0xF00)
%define AP_PARAM_STACK (AP_BASE + 0xF04)
%define AP_PARAM_CPU   (AP_BASE + 0xF08)
%define AP_PARAM_ENTRY (AP_BASE + 0xF0C)

section .text
align 16

_ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, AP_STACK_TOP
    lgdt [AP_BASE + (ap_gdtr - _ap_trampoline_start)]

    mov eax, cr0
    or eax, 1                     ; CR0.PE
    mov cr0, eax
    jmp 0x08:(AP_BASE + (ap_protected - _ap_trampoline_start))

[bits 32]
ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov eax, [AP_PARAM_CR3]       ; kernel page directory
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80000000            ; CR0.PG
    mov cr0, eax
    jmp short .flushed            ; next fetch must be identity mapped

.flushed:
    mov esp, [AP_PARAM_STACK]     ; direct-mapped per-CPU stack
    mov eax, [AP_PARAM_CPU]
    push eax                      ; cdecl argument
    mov eax, [AP_PARAM_ENTRY]
    call eax                      ; smp_ap_entry(cpu), never returns

.hang:
    cli
    hlt
    jmp .hang

; Full memory barrier for the BSP: the trampoline and the parameter words
; must be globally visible before the INIT/SIPI sequence starts.
_smp_memory_barrier:
    mfence
    ret

align 8
ap_gdt:
    dq 0
    dw 0xFFFF, 0x0000             ; 0x08: 32-bit code, base 0, 4 GiB
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000             ; 0x10: data, base 0, 4 GiB
    db 0x00, 0x92, 0xCF, 0x00
ap_gdt_end:
ap_gdtr:
    dw ap_gdt_end - ap_gdt - 1
    dd AP_BASE + (ap_gdt - _ap_trampoline_start)

align 16
_ap_trampoline_end:
