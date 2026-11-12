; =============================================================================
; Haku x86-64 AP trampoline (NASM, copied to physical 0x8000 at boot)
;
; The AP starts in real mode at (SIPI vector << 12):0000. This blob walks it
; through the same path entry64.asm took on the bootstrap processor, minus
; the page-table construction: the BSP already built a PML4 that identity
; maps the low 1 GiB and the physmap, and its physical address is passed in
; the parameter block.
;
;   real mode -> protected mode (32-bit) -> CR4.PAE + EFER.LME -> long mode
;   -> smp64_ap_entry(cpu)
;
; The blob is position-dependent at 0x8000: labels are addressed as
; AP_BASE + (label - start). The C side copies [start, end) to 0x8000 and
; SIPI vector 0x08 starts it. It must stay inside one page; parameters live
; at 0x8F00, eight bytes each.
; =============================================================================

[bits 16]

global ap_trampoline64_start
global ap_trampoline64_end
global smp_memory_barrier64

%define AP_BASE        0x8000
%define AP_STACK_TOP   0x8F00
%define AP_PARAM_CR3   (AP_BASE + 0xF00)
%define AP_PARAM_STACK (AP_BASE + 0xF08)
%define AP_PARAM_CPU   (AP_BASE + 0xF10)
%define AP_PARAM_ENTRY (AP_BASE + 0xF18)

section .text
align 16

ap_trampoline64_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, AP_STACK_TOP
    lgdt [AP_BASE + (ap64_gdtr - ap_trampoline64_start)]

    mov eax, cr0
    or eax, 1                     ; CR0.PE
    mov cr0, eax
    jmp 0x08:(AP_BASE + (ap64_protected - ap_trampoline64_start))

[bits 32]
ap64_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov eax, cr4
    or eax, 0x20                  ; CR4.PAE
    mov cr4, eax

    mov eax, [AP_PARAM_CR3]       ; BSP's PML4, physical
    mov cr3, eax

    mov ecx, 0xC0000080           ; IA32_EFER: LME + NXE
    rdmsr
    or eax, 0x901
    wrmsr

    mov eax, cr0
    or eax, 0x80000001            ; CR0.PG (PE is already set)
    mov cr0, eax
    jmp 0x18:(AP_BASE + (ap64_long - ap_trampoline64_start))

[bits 64]
ap64_long:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [AP_PARAM_STACK]     ; direct-mapped per-CPU stack
    sub rsp, 0x20                 ; shadow space for the Win64 callee
    mov rcx, [AP_PARAM_CPU]       ; first Windows x64 argument
    mov rax, [AP_PARAM_ENTRY]
    call rax                      ; smp64_ap_entry(cpu), never returns

.hang:
    cli
    hlt
    jmp .hang

; Full memory barrier for the BSP: trampoline and parameters must be visible
; before the INIT/SIPI sequence starts.
smp_memory_barrier64:
    mfence
    ret

align 8
ap64_gdt:
    dq 0
    dw 0xFFFF, 0x0000             ; 0x08: 32-bit code, base 0, 4 GiB
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000             ; 0x10: data, base 0, 4 GiB
    db 0x00, 0x92, 0xCF, 0x00
    dw 0xFFFF, 0x0000             ; 0x18: 64-bit code (L=1)
    db 0x00, 0x9A, 0xAF, 0x00
    dw 0xFFFF, 0x0000             ; 0x20: data, base 0, 4 GiB
    db 0x00, 0x92, 0xCF, 0x00
ap64_gdt_end:
ap64_gdtr:
    dw ap64_gdt_end - ap64_gdt - 1
    dd AP_BASE + (ap64_gdt - ap_trampoline64_start)

align 16
ap_trampoline64_end:
