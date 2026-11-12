; Minimal static ELF64 executable used by the x86-64 syscall/ELF self-test.
;
; tools/mkelf64.py wraps this flat binary in an ELF image at USER64_BASE and
; the kernel embeds the result in .rdata. The program exercises the six
; argument SYSCALL ABI: the success paths for write, writev, getpid,
; clock_gettime, open/read/close/lseek, anonymous and file-backed mmap and
; munmap, directory creation and file writes on the FAT12 disk, fork and
; wait4, and the error paths for a bad descriptor, a bad user pointer, an
; unknown number, a W+X mapping, a bad mmap descriptor/offset and an
; unaligned munmap. It prints HAKU64-FILE-OK by reading the disk, proves the
; write path with HAKU64-DISK-OK, observes a MAP_SHARED page through a
; forked child and prints HAKU64-FORK-OK, runs a clone()d thread that shares
; the address space and the descriptor table, sleeps on a futex until the
; thread wakes it and prints HAKU64-THREAD-OK, walks the rest of the ported
; table -- stat/fstat/statx, chdir/getcwd and relative names, openat/
; mkdirat/getdents64/rmdir, create/ftruncate/truncate/rename/unlink, uname/
; time/times/rlimit/clock_getres, prctl names, getrandom, brk+mprotect,
; poll/select, sched_yield, dup3/fcntl -- and prints HAKU64-PORT-OK, then
; stores a magic word in its zero-filled BSS page and exits with status 0;
; the kernel reads both back, which proves the image ran in ring 3 instead
; of merely being mapped. A failure exits with the number of the step that
; failed (the early ABI tests leave 1, the disk, fork, dup, sleep, exec and
; thread steps use 20-70, the ported table uses 71-117), which the kernel
; prints as HAKU64-ELF-RUN-FAIL: exit=N.
;
; The code is position independent (RIP-relative operands only), so the flat
; binary does not need a linker to resolve USER64_BASE: mkelf64.py chooses the
; load address, and USER64_BASE is only needed for the BSS store.
[bits 64]
default rel

%include "haku/abi64.inc"

%ifndef USER64_BASE
%define USER64_BASE 0x0000008000400000
%endif

%ifndef USER64_BSS_OFFSET
%define USER64_BSS_OFFSET 0x1000
%endif
; mkelf64.py puts the read-write PT_LOAD segment at USER64_BSS_OFFSET above
; the load base; build.py sets both numbers together.
%define USER64_BSS (USER64_BASE + USER64_BSS_OFFSET)

; A free page inside the kernel's anonymous mmap window (K64_USER_MMAP_BASE
; is 0x8010000000), used to exercise MAP_FIXED.
%define USER64_FIXED 0x0000008010008000

; Scratch inside the writable segment for file buffers. The magic word the
; kernel reads back lives at offset 0, so the buffers start further in.
%define FILE_BUF   (USER64_BSS + 0x200)
%define STATUS_BUF (USER64_BSS + 0x300)
%define NOTE_BUF   (USER64_BSS + 0x400)
%define REREAD_BUF (USER64_BSS + 0x500)
%define EXEC_ARGV  (USER64_BSS + 0x600)
%define EXEC_ENVP  (USER64_BSS + 0x680)
%define THREAD_FLAG  (USER64_BSS + 0x700)
%define THREAD_COUNT (USER64_BSS + 0x708)
%define THREAD_TIDS  (USER64_BSS + 0x710)
%define STAT_BUF     (USER64_BSS + 0x780)
%define CWD_BUF      (USER64_BSS + 0x820)
%define DIR_BUF      (USER64_BSS + 0x880)
%define POLL_BUF     (USER64_BSS + 0xAA0)
%define UNAME_BUF    (USER64_BSS + 0xAC0)
%define RAND_BUF     (USER64_BSS + 0xC80)

