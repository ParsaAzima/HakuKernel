; Haku hello module: a character device implemented entirely as a loadable
; ring-0 module.
;
; The kernel binds /dev/hello to this image while the module is loaded and
; refuses `delete_module` while a descriptor is open, so a read never lands
; in freed module code. The device is read-only: writes are accepted and
; discarded, which is what a shell redirect expects.

bits 32

section .text
global haku_module_init
global haku_module_exit
extern haku_module_register_device
extern haku_module_log

; int32_t hello_read(void *buffer, uint32_t count, uint32_t *out_count)
hello_read:
    push esi
    push edi
    mov edi, [esp + 12]        ; buffer
    mov ecx, [esp + 16]        ; count
    mov esi, hello_text
    mov edx, hello_text_length
    cmp ecx, edx
    jbe .ready
    mov ecx, edx
.ready:
    mov edx, ecx               ; bytes actually delivered
    cld
    rep movsb
    mov eax, [esp + 20]        ; out_count
    mov [eax], edx
    pop edi
    pop esi
    mov eax, 1
    ret

; int32_t hello_write(const void *buffer, uint32_t count, uint32_t *out_count)
hello_write:
    mov eax, [esp + 8]         ; count
    mov edx, [esp + 12]        ; out_count
    mov [edx], eax
    mov eax, 1
    ret

haku_module_init:
    push dword hello_write
    push dword hello_read
    push dword hello_name
    call haku_module_register_device
    add esp, 12
    test eax, eax
    js .fail
    push dword hello_loaded
    call haku_module_log
    add esp, 4
    xor eax, eax               ; 0 = the module accepted the load
    ret
.fail:
    ret                        ; negative errno reaches init_module

haku_module_exit:
    push dword hello_exit
    call haku_module_log
    add esp, 4
    ret

section .rodata
hello_name db 'hello', 0
hello_text db 'hello from a haku module', 10
hello_text_end:
hello_text_length equ hello_text_end - hello_text
hello_loaded db 'HELLO-MODULE-OK /dev/hello registered', 10, 0
hello_exit db 'HELLO-MODULE-EXIT', 10, 0
