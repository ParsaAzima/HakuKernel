; Haku demo module: the smallest real ring-0 module.
;
; The host tool (tools/mkhakumod.py) turns this ELF32 object into a .hkm
; image. The kernel copies the image, resolves the four imports below
; against its export table, and calls haku_module_init as `int init(void)`.
; Returning zero keeps the module loaded; delete_module then calls
; haku_module_exit before releasing the image.
;
; The module writes through the kernel log, so the boot marker proves the
; relocated code really ran in ring 0 rather than being checked on paper.

bits 32

section .text
global haku_module_init
global haku_module_exit
extern haku_module_log
extern haku_module_log_dec
extern haku_module_ticks

haku_module_init:
    push dword message_init
    call haku_module_log
    add esp, 4

    call haku_module_ticks
    push eax
    call haku_module_log_dec
    add esp, 4

    push dword message_newline
    call haku_module_log
    add esp, 4

    xor eax, eax                ; 0 = the module accepted the load
    ret

haku_module_exit:
    push dword message_exit
    call haku_module_log
    add esp, 4
    ret

section .rodata
message_init db 'HAKU-MODULE-OK init ticks=', 0
message_newline db 10, 0
message_exit db 'HAKU-MODULE-EXIT', 10, 0
