[bits 32]

%include "haku/abi.inc"

section .text

extern _haku_errno_check
extern _haku_pthread_begin

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

; haku_tcb_address(): the self pointer at the base of this thread's control
; block, i.e. the user FS base itself. No block is installed only before
; crt0's TLS setup, which runs before any library call.
global _haku_tcb_address
_haku_tcb_address:
    mov eax, [fs:0]
    ret

; haku_errno_slot(): the address of this thread's errno. Following the block's
; self pointer instead of assuming the segment base is already loaded means
; the caller never needs set_thread_area's result, and the slot stays valid
; after a fork (the child's block is at the same address).
global _haku_errno_slot
_haku_errno_slot:
    mov eax, [fs:0]
    add eax, HAKU_TCB_ERRNO
    ret

; haku_thread_clone(stack_top, tls, record, flags, child_tid)
; clone(2) for the pthread layer: the child shares the address space and
; descriptor row, gets `stack_top` as its ESP and `tls` as its FS base (so
; the kernel installs the thread's control block before the child runs).
; The record travels in ebp, a register the kernel copies into the child, and
; the child calls haku_pthread_begin(record), which never returns. The flags
; carry the detach bit, so no wait4 is needed to release the task slot, and
; CLONE_CHILD_SETTID/CLEARTID publish the child's id in *child_tid and zero it
; at exit, which is the word a joiner waits on.
global _haku_thread_clone
_haku_thread_clone:
    push ebx
    push esi
    push edi
    push ebp
    mov ecx, [esp + 20]          ; stack_top
    mov edi, [esp + 24]          ; tls base for CLONE_SETTLS
    mov ebp, [esp + 28]          ; record, survives into the child
    mov ebx, [esp + 32]          ; clone flags
    mov esi, [esp + 36]          ; child_tid word
    xor edx, edx                 ; parent_tid
    mov eax, HAKU_SYS_CLONE
    int 0x80
    test eax, eax
    jnz .parent
    ; Child: the kernel has already loaded stack_top into esp.
    push ebp
    call _haku_pthread_begin
    hlt                          ; haku_pthread_begin never returns
.parent:
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

; haku_thread_retire(stack, stack_size, page, status)
; A detached thread's last act: unmap the stack it is standing on and the page
; that holds its control block and handle, then exit the thread. Nothing may
; touch either mapping in between - not a push, not a pop, not a return - so
; the page address waits in edi and the status in esi, both of which int 0x80
; preserves, and the exit is issued straight from the unmapped stack pointer.
global _haku_thread_retire
_haku_thread_retire:
    push ebx
    push esi
    push edi
    mov ebx, [esp + 16]          ; stack base
    mov ecx, [esp + 20]          ; stack size
    mov edi, [esp + 24]          ; page holding the control block
    mov esi, [esp + 28]          ; exit status
    mov eax, HAKU_SYS_MUNMAP
    int 0x80
    mov ebx, edi
    mov ecx, 4096                ; the thread page, see HAKU_THREAD_PAGE
    mov eax, HAKU_SYS_MUNMAP
    int 0x80
    mov ebx, esi
    mov eax, HAKU_SYS_EXIT
    int 0x80
    hlt
