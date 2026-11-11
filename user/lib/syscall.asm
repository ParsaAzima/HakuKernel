[bits 32]

%include "haku/abi.inc"

section .text

extern _haku_errno_check

; vfork(2): the child runs on the parent's stack, so the pending return
; address must not sit there while the syscall is in flight - the child's
; own calls would overwrite it. edx survives the syscall, so it waits there.
global _vfork
_vfork:
    pop edx
    mov eax, HAKU_SYS_VFORK
    int 0x80
    push eax
    call _haku_errno_check
    add esp, 4
    push edx
    ret

global _haku_sigreturn_trampoline
_haku_sigreturn_trampoline:
    ; A signal handler's restorer: esp still points just above the frame
    ; the kernel built, which is where rt_sigreturn looks for it.
    mov eax, HAKU_SYS_RT_SIGRETURN
    int 0x80
    hlt

global _haku_syscall0
_haku_syscall0:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall1
_haku_syscall1:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov ebx, [esp + 20]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall2
_haku_syscall2:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov ebx, [esp + 20]
    mov ecx, [esp + 24]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall3
_haku_syscall3:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov ebx, [esp + 20]
    mov ecx, [esp + 24]
    mov edx, [esp + 28]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall4
_haku_syscall4:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov ebx, [esp + 20]
    mov ecx, [esp + 24]
    mov edx, [esp + 28]
    mov esi, [esp + 32]
    mov edi, [esp + 36]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall5
_haku_syscall5:
    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov ebx, [esp + 20]
    mov ecx, [esp + 24]
    mov edx, [esp + 28]
    mov esi, [esp + 32]
    mov edi, [esp + 36]
    int 0x80
    pop edi
    pop esi
    pop ebx
    ret

global _haku_syscall6
_haku_syscall6:
    push ebx
    push esi
    push edi
    push ebp                ; the sixth argument travels in ebp
    mov eax, [esp + 20]
    mov ebx, [esp + 24]
    mov ecx, [esp + 28]
    mov edx, [esp + 32]
    mov esi, [esp + 36]
    mov edi, [esp + 40]
    mov ebp, [esp + 44]
    int 0x80
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

; thread_create(int (*fn)(void *), void *arg, void *stack_top)
; Creates a thread that shares the caller's address space and descriptors.
; The clone returns twice: the parent resumes with the new tid in eax, while
; the child starts on the caller-supplied stack with eax = 0 and calls
; fn(arg). The child never returns from this function: it exits the thread.
; ebp and esi carry fn/arg across the syscall, because the kernel copies the
; register frame into the child.
global _haku_thread_create
_haku_thread_create:
    push ebx
    push esi
    push edi
    push ebp
    mov ebp, [esp + 20]          ; fn
    mov esi, [esp + 24]          ; arg
    mov ecx, [esp + 28]          ; stack_top becomes the child's esp
    mov ebx, HAKU_CLONE_VM | HAKU_CLONE_FS | HAKU_CLONE_FILES | HAKU_CLONE_SIGHAND | HAKU_CLONE_SYSVSEM | HAKU_CLONE_THREAD
    xor edx, edx                 ; parent_tid
    xor edi, edi                 ; tls
    mov eax, HAKU_SYS_CLONE
    int 0x80
    test eax, eax
    jnz .parent
    ; Child: the kernel already switched esp to stack_top.
    push esi
    call ebp
    mov ebx, eax                 ; fn's return value is the exit status
    mov eax, HAKU_SYS_EXIT
    int 0x80
    hlt
.parent:
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret
