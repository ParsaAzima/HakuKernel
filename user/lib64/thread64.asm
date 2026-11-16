; Haku x86-64 thread start and retire.
;
; haku64_clone_thread(child_stack, tls, record, clear_tid, detached, entry)
; issues clone(2) with CLONE_SETTLS. A non-null clear_tid adds
; CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID, which publishes the child tid in
; that word before the child can run and zeroes it (waking a futex) when the
; child ends; a null clear_tid means a thread with no join word. `detached`
; adds CLONE_DETACH, so the kernel releases the task slot at its next switch
; instead of leaving a zombie behind. `entry` receives `record` in RCX.
;
; The child resumes at the instruction after SYSCALL with the new stack
; already in RSP, so it must not return through this frame: it calls
; entry(record) instead. R13 holds the record and R15 the entry point across
; the call because the kernel copies the whole register frame into the child.
; The caller's initial RSP is 16-byte aligned, so the CALL below leaves the
; callee on an 8-mod-16 boundary, exactly what the Microsoft x64 ABI wants.
;
; The parent path only has to undo its six pushes and return the tid.
;
; haku64_thread_retire(child_stack, stack_size, status) is a detached
; thread's last act: it unmaps the stack it is running on and then exits.
; Nothing may touch the stack in between, so the status is parked in R9
; across the first syscall (which clobbers RCX and R11) and the two calls
; are issued straight from assembler with no frame and no stack traffic.
[bits 64]
default rel

%include "haku/abi64.inc"

section .text

global haku64_clone_thread
global haku64_thread_retire

haku64_clone_thread:
    push r12
    push r13
    push r14
    push r15
    push rdi                    ; RDI/RSI are nonvolatile under the MS ABI and
    push rsi                    ; this stub repurposes both for the kernel
    mov r12, rcx                ; child stack, loaded into the child's RSP
    mov r13, r8                 ; record, handed to the entry point
    mov r8, rdx                 ; TLS base (the child's FS, via CLONE_SETTLS)
    mov r10, r9                 ; child tid word, 0 for a thread nobody joins
    ; detached is an `int`, so load exactly four bytes: a qword load would
    ; pick up the caller's unwritten upper half and make a zero argument look
    ; non-zero. R14D zero-extends into R14.
    mov r14d, [rsp + 88]        ; detached: the first stack argument
    mov r15, [rsp + 96]         ; entry: the second stack argument

    mov edi, HAKU64_CLONE_VM | HAKU64_CLONE_FS | HAKU64_CLONE_FILES | HAKU64_CLONE_SIGHAND | HAKU64_CLONE_SYSVSEM | HAKU64_CLONE_THREAD | HAKU64_CLONE_SETTLS
    test r10, r10
    jz .no_tid_flags
    or edi, HAKU64_CLONE_CHILD_SETTID | HAKU64_CLONE_CHILD_CLEARTID
.no_tid_flags:
    test r14, r14
    jz .no_detach
    or edi, HAKU64_CLONE_DETACH
.no_detach:
    mov rsi, r12                ; child_stack
    xor edx, edx                ; parent_tid
    mov rax, HAKU64_SYS_CLONE
    syscall

    test rax, rax
    jnz .parent

    ; Child: RSP is the caller-supplied stack, the parent's frame is gone.
    ; R13 still names the record and R15 the entry point. The 32 bytes below
    ; RSP are the shadow space the call needs.
    mov rcx, r13
    call r15
    hlt                         ; an entry point never returns

.parent:
    pop rsi
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    ret

haku64_thread_retire:
    mov r9, r8                  ; the exit status, across the munmap syscall
    mov rdi, rcx                ; stack base
    mov rsi, rdx                ; stack size
    mov rax, HAKU64_SYS_MUNMAP
    syscall
    ; The stack is gone: no push, pop, call or interrupt-return from here.
    mov rdi, r9
    mov rax, HAKU64_SYS_EXIT
    syscall
    ud2