_start:
    mov r15d, 1                     ; step number reported by fail (see top)
    ; write(1, "ELF64\n", 6) through SYSCALL, then prove the SYSRET return
    ; with a second write that only ring 3 can reach after the first one.
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_boot]
    mov edx, msg_boot_end - msg_boot
    syscall
    cmp rax, msg_boot_end - msg_boot
    jne fail

    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_sys64]
    mov edx, msg_sys64_end - msg_sys64
    syscall
    cmp rax, msg_sys64_end - msg_sys64
    jne fail

    ; getpid() must return a non-zero task id.
    mov eax, HAKU64_SYS_GETPID
    syscall
    test rax, rax
    jle fail

    ; clock_gettime(CLOCK_MONOTONIC, &timespec) writes a sane tv_nsec.
    sub rsp, 32
    mov eax, HAKU64_SYS_CLOCK_GETTIME
    mov edi, HAKU64_CLOCK_MONOTONIC
    mov rsi, rsp
    syscall
    test rax, rax
    jne fail
    cmp qword [rsp + 8], 1000000000
    jae fail

    ; CLOCK_REALTIME is anchored on the CMOS RTC, so its seconds field must
    ; be at least the monotonic count and still carry a sane tv_nsec.
    mov eax, HAKU64_SYS_CLOCK_GETTIME
    mov edi, HAKU64_CLOCK_REALTIME
    lea rsi, [rsp + 16]
    syscall
    test rax, rax
    jne fail
    cmp qword [rsp + 24], 1000000000
    jae fail
    mov rax, [rsp + 16]
    cmp rax, [rsp]
    jb fail

    ; mmap(NULL, 0x2000, R+W, PRIVATE|ANONYMOUS, -1, 0) returns a user
    ; address that reads back what was written to both pages.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x2000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    test rax, rax
    jle fail
    mov r12, rax
    mov rax, 0x1122334455667788
    mov [r12], rax
    mov [r12 + 0x1000], rax
    cmp [r12], rax
    jne fail
    cmp [r12 + 0x1000], rax
    jne fail

    ; munmap(base, 0x2000) releases both pages.
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r12
    mov esi, 0x2000
    syscall
    test rax, rax
    jne fail

    ; MAP_FIXED at a free page inside the anonymous window lands exactly
    ; at the requested address.
    mov eax, HAKU64_SYS_MMAP
    mov r14, USER64_FIXED
    mov rdi, r14
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS | HAKU64_MAP_FIXED
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, r14
    jne fail
    mov rax, 0x55AA55AA55AA55AA
    mov [r14], rax
    cmp [r14], rax
    jne fail

    ; Repeating it over the same range is refused: MAP_FIXED maps free
    ; ranges only, it does not replace an existing mapping.
    mov eax, HAKU64_SYS_MMAP
    mov rdi, USER64_FIXED
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS | HAKU64_MAP_FIXED
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; An unaligned MAP_FIXED address is -EINVAL.
    mov eax, HAKU64_SYS_MMAP
    lea rdi, [r14 + 1]
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS | HAKU64_MAP_FIXED
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; MAP_SHARED | MAP_ANONYMOUS is accepted and behaves like a private
    ; anonymous page until there is a fork to share it with.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_SHARED | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    test rax, rax
    jle fail
    mov r13, rax
    mov rax, 0x1234567890ABCDEF
    mov [r13], rax
    cmp [r13], rax
    jne fail

    ; MAP_PRIVATE together with MAP_SHARED is -EINVAL.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_SHARED | HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; Release both of those pages again.
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r14
    mov esi, 0x1000
    syscall
    test rax, rax
    jne fail
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r13
    mov esi, 0x1000
    syscall
    test rax, rax
    jne fail

    ; ---- file store and file-backed mmap -----------------------------
    ; open("/hello.txt", O_RDWR) hands out a descriptor above 2. It has to
    ; be writable because the MAP_SHARED test below stores through the
    ; mapping, and the kernel requires a writable descriptor for that.
    mov rbx, FILE_BUF
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_hello]
    mov esi, HAKU64_O_RDWR
    xor edx, edx
    syscall
    test rax, rax
    jle fail
    cmp rax, 2
    jbe fail
    mov r15, rax

    ; read() returns the file bytes; printing them is the HAKU64-FILE-OK
    ; marker, so the console line itself is the proof.
    mov eax, HAKU64_SYS_READ
    mov rdi, r15
    mov rsi, rbx
    mov edx, 32
    syscall
    cmp rax, 16
    jne fail
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    mov rsi, rbx
    mov edx, 16
    syscall
    cmp rax, 16
    jne fail

    ; A private mapping copies the page: its bytes match the file, but a
    ; store to it does not change what read() sees.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_PRIVATE
    mov r8, r15
    xor r9d, r9d
    syscall
    test rax, rax
    jle fail
    mov r13, rax
    mov rcx, [rbx]
    cmp [r13], rcx
    jne fail
    mov rcx, [rbx + 8]
    cmp [r13 + 8], rcx
    jne fail
    mov byte [r13], 'X'
    mov eax, HAKU64_SYS_LSEEK
    mov rdi, r15
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jne fail
    mov eax, HAKU64_SYS_READ
    mov rdi, r15
    lea rsi, [rbx + 64]
    mov edx, 1
    syscall
    cmp rax, 1
    jne fail
    cmp byte [rbx + 64], 'H'
    jne fail
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r13
    mov esi, 0x1000
    syscall
    test rax, rax
    jne fail

    ; A shared mapping is the file's own page: the store is visible to
    ; read(), and munmap must not take the frame with it.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_SHARED
    mov r8, r15
    xor r9d, r9d
    syscall
    test rax, rax
    jle fail
    mov r13, rax
    mov byte [r13], 'S'
    mov eax, HAKU64_SYS_LSEEK
    mov rdi, r15
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jne fail
    mov eax, HAKU64_SYS_READ
    mov rdi, r15
    lea rsi, [rbx + 64]
    mov edx, 1
    syscall
    cmp rax, 1
    jne fail
    cmp byte [rbx + 64], 'S'
    jne fail
    mov byte [r13], 'H'              ; put the marker byte back
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r13
    mov esi, 0x1000
    syscall
    test rax, rax
    jne fail

    ; The file survived that munmap: a fresh read still returns the marker.
    mov eax, HAKU64_SYS_LSEEK
    mov rdi, r15
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jne fail
    mov eax, HAKU64_SYS_READ
    mov rdi, r15
    mov rsi, rbx
    mov edx, 16
    syscall
    cmp rax, 16
    jne fail
    lea rax, [msg_file]
    mov rcx, [rax]
    cmp rcx, [rbx]
    jne fail
    mov rcx, [rax + 8]
    cmp rcx, [rbx + 8]
    jne fail

    ; The three-page pattern file: a page-aligned offset maps the page the
    ; offset selects, and the mapping matches a read() of the same range.
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_pattern]
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jle fail
    mov r14, rax
    mov eax, HAKU64_SYS_LSEEK
    mov rdi, r14
    mov esi, 0x1000
    xor edx, edx
    syscall
    cmp rax, 0x1000
    jne fail
    mov eax, HAKU64_SYS_READ
    mov rdi, r14
    mov rsi, rbx
    mov edx, 16
    syscall
    cmp rax, 16
    jne fail
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE
    mov r8, r14
    mov r9d, 0x1000
    syscall
    test rax, rax
    jle fail
    mov r13, rax
    mov rcx, [rbx]
    cmp [r13], rcx
    jne fail
    mov rcx, [rbx + 8]
    cmp [r13 + 8], rcx
    jne fail
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r13
    mov esi, 0x1000
    syscall
    test rax, rax
    jne fail

    ; A non-page-aligned offset and a range past the end of the file are
    ; both -EINVAL: there is no SIGBUS model to report a late access.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE
    mov r8, r14
    mov r9d, 0x10
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE
    mov r8, r14
    mov r9d, 0x3000
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; close() drops the descriptor, so read() and mmap() are -EBADF;
    ; open() of an unknown name is -ENOENT and a write open is -EINVAL.
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r14
    syscall
    test rax, rax
    jne fail
    mov eax, HAKU64_SYS_READ
    mov rdi, r14
    mov rsi, rbx
    mov edx, 1
    syscall
    cmp rax, -HAKU64_EBADF
    jne fail
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE
    mov r8, r14
    xor r9d, r9d
    syscall
    cmp rax, -HAKU64_EBADF
    jne fail
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_missing]
    xor esi, esi
    xor edx, edx
    syscall
    cmp rax, -HAKU64_ENOENT
    jne fail
    ; O_LARGEFILE/O_CLOEXEC/O_NOFOLLOW are dropped because a libc sets them
    ; by default; an unknown flag bit is still -EINVAL.
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_hello]
    mov esi, 0x200000
    xor edx, edx
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; ---- FAT12 disk: mkdir, create, write, read back ------------------
    ; /TMP does not exist on the fresh seed volume; a rerun against a
    ; volume that still has it must also pass.
    mov r15d, 20
    mov eax, HAKU64_SYS_MKDIR
    lea rdi, [path_tmp]
    xor esi, esi
    syscall
    test rax, rax
    jz .tmp_ready
    cmp rax, -HAKU64_EEXIST
    jne fail
.tmp_ready:

    mov r15d, 21
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_note]
    mov esi, HAKU64_O_CREAT | HAKU64_O_TRUNC | HAKU64_O_RDWR
    xor edx, edx
    syscall
    test rax, rax
    jle fail
    mov r14, rax

    mov r15d, 22
    mov eax, HAKU64_SYS_WRITE
    mov rdi, r14
    lea rsi, [msg_disk]
    mov edx, msg_disk_end - msg_disk
    syscall
    cmp rax, msg_disk_end - msg_disk
    jne fail

    ; Read the bytes back through the descriptor before closing it: this
    ; is the page cache, and close() is what flushes it to the volume.
    mov r15d, 23
    mov eax, HAKU64_SYS_LSEEK
    mov rdi, r14
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jne fail
    mov r15d, 24
    mov eax, HAKU64_SYS_READ
    mov rdi, r14
    mov rsi, NOTE_BUF
    mov edx, msg_disk_end - msg_disk
    syscall
    cmp rax, msg_disk_end - msg_disk
    jne fail
    lea rax, [msg_disk]
    mov rcx, [rax]
    mov rdx, NOTE_BUF
    cmp rcx, [rdx]
    jne fail
    mov rcx, [rax + 8]
    cmp rcx, [rdx + 8]
    jne fail
    mov r15d, 25
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r14
    syscall
    test rax, rax
    jne fail

    ; The marker is the file's own contents, printed after the flush.
    mov r15d, 26
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    mov rsi, NOTE_BUF
    mov edx, msg_disk_end - msg_disk
    syscall
    cmp rax, msg_disk_end - msg_disk
    jne fail

    ; "." and ".." are real path components on the volume, not text the
    ; kernel strips: /TMP/../HELLO.TXT and /./HELLO.TXT both reach the
    ; seeded file.
    mov r15d, 27
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_dotdot]
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jle fail
    mov r14, rax
    mov r15d, 28
    mov eax, HAKU64_SYS_READ
    mov rdi, r14
    mov rsi, REREAD_BUF
    mov edx, msg_file_end - msg_file
    syscall
    cmp rax, msg_file_end - msg_file
    jne fail
    lea rax, [msg_file]
    mov rcx, [rax]
    mov rdx, REREAD_BUF
    cmp rcx, [rdx]
    jne fail
    mov rcx, [rax + 8]
    cmp rcx, [rdx + 8]
    jne fail
    mov r15d, 29
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r14
    syscall
    test rax, rax
    jne fail

    mov r15d, 30
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_dot]
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jle fail
    mov r14, rax
    mov r15d, 31
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r14
    syscall
    test rax, rax
    jne fail

    ; ---- fork: a MAP_SHARED page has a second observer -----------------
    ; The parent stores a word, the child sees it and overwrites it, and
    ; the parent sees the child's value after wait4(). That is only true
    ; if fork aliases the shared frame instead of copying it.
    mov r15d, 32
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_SHARED | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    test rax, rax
    jle fail
    mov r12, rax
    mov rax, 0x1111222233334444
    mov [r12], rax

    mov r15d, 33
    mov eax, HAKU64_SYS_FORK
    syscall
    test rax, rax
    js fail
    jz child_proc

    ; Parent: wait for the child and check its exit status is 7.
    mov r13, rax
    mov r15d, 34
    mov eax, HAKU64_SYS_WAIT4
    mov rdi, r13
    mov rsi, STATUS_BUF
    xor edx, edx
    syscall
    cmp rax, r13
    jne fail
    mov rdx, STATUS_BUF
    mov rax, [rdx]
    shr rax, 8
    and eax, 0xFF
    cmp eax, 7
    jne fail

    ; The child's store is visible in the parent's mapping.
    mov r15d, 35
    mov rax, 0xAAAABBBBCCCCDDDD
    cmp [r12], rax
    jne fail

    mov r15d, 36
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r12
    mov esi, 0x1000
    syscall
    test rax, rax
    jne fail

    mov r15d, 37
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_fork]
    mov edx, msg_fork_end - msg_fork
    syscall
    cmp rax, msg_fork_end - msg_fork
    jne fail
    jmp fork_done

child_proc:
    mov rax, 0x1111222233334444
    cmp [r12], rax
    jne fail
    mov rax, 0xAAAABBBBCCCCDDDD
    mov [r12], rax
    mov eax, HAKU64_SYS_EXIT
    mov edi, 7
    syscall
    jmp $

fork_done:

    ; ---- fork: a private page is shared copy-on-write -----------------
    ; The parent stores a word and forks. The child must see the original
    ; and may overwrite it without the parent seeing the store: that is
    ; only true if fork shares the frame read-only and each writer copies
    ; it. Reaping the child returns the count to one, so the parent's own
    ; store can simply drop the write protection.
    mov r15d, 60
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    test rax, rax
    jle fail
    mov r12, rax
    mov rax, 0x1111222233334444
    mov [r12], rax

    mov r15d, 61
    mov eax, HAKU64_SYS_FORK
    syscall
    test rax, rax
    js fail
    jz cow_child

    mov r13, rax
    mov r15d, 62
    mov eax, HAKU64_SYS_WAIT4
    mov rdi, r13
    mov rsi, STATUS_BUF
    xor edx, edx
    syscall
    cmp rax, r13
    jne fail
    mov rdx, STATUS_BUF
    mov rax, [rdx]
    shr rax, 8
    and eax, 0xFF
    cmp eax, 9
    jne fail

    ; The child wrote its own copy, so the parent's word is untouched.
    mov r15d, 63
    mov rax, 0x1111222233334444
    cmp [r12], rax
    jne fail

    ; And a parent store now gets the page back, writable.
    mov rax, 0x5555666677778888
    mov [r12], rax
    cmp [r12], rax
    jne fail

    mov r15d, 64
    mov eax, HAKU64_SYS_MUNMAP
    mov rdi, r12
    mov esi, 0x1000
    syscall
    test rax, rax
    jne fail

    mov r15d, 65
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_cow]
    mov edx, msg_cow_end - msg_cow
    syscall
    cmp rax, msg_cow_end - msg_cow
    jne fail
    jmp cow_done

cow_child:
    mov rax, 0x1111222233334444
    cmp [r12], rax
    jne fail
    mov rax, 0xAAAABBBBCCCCDDDD
    mov [r12], rax
    cmp [r12], rax
    jne fail
    mov eax, HAKU64_SYS_EXIT
    mov edi, 9
    syscall
    jmp $

cow_done:

    ; ---- dup: two descriptors, one description ------------------------
    ; HELLO.TXT starts with "HAKU64-FILE-OK\r\n". Read 4 bytes, dup the
    ; descriptor, then read the next 4 through the duplicate: they are
    ; "64-F" only if both descriptors share one file offset.
    mov r15d, 38
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_hello]
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jle fail
    mov r14, rax
    mov r15d, 39
    mov rbx, FILE_BUF
    mov eax, HAKU64_SYS_READ
    mov rdi, r14
    mov rsi, rbx
    mov edx, 4
    syscall
    cmp rax, 4
    jne fail
    cmp dword [rbx], 0x554B4148            ; "HAKU"
    jne fail

    mov r15d, 40
    mov eax, HAKU64_SYS_DUP
    mov rdi, r14
    syscall
    test rax, rax
    jle fail
    mov r13, rax
    mov r15d, 41
    mov rbx, REREAD_BUF
    mov eax, HAKU64_SYS_READ
    mov rdi, r13
    mov rsi, rbx
    mov edx, 4
    syscall
    cmp rax, 4
    jne fail
    cmp dword [rbx], 0x462D3436            ; "64-F"
    jne fail

    ; dup2 puts the same description on a chosen descriptor, and the offset
    ; stays shared after the original is closed.
    mov r15d, 42
    mov eax, HAKU64_SYS_DUP2
    mov rdi, r14
    mov esi, 9
    syscall
    cmp rax, 9
    jne fail
    mov r15d, 43
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r14
    syscall
    test rax, rax
    jne fail
    mov r15d, 44
    mov rbx, FILE_BUF
    mov eax, HAKU64_SYS_READ
    mov edi, 9
    mov rsi, rbx
    mov edx, 3
    syscall
    cmp rax, 3
    jne fail
    cmp byte [rbx], 0x49                   ; "ILE" follows "HAKU64-F"
    jne fail
    cmp byte [rbx + 1], 0x4C
    jne fail
    cmp byte [rbx + 2], 0x45
    jne fail
    mov r15d, 45
    mov eax, HAKU64_SYS_CLOSE
    mov edi, 9
    syscall
    test rax, rax
    jne fail
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r13
    syscall
    test rax, rax
    jne fail

    ; dup() of the console hands out a descriptor that still writes to COM1,
    ; so the marker below is itself the proof.
    mov r15d, 46
    mov eax, HAKU64_SYS_DUP
    mov edi, 1
    syscall
    test rax, rax
    jle fail
    mov r13, rax
    mov eax, HAKU64_SYS_WRITE
    mov rdi, r13
    lea rsi, [msg_dup]
    mov edx, msg_dup_end - msg_dup
    syscall
    cmp rax, msg_dup_end - msg_dup
    jne fail
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r13
    syscall
    test rax, rax
    jne fail

    ; ---- nanosleep advances CLOCK_MONOTONIC by at least the request ----
    mov rdx, STATUS_BUF
    mov r15d, 47
    mov eax, HAKU64_SYS_CLOCK_GETTIME
    mov edi, HAKU64_CLOCK_MONOTONIC
    mov rsi, rdx
    syscall
    test rax, rax
    jne fail
    mov r12, [rdx]                         ; seconds
    mov rax, 1000000000
    imul r12, rax
    add r12, [rdx + 8]                     ; nanoseconds

    mov r15d, 48
    mov dword [rdx], 0                     ; 10 ms requested
    mov dword [rdx + 4], 0
    mov dword [rdx + 8], 10000000
    mov dword [rdx + 12], 0
    mov eax, HAKU64_SYS_NANOSLEEP
    mov rdi, rdx
    xor esi, esi
    syscall
    test rax, rax
    jne fail
    mov r15d, 49
    mov eax, HAKU64_SYS_CLOCK_GETTIME
    mov edi, HAKU64_CLOCK_MONOTONIC
    mov rsi, rdx
    syscall
    test rax, rax
    jne fail
    mov r13, [rdx]
    mov rax, 1000000000
    imul r13, rax
    add r13, [rdx + 8]
    sub r13, r12
    cmp r13, 10000000
    jb fail

    ; A nanosecond field outside [0, 1e9) is -EINVAL.
    mov r15d, 50
    mov dword [rdx + 8], 1000000000
    mov eax, HAKU64_SYS_NANOSLEEP
    mov rdi, rdx
    xor esi, esi
    syscall
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; ---- fork + execve: run a program that lives on the disk ----------
    ; USER.ELF is not linked into the kernel, so the only way this line can
    ; print is if execve read it from the FAT12 volume, validated it and
    ; replaced the child's address space with it.
    mov r15d, 51
    mov eax, HAKU64_SYS_FORK
    syscall
    test rax, rax
    js fail
    jz exec_child

    mov r13, rax
    mov r15d, 52
    mov eax, HAKU64_SYS_WAIT4
    mov rdi, r13
    mov rsi, STATUS_BUF
    xor edx, edx
    syscall
    cmp rax, r13
    jne fail
    mov r15d, 53
    mov rbx, STATUS_BUF
    mov rax, [rbx]
    shr rax, 8
    and eax, 0xFF
    cmp eax, 42
    jne fail
    mov r15d, 54
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    lea rsi, [msg_exec]
    mov edx, msg_exec_end - msg_exec
    syscall
    cmp rax, msg_exec_end - msg_exec
    jne fail
    jmp exec_done

exec_child:
    ; A failed execve must leave the image running: /NOPE does not exist, so
    ; this returns -ENOENT and the child keeps executing these instructions.
    mov r15d, 55
    mov eax, HAKU64_SYS_EXECVE
    lea rdi, [path_missing]
    xor esi, esi
    xor edx, edx
    syscall
    cmp rax, -HAKU64_ENOENT
    jne exec_bad

    ; Now the real thing: argv = {"/USER.ELF", "alpha", "beta", NULL} and
    ; envp = {"A=1", NULL}. The stack the child gets built by execve is what
    ; USER.ELF reads, so status 42 proves argc/argv/envp landed correctly.
    mov rbx, EXEC_ARGV
    lea rax, [path_user]
    mov [rbx], rax
    lea rax, [exec_arg1]
    mov [rbx + 8], rax
    lea rax, [exec_arg2]
    mov [rbx + 16], rax
    mov qword [rbx + 24], 0
    mov rdx, EXEC_ENVP
    lea rax, [exec_env1]
    mov [rdx], rax
    mov qword [rdx + 8], 0

    mov r15d, 56
    mov rsi, rbx
    mov eax, HAKU64_SYS_EXECVE
    lea rdi, [path_user]
    syscall
exec_bad:
    ; execve only returns when it failed; 99 is a status the parent rejects.
    mov eax, HAKU64_SYS_EXIT
    mov edi, 99
    syscall
    jmp $

exec_done:

    ; ---- threads: clone(CLONE_VM|FILES|THREAD|SIGHAND) ----------------
    ; The child gets its own stack but the same address space, so the words
    ; it stores below are the words the parent reads: that is what separates
    ; a thread from fork. The futex make the parent sleep instead of spin.
    mov r15d, 57
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x2000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    test rax, rax
    jle fail
    add rax, 0x2000                ; top of the thread stack, page aligned
    mov r13, rax

    ; Start from a known state: BSS is zero, but a re-run would not be.
    mov rbx, THREAD_FLAG
    mov qword [rbx], 0
    mov qword [rbx + 8], 0         ; THREAD_COUNT
    mov qword [rbx + 16], 0        ; THREAD_TIDS[0]
    mov qword [rbx + 24], 0        ; THREAD_TIDS[1]

    mov r15d, 58
    mov eax, HAKU64_SYS_CLONE
    mov edi, HAKU64_CLONE_VM | HAKU64_CLONE_FILES | HAKU64_CLONE_SIGHAND | HAKU64_CLONE_THREAD
    mov rsi, r13
    xor edx, edx
    xor r10d, r10d
    xor r8d, r8d
    syscall
    test rax, rax
    js fail
    jz thread_child
    mov r13, rax                   ; the child's tid

    ; Wait for the child's flag, asleep. A wake races with the check, so the
    ; loop re-reads the word and tolerates EAGAIN, like real futex users.
    mov r15d, 59
    mov rbx, THREAD_FLAG
.thread_wait:
    mov rax, [rbx]
    test rax, rax
    jnz .thread_awake
    mov rdi, rbx
    mov eax, HAKU64_SYS_FUTEX
    mov esi, HAKU64_FUTEX_WAIT
    xor edx, edx
    xor r10d, r10d
    xor r8d, r8d
    xor r9d, r9d
    syscall
    test rax, rax
    je .thread_wait
    cmp rax, -HAKU64_EAGAIN
    je .thread_wait
    jmp fail
.thread_awake:

    ; The child's locked increment landed in the shared page.
    mov r15d, 60
    mov rbx, THREAD_TIDS
    cmp qword [rbx - 8], 1
    jne fail

    ; gettid() in the child is the tid clone() returned.
    mov r15d, 61
    mov rax, [rbx]
    cmp rax, r13
    jne fail

    ; getpid() is the same process id in both tasks (CLONE_THREAD).
    mov r15d, 62
    mov eax, HAKU64_SYS_GETPID
    syscall
    cmp rax, [rbx + 8]
    jne fail

    ; CLONE_FILES: the child's dup2(1, 9) is visible here, in the parent's
    ; own descriptor table, and fd 9 is the console.
    mov r15d, 63
    mov eax, HAKU64_SYS_WRITE
    mov edi, 9
    lea rsi, [msg_thread]
    mov edx, msg_thread_end - msg_thread
    syscall
    cmp rax, msg_thread_end - msg_thread
    jne fail
    mov eax, HAKU64_SYS_CLOSE
    mov edi, 9
    syscall

    ; The thread is a child like any other: wait4 reaps it and reports its
    ; status, which is 7 only when its whole sequence ran.
    mov r15d, 64
    mov eax, HAKU64_SYS_WAIT4
    mov rdi, r13
    mov rsi, STATUS_BUF
    xor edx, edx
    syscall
    cmp rax, r13
    jne fail
    mov r15d, 65
    mov rbx, STATUS_BUF
    mov rax, [rbx]
    shr rax, 8
    and eax, 0xFF
    cmp eax, 7
    jne fail

    ; ---- metadata, paths, directories and the rest of the port ----------
    ; stat() of a seeded file: a regular 16-byte file (S_IFREG in st_mode).
    mov r15d, 71
    mov eax, HAKU64_SYS_STAT
    lea rdi, [path_hello]
    mov rsi, STAT_BUF
    syscall
    test rax, rax
    jne fail
    mov rbx, STAT_BUF
    cmp qword [rbx + 48], 16           ; st_size
    jne fail
    mov eax, dword [rbx + 24]          ; st_mode
    and eax, 0xF000
    cmp eax, 0x8000
    jne fail

    ; fstat() on the same file through a descriptor.
    mov r15d, 72
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_hello]
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    js fail
    mov r13, rax
    mov r15d, 73
    mov eax, HAKU64_SYS_FSTAT
    mov rdi, r13
    mov rsi, STAT_BUF
    syscall
    test rax, rax
    jne fail
    mov rbx, STAT_BUF
    cmp qword [rbx + 48], 16
    jne fail
    mov r15d, 74
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r13
    syscall
    test rax, rax
    jne fail

    ; statx() fills the 256-byte modern layout; access() accepts F_OK|R_OK.
    mov r15d, 75
    mov eax, HAKU64_SYS_STATX
    mov edi, HAKU64_AT_FDCWD
    lea rsi, [path_hello]
    xor edx, edx
    mov r10d, 0x7FF
    mov r8, STAT_BUF
    syscall
    test rax, rax
    jne fail
    mov rbx, STAT_BUF
    cmp qword [rbx + 40], 16           ; stx_size
    jne fail
    mov r15d, 76
    mov eax, HAKU64_SYS_ACCESS
    lea rdi, [path_hello]
    mov esi, HAKU64_F_OK | HAKU64_R_OK
    syscall
    test rax, rax
    jne fail

    ; chdir(/TMP), getcwd() reports the canonical path, and a relative name
    ; now resolves under it.
    mov r15d, 77
    mov eax, HAKU64_SYS_CHDIR
    lea rdi, [path_tmp]
    syscall
    test rax, rax
    jne fail
    mov r15d, 78
    mov eax, HAKU64_SYS_GETCWD
    mov rdi, CWD_BUF
    mov esi, 64
    syscall
    mov rbx, CWD_BUF
    cmp rax, rbx
    jne fail
    cmp dword [rbx], 0x504D542F        ; "/TMP"
    jne fail
    cmp byte [rbx + 4], 0
    jne fail

    mov r15d, 79
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [name_note]               ; "NOTE.TXT", relative to /TMP
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    js fail
    mov r13, rax
    mov r15d, 80
    mov eax, HAKU64_SYS_FSTAT
    mov rdi, r13
    mov rsi, STAT_BUF
    syscall
    test rax, rax
    jne fail
    mov rbx, STAT_BUF
    cmp qword [rbx + 48], 0
    jle fail
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r13
    syscall

    ; openat() with AT_FDCWD sees the same working directory.
    mov r15d, 81
    mov eax, HAKU64_SYS_OPENAT
    mov edi, HAKU64_AT_FDCWD
    lea rsi, [name_note]
    xor edx, edx
    xor r10d, r10d
    syscall
    test rax, rax
    js fail
    mov r13, rax
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r13
    syscall

    ; mkdirat + O_DIRECTORY + getdents64 (empty new directory) + rmdir.
    mov r15d, 82
    mov eax, HAKU64_SYS_MKDIRAT
    mov edi, HAKU64_AT_FDCWD
    lea rsi, [name_subdir]
    syscall
    test rax, rax
    jne fail
    mov r15d, 83
    mov eax, HAKU64_SYS_OPENAT
    mov edi, HAKU64_AT_FDCWD
    lea rsi, [name_subdir]
    mov edx, HAKU64_O_RDONLY | HAKU64_O_DIRECTORY
    xor r10d, r10d
    syscall
    test rax, rax
    js fail
    mov r13, rax
    mov r15d, 84
    mov eax, HAKU64_SYS_GETDENTS64
    mov rdi, r13
    mov rsi, DIR_BUF
    mov edx, 512
    syscall
    test rax, rax
    jne fail                            ; "." and ".." are skipped
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r13
    syscall
    mov r15d, 85
    mov eax, HAKU64_SYS_UNLINKAT
    mov edi, HAKU64_AT_FDCWD
    lea rsi, [name_subdir]
    mov edx, HAKU64_AT_REMOVEDIR
    syscall
    test rax, rax
    jne fail

    ; Create, ftruncate, truncate, rename and unlink a file by relative name.
    mov r15d, 86
    mov eax, HAKU64_SYS_OPENAT
    mov edi, HAKU64_AT_FDCWD
    lea rsi, [name_tmpfile]
    mov edx, HAKU64_O_CREAT | HAKU64_O_RDWR | HAKU64_O_TRUNC
    xor r10d, r10d
    syscall
    test rax, rax
    js fail
    mov r13, rax
    mov r15d, 87
    mov eax, HAKU64_SYS_WRITE
    mov rdi, r13
    lea rsi, [msg_abc]
    mov edx, 3
    syscall
    cmp rax, 3
    jne fail
    mov r15d, 88
    mov eax, HAKU64_SYS_FTRUNCATE
    mov rdi, r13
    mov esi, 1
    syscall
    test rax, rax
    jne fail
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r13
    syscall
    mov r15d, 89
    mov eax, HAKU64_SYS_TRUNCATE
    lea rdi, [name_tmpfile]
    mov esi, 2
    syscall
    test rax, rax
    jne fail
    mov r15d, 90
    mov eax, HAKU64_SYS_NEWFSTATAT
    mov edi, HAKU64_AT_FDCWD
    lea rsi, [name_tmpfile]
    mov rdx, STAT_BUF
    xor r10d, r10d
    syscall
    test rax, rax
    jne fail
    mov rbx, STAT_BUF
    cmp qword [rbx + 48], 2
    jne fail
    mov r15d, 91
    mov eax, HAKU64_SYS_RENAMEAT
    mov edi, HAKU64_AT_FDCWD
    lea rsi, [name_tmpfile]
    mov edx, HAKU64_AT_FDCWD
    lea r10, [name_renamed]
    syscall
    test rax, rax
    jne fail
    mov r15d, 92
    mov eax, HAKU64_SYS_STAT
    lea rdi, [path_tmpfile]            ; /TMP/TMPFILE.TXT is gone
    mov rsi, STAT_BUF
    syscall
    cmp rax, -HAKU64_ENOENT
    jne fail
    mov r15d, 93
    mov eax, HAKU64_SYS_UNLINKAT
    mov edi, HAKU64_AT_FDCWD
    lea rsi, [name_renamed]
    xor edx, edx
    syscall
    test rax, rax
    jne fail

    ; A non-empty directory lists its entry: open /TMP and read one record.
    mov r15d, 94
    mov eax, HAKU64_SYS_OPEN
    lea rdi, [path_tmp]
    mov esi, HAKU64_O_RDONLY | HAKU64_O_DIRECTORY
    xor edx, edx
    syscall
    test rax, rax
    js fail
    mov r13, rax
    mov r15d, 95
    mov eax, HAKU64_SYS_GETDENTS64
    mov rdi, r13
    mov rsi, DIR_BUF
    mov edx, 512
    syscall
    cmp rax, 32
    jb fail
    mov rbx, DIR_BUF
    mov eax, dword [rbx + 16]          ; reclen + type + name
    shr eax, 16
    and eax, 0xFF
    cmp eax, 8                         ; DT_REG for NOTE.TXT
    jne fail
    mov eax, HAKU64_SYS_CLOSE
    mov rdi, r13
    syscall

    ; Back to the root; getcwd reports "/".
    mov r15d, 96
    mov eax, HAKU64_SYS_CHDIR
    lea rdi, [path_root]
    syscall
    test rax, rax
    jne fail
    mov r15d, 97
    mov eax, HAKU64_SYS_GETCWD
    mov rdi, CWD_BUF
    mov esi, 64
    syscall
    mov rbx, CWD_BUF
    cmp byte [rbx], '/'
    jne fail
    cmp byte [rbx + 1], 0
    jne fail

    ; uname, gettimeofday, times, getrlimit and clock_getres.
    mov r15d, 98
    mov eax, HAKU64_SYS_UNAME
    mov rdi, UNAME_BUF
    syscall
    test rax, rax
    jne fail
    mov rbx, UNAME_BUF
    cmp dword [rbx], 0x756B6148        ; "Haku"
    jne fail

    mov r15d, 99
    mov eax, HAKU64_SYS_GETTIMEOFDAY
    mov rdi, STAT_BUF
    xor esi, esi
    syscall
    test rax, rax
    jne fail
    mov rbx, STAT_BUF
    cmp qword [rbx], 0
    jle fail

    mov r15d, 100
    mov eax, HAKU64_SYS_TIMES
    mov rdi, STATUS_BUF
    syscall
    test rax, rax
    js fail

    mov r15d, 101
    mov eax, HAKU64_SYS_GETRLIMIT
    mov edi, 7                         ; RLIMIT_NOFILE
    mov rsi, STAT_BUF
    syscall
    test rax, rax
    jne fail
    mov rbx, STAT_BUF
    cmp qword [rbx], 16
    jne fail

    mov r15d, 102
    mov eax, HAKU64_SYS_CLOCK_GETRES
    mov edi, HAKU64_CLOCK_MONOTONIC
    mov rsi, STAT_BUF
    syscall
    test rax, rax
    jne fail

    ; prctl(PR_SET_NAME) then PR_GET_NAME reports the same 5 bytes.
    mov r15d, 103
    mov eax, HAKU64_SYS_PRCTL
    mov edi, HAKU64_PR_SET_NAME
    lea rsi, [name_ring3]
    syscall
    test rax, rax
    jne fail
    mov r15d, 104
    mov eax, HAKU64_SYS_PRCTL
    mov edi, HAKU64_PR_GET_NAME
    mov rsi, STAT_BUF
    syscall
    test rax, rax
    jne fail
    mov rbx, STAT_BUF
    cmp dword [rbx], 0x676E6972       ; "ring"
    jne fail
    cmp byte [rbx + 4], '3'
    jne fail

    ; getrandom() fills the buffer and two calls differ.
    mov r15d, 105
    mov eax, HAKU64_SYS_GETRANDOM
    mov rdi, RAND_BUF
    mov esi, 8
    xor edx, edx
    syscall
    cmp rax, 8
    jne fail
    mov rbx, RAND_BUF
    mov r13, [rbx]
    mov r15d, 106
    mov eax, HAKU64_SYS_GETRANDOM
    mov rdi, RAND_BUF
    mov esi, 8
    xor edx, edx
    syscall
    cmp rax, 8
    jne fail
    mov rbx, RAND_BUF
    cmp r13, [rbx]
    je fail

    ; brk(0) reports the base; growing maps zeroed pages, mprotect retunes
    ; them, and shrinking releases them again.
    mov r15d, 107
    mov eax, HAKU64_SYS_BRK
    xor edi, edi
    syscall
    test rax, rax
    jz fail
    mov r13, rax
    lea rdi, [r13 + 0x2000]
    mov eax, HAKU64_SYS_BRK
    syscall
    lea rbx, [r13 + 0x2000]
    cmp rax, rbx
    jne fail
    mov r15d, 108
    mov qword [r13 + 0x1000], 0x1234
    cmp qword [r13 + 0x1000], 0x1234
    jne fail
    mov r15d, 109
    mov eax, HAKU64_SYS_MPROTECT
    mov rdi, r13
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    syscall
    test rax, rax
    jne fail
    mov r15d, 110
    mov eax, HAKU64_SYS_BRK
    mov rdi, r13
    syscall
    cmp rax, r13
    jne fail

    ; poll() on the console reports POLLOUT immediately.
    mov r15d, 111
    mov rbx, POLL_BUF
    mov dword [rbx], 1
    mov word [rbx + 4], HAKU64_POLLOUT
    mov word [rbx + 6], 0
    mov eax, HAKU64_SYS_POLL
    mov rdi, rbx
    mov esi, 1
    xor edx, edx
    syscall
    cmp rax, 1
    jne fail
    mov ax, [rbx + 6]
    test ax, HAKU64_POLLOUT
    jz fail

    ; select() over fd 0 (the console is always readable) returns one ready.
    mov r15d, 112
    mov rdi, DIR_BUF
    xor eax, eax
    mov ecx, 128
    rep stosb
    mov rbx, DIR_BUF
    mov byte [rbx], 1                  ; bit 0 = fd 0
    mov eax, HAKU64_SYS_SELECT
    mov edi, 1
    mov rsi, rbx
    xor edx, edx
    xor r10d, r10d
    xor r8d, r8d
    syscall
    cmp rax, 1
    jne fail
    cmp byte [rbx], 1
    jne fail

    ; sched_yield() returns 0 with no other READY task, which is allowed.
    mov r15d, 113
    mov eax, HAKU64_SYS_SCHED_YIELD
    syscall
    test rax, rax
    jne fail

    ; dup3(1, 10) is dup2 with a flag argument; fcntl reports the descriptor.
    mov r15d, 114
    mov eax, HAKU64_SYS_DUP3
    mov edi, 1
    mov esi, 10
    xor edx, edx
    syscall
    cmp rax, 10
    jne fail
    mov r15d, 115
    mov eax, HAKU64_SYS_WRITE
    mov edi, 10
    lea rsi, [msg_port]
    mov edx, msg_port_end - msg_port
    syscall
    cmp rax, msg_port_end - msg_port
    jne fail
    mov eax, HAKU64_SYS_CLOSE
    mov edi, 10
    syscall
    mov r15d, 116
    mov eax, HAKU64_SYS_FCNTL
    mov edi, 1
    mov esi, HAKU64_F_GETFD
    xor edx, edx
    syscall
    test rax, rax
    jne fail
    mov r15d, 117
    mov eax, HAKU64_SYS_FCNTL
    mov edi, 1
    mov esi, HAKU64_F_GETFL
    xor edx, edx
    syscall
    test rax, rax
    js fail

    ; ---- error paths through the INT 0x80 gate -------------------------
    ; write() to a descriptor other than 1 or 2 is -EBADF.
    mov eax, HAKU64_SYS_WRITE
    mov edi, 99
    lea rsi, [msg_boot]
    mov edx, 1
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EBADF
    jne fail

    ; A pointer outside the user half is -EFAULT, not a kernel fault.
    mov eax, HAKU64_SYS_WRITE
    mov edi, 1
    mov esi, 0x10
    mov edx, 1
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EFAULT
    jne fail

    ; An unimplemented number is -ENOSYS.
    mov eax, 9999
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_ENOSYS
    jne fail

    ; mmap with W+X, a real descriptor, or a non-zero offset is -EINVAL.
    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ | HAKU64_PROT_WRITE | HAKU64_PROT_EXEC
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EINVAL
    jne fail

    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    xor r8d, r8d
    xor r9d, r9d
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EINVAL
    jne fail

    mov eax, HAKU64_SYS_MMAP
    xor edi, edi
    mov esi, 0x1000
    mov edx, HAKU64_PROT_READ
    mov r10d, HAKU64_MAP_PRIVATE | HAKU64_MAP_ANONYMOUS
    mov r8, -1
    mov r9d, 1
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; munmap of an unaligned address is -EINVAL.
    mov eax, HAKU64_SYS_MUNMAP
    lea rdi, [r12 + 1]
    mov esi, 0x1000
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EINVAL
    jne fail

    ; clock_gettime() to an unmapped pointer is -EFAULT.
    mov eax, HAKU64_SYS_CLOCK_GETTIME
    mov edi, HAKU64_CLOCK_MONOTONIC
    mov esi, 0x10
    int HAKU64_SYSCALL_VECTOR
    cmp rax, -HAKU64_EFAULT
    jne fail

    ; writev(1, {"HAKU64-ABI-OK", "\n"}, 2) is the ABI success marker.
    lea rax, [msg_abi]
    mov [rsp], rax
    mov qword [rsp + 8], msg_abi_end - msg_abi
    lea rax, [msg_nl]
    mov [rsp + 16], rax
    mov qword [rsp + 24], 1
    mov eax, HAKU64_SYS_WRITEV
    mov edi, 1
    mov rsi, rsp
    mov edx, 2
    syscall
    cmp rax, (msg_abi_end - msg_abi) + (msg_nl_end - msg_nl)
    jne fail

    ; The BSS store is read back by the kernel after the task exits.
    mov rax, USER64_BSS
    mov dword [rax], 0x48414B55       ; "HAKU", read back by the kernel
    mov eax, HAKU64_SYS_EXIT
    xor edi, edi
    syscall

