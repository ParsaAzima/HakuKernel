; Haku x86-64 thread start.
;
; haku64_clone_thread(child_stack, tls, record, clear_tid) issues clone(2)
; with CLONE_SETTLS and CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID. The child
; resumes at the instruction after SYSCALL with the new stack already in RSP,
; so it must not return through this frame: it calls haku64_thread_entry(record)
; instead, which installs GS and runs the user's function. R13 holds the
; record across the call because the kernel copies the whole register frame
; into the child.
;
; The child tid word is published by the kernel (CLONE_CHILD_SETTID) before
; the child can run and zeroed by it (CLONE_CHILD_CLEARTID) after the child
; ends, which is what haku64_thread_join() futex-waits on.
;
; The parent path only has to undo its two pushes and return the tid.
;
; haku64_thread_entry is C compiled with the Microsoft x64 ABI, so its one
; argument goes in RCX (not RDI as the SysV ABI would have it).
[bits 64]
default rel

%include "haku/abi64.inc"

section .text

extern haku64_thread_entry

global haku64_clone_thread

haku64_clone_thread:
    push r12
    push r13
    mov r12, rcx                ; child stack, loaded into the child's RSP
    mov r13, r8                 ; start record, 32 bytes above that RSP
    mov r8, rdx                 ; TLS base (the child's FS, via CLONE_SETTLS)
    mov r10, r9                 ; child tid word (SETTID/CLEARTID)

    mov rax, HAKU64_SYS_CLONE
    mov edi, HAKU64_CLONE_VM | HAKU64_CLONE_FS | HAKU64_CLONE_FILES | HAKU64_CLONE_SIGHAND | HAKU64_CLONE_SYSVSEM | HAKU64_CLONE_THREAD | HAKU64_CLONE_SETTLS | HAKU64_CLONE_CHILD_SETTID | HAKU64_CLONE_CHILD_CLEARTID
    mov rsi, r12                ; child_stack
    xor edx, edx                ; parent_tid
    syscall

    test rax, rax
    jnz .parent

    ; Child: RSP is the caller-supplied stack, the parent's frame is gone.
    ; R13 still names the record, and the 32 bytes between RSP and the
    ; record are exactly the shadow space this call needs.
    mov rcx, r13
    call haku64_thread_entry
    hlt                         ; haku64_thread_entry calls _exit, never returns

.parent:
    pop r13
    pop r12
    ret
