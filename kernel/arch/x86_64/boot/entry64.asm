; Haku x86-64 entry: stage2 hands control here in 32-bit protected mode
; (flat segments, ESP = 0x90000, kernel image at 0x10000) and this stub
; enters IA-32e long mode, then calls the 64-bit C kernel.
;
; Long mode needs three things the 32-bit kernel never had: PAE (CR4.PAE),
; a four-level page table rooted in CR3, and EFER.LME. The identity map is
; built with 2 MiB pages so 1 GiB of RAM costs 512 entries in one table
; instead of 256 page tables — the same trick Linux uses for its kernel
; mapping on non-PAE builds, and enough for the boot path and everything
; below 1 GiB.
;
; Memory used here (all free at this point; stage2 owns 0x7E00-0x8E00,
; the VBE handover record 0x6F00-0x7800, the kernel 0x10000+, and the
; bootstrap stack below 0x90000):
;   0x1000  PML4 (page map level 4)
;   0x2000  PDPT (page directory pointer table)
;   0x3000  PD   (page directory, 2 MiB pages)

[BITS 32]

%define PML4_PHYS       0x00001000
%define PDPT_PHYS       0x00002000
%define PD_PHYS         0x00003000
%define BOOT_STACK_TOP  0x00090000

%define PAGE_PRESENT    0x001
%define PAGE_WRITABLE   0x002
%define PAGE_2MB        0x080

%define CR4_PAE         (1 << 5)
%define CR0_PG          (1 << 31)
%define CR0_PE          (1 << 0)
%define EFER_LME        (1 << 8)
%define EFER_NXE        (1 << 11)
%define MSR_EFER        0xC0000080

global entry64
global serial_write64
global serial_putc64
global invlpg64
global cpu_halt64
extern kernel64_main

section .text

entry64:
    cli
    cld

    ; --- blank the three paging structures --------------------------------
    mov edi, PML4_PHYS
    mov ecx, 3 * 1024                 ; three 4 KiB pages of dwords
    xor eax, eax
    rep stosd

    ; --- link PML4[0] -> PDPT, PDPT[0] -> PD -------------------------------
    mov eax, PDPT_PHYS | PAGE_PRESENT | PAGE_WRITABLE
    mov [PML4_PHYS], eax
    mov eax, PD_PHYS | PAGE_PRESENT | PAGE_WRITABLE
    mov [PDPT_PHYS], eax

    ; --- PD: 512 x 2 MiB = 1 GiB identity mapped ---------------------------
    ; Everything the boot path touches (kernel image, handover records,
    ; bootstrap stack) is below 1 GiB, and the frame allocator never hands
    ; out memory the kernel cannot address.
    mov edi, PD_PHYS
    mov eax, PAGE_PRESENT | PAGE_WRITABLE | PAGE_2MB
    mov ecx, 512
.fill_pd:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    ; --- enter long mode: CR4.PAE, EFER.LME, CR3, CR0.PG -------------------
    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax

    mov ecx, MSR_EFER
    rdmsr
    or eax, EFER_LME | EFER_NXE
    wrmsr

    mov eax, PML4_PHYS
    mov cr3, eax

    mov eax, cr0
    or eax, CR0_PG | CR0_PE
    mov cr0, eax

    ; --- far jump: only a code segment with L=1 leaves compatibility mode -
    lgdt [gdt64_desc]
    jmp 0x08:.long_mode

[BITS 64]
.long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; The Microsoft x64 ABI wants RSP 16-byte aligned at the call, which
    ; leaves it 8 mod 16 on entry to the callee (the return address is
    ; pushed by the call itself).
    mov rsp, BOOT_STACK_TOP
    xor ebp, ebp
    and rsp, ~0xF

    call kernel64_main

.hang:
    cli
    hlt
    jmp .hang

; ---------------------------------------------------------------- helpers
; void serial_write64(const char *text)   [Microsoft x64: pointer in RCX]
; Polled COM1 output; stage 1 already programmed the divisor and frame.
serial_write64:
    push rbx                          ; callee-saved, and we use it
    mov rbx, rcx
.loop:
    mov al, [rbx]
    test al, al
    jz .done
.wait:
    mov dx, 0x3FD                     ; line status register
    in al, dx
    test al, 0x20                     ; THR empty?
    jz .wait
    mov dx, 0x3F8
    mov al, [rbx]
    out dx, al
    inc rbx
    jmp .loop
.done:
    pop rbx
    ret

; void cpu_halt64(void)
cpu_halt64:
    hlt
    ret

; void serial_putc64(char c)   [Microsoft x64: character in CL]
serial_putc64:
.wait:
    mov dx, 0x3FD                     ; line status register
    in al, dx
    test al, 0x20                     ; THR empty?
    jz .wait
    mov dx, 0x3F8
    mov al, cl
    out dx, al
    ret

; void invlpg64(u64 address)   [Microsoft x64: address in RCX]
; MSVC x64 has no inline assembly, so the one TLB instruction the paging
; code needs lives here.
invlpg64:
    invlpg [rcx]
    ret

; ---------------------------------------------------------------- GDT
section .data
align 16
gdt64:
    dq 0x0000000000000000             ; null
.code:
    dq 0x00209A0000000000             ; L=1, P=1, DPL=0, S=1, exec/read
.data:
    dq 0x0000920000000000             ; P=1, DPL=0, S=1, data read/write
gdt64_end:

gdt64_desc:
    dw gdt64_end - gdt64 - 1
    dq gdt64