.hang:
    jmp .hang

; The clone child: same address space, its own stack, and its own copy of
; every register except RAX (0) and RSP (the stack clone() was given).
thread_child:
    mov r15d, 66
    mov rbx, THREAD_COUNT
    lock inc qword [rbx]

    mov r15d, 67
    mov eax, HAKU64_SYS_GETTID
    syscall
    test rax, rax
    jle thread_bad
    mov rbx, THREAD_TIDS
    mov [rbx], rax

    mov r15d, 68
    mov eax, HAKU64_SYS_GETPID
    syscall
    test rax, rax
    jle thread_bad
    mov [rbx + 8], rax

    ; CLONE_FILES: this dup2 updates the table the parent is using too.
    mov r15d, 69
    mov eax, HAKU64_SYS_DUP2
    mov edi, 1
    mov esi, 9
    syscall
    cmp rax, 9
    jne thread_bad

    ; Publish the word the parent is asleep on, then wake it.
    mov r15d, 70
    mov rbx, THREAD_FLAG
    mov qword [rbx], 1
    mov rdi, rbx
    mov eax, HAKU64_SYS_FUTEX
    mov esi, HAKU64_FUTEX_WAKE
    mov edx, 1
    xor r10d, r10d
    xor r8d, r8d
    xor r9d, r9d
    syscall

    mov eax, HAKU64_SYS_EXIT
    mov edi, 7
    syscall

thread_bad:
    mov rdi, r15
    mov eax, HAKU64_SYS_EXIT
    syscall
    jmp $

; R15 holds the step number from the header; exiting with it names the check
; that failed. The INT 0x80 gate is used so this path exercises both entry
; points even when the fast path is what broke.
fail:
    mov rdi, r15
    mov eax, HAKU64_SYS_EXIT
    int HAKU64_SYSCALL_VECTOR
    jmp $

msg_boot:
    db 'ELF64', 10
msg_boot_end:

msg_sys64:
    db 'HAKU64-SYSCALL64-OK', 10
msg_sys64_end:

msg_file:
    db 'HAKU64-FILE-OK', 13, 10
msg_file_end:

msg_disk:
    db 'HAKU64-DISK-OK', 13, 10
msg_disk_end:

msg_fork:
    db 'HAKU64-FORK-OK', 13, 10
msg_fork_end:

msg_cow:
    db 'HAKU64-COW-OK', 13, 10
msg_cow_end:

msg_dup:
    db 'HAKU64-DUP-OK', 13, 10
msg_dup_end:

msg_exec:
    db 'HAKU64-EXEC-OK', 13, 10
msg_exec_end:

msg_thread:
    db 'HAKU64-THREAD-OK', 13, 10
msg_thread_end:

msg_port:
    db 'HAKU64-PORT-OK', 13, 10
msg_port_end:

msg_abc:
    db 'abc'


path_root:
    db '/', 0

name_note:
    db 'NOTE.TXT', 0

name_subdir:
    db 'SUBDIR', 0

name_tmpfile:
    db 'TMPFILE.TXT', 0

name_renamed:
    db 'RENAMED.TXT', 0

path_tmpfile:
    db '/TMP/TMPFILE.TXT', 0

name_ring3:
    db 'ring3', 0

path_hello:
    db '/hello.txt', 0

path_pattern:
    db '/pattern.bin', 0

path_missing:
    db '/nope', 0

path_tmp:
    db '/TMP', 0

path_note:
    db '/TMP/NOTE.TXT', 0

path_dotdot:
    db '/TMP/../HELLO.TXT', 0

path_dot:
    db '/./HELLO.TXT', 0

path_user:
    db '/USER.ELF', 0

exec_arg1:
    db 'alpha', 0

exec_arg2:
    db 'beta', 0

exec_env1:
    db 'A=1', 0

msg_abi:
    db 'HAKU64-ABI-OK'
msg_abi_end:

msg_nl:
    db 10
msg_nl_end:
