; Minimal static ELF32/i386 executable used to exercise the ELF loader.
[bits 32]
[org 0]

%define USER_CODE_BASE 0x40000000
%define USER_DATA_BASE 0x40003000
%define USER_STACK_TOP 0xC0000000

%include "haku/abi.inc"

; ELF32 header
    db 0x7F, 'E', 'L', 'F'
    db 1, 1, 1, 0
    times 8 db 0
    dw 2                          ; ET_EXEC
    dw 3                          ; EM_386
    dd 1                          ; EV_CURRENT
    dd USER_CODE_BASE             ; entry point
    dd 52                         ; program-header table offset
    dd 0                          ; no section table
    dd 0                          ; flags
    dw 52                         ; ELF header size
    dw 32                         ; program-header size
    dw 2                          ; two PT_LOAD segments
    dw 40, 0, 0                   ; no section headers

; RX code segment: file offset 0x1000, virtual address 0x40000000.
; Three 4 KiB pages are reserved for the code so the tests can grow.
    dd 1, 0x1000, USER_CODE_BASE, USER_CODE_BASE
    dd code_end - code_start, code_end - code_start
    dd 5, 0x1000

; RW data segment: file offset 0x4000, virtual address 0x40003000.
    dd 1, 0x4000, USER_DATA_BASE, USER_DATA_BASE
    dd data_file_end - data_start, data_end - data_start
    dd 6, 0x1000

    times 0x1000 - ($ - $$) db 0
code_start:
_user_program_start:
    ; write(1, startup, startup_len)
    mov esi, 1
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (startup_message - data_start)
    mov edx, startup_message_end - startup_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Both scheduled processes must receive distinct valid PIDs.
    mov esi, 2
    mov eax, SYS_GETPID
    int 0x80
    cmp eax, 1
    jb .fail
    cmp eax, 2
    ja .fail

    ; The data segment's BSS tail must be zero-filled by the ELF loader.
    mov esi, 2
    cmp dword [USER_DATA_BASE + (bss_word - data_start)], 0
    jne .fail
    mov dword [USER_DATA_BASE + (bss_word - data_start)], 0x12345678
    cmp dword [USER_DATA_BASE + (bss_word - data_start)], 0x12345678
    jne .fail

    ; Unmapped, kernel, cross-page, and wrapping ranges return -EFAULT.
    mov esi, 3
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, 0x1000
    mov edx, 4
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 4
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, 0x100000
    mov edx, 1
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 5
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + 0xFFF
    mov edx, 2
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 6
    mov eax, SYS_READ
    mov ebx, 0
    mov ecx, 0xFFFFFFFC
    mov edx, 8
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 7
    mov eax, SYS_READ
    mov ebx, 0
    mov ecx, 0x1000
    mov edx, 1
    int 0x80
    cmp eax, -14
    jne .fail
    ; An RX code page is readable but must not be a read() destination.
    mov eax, SYS_READ
    xor ebx, ebx
    mov ecx, USER_CODE_BASE
    mov edx, 1
    int 0x80
    cmp eax, -14
    jne .fail

    ; Invalid fd, unknown syscall, and oversized I/O return errno values.
    mov esi, 8
    mov eax, SYS_WRITE
    mov ebx, 99
    mov ecx, USER_DATA_BASE + (startup_message - data_start)
    mov edx, 1
    int 0x80
    cmp eax, -9
    jne .fail
    mov esi, 9
    mov eax, 999
    int 0x80
    cmp eax, -38
    jne .fail
    ; A count above the kernel's internal chunk size is legal: Linux serves a
    ; big read or write in pieces and reports what it moved, which is what a
    ; libc relies on (busybox cat reads 64 KiB at a time). The count is no
    ; longer rejected before the descriptor is looked at, so a bad descriptor
    ; now reports EBADF where it used to report EINVAL.
    mov esi, 10
    mov eax, SYS_WRITE
    mov ebx, 77                 ; never opened
    mov ecx, USER_CODE_BASE     ; the code segment is large enough for 4097
    mov edx, 4097
    int 0x80
    cmp eax, -9                 ; EBADF
    jne .fail

    ; Invalid open flag combinations and bad descriptors fail distinctly.
    mov esi, 30
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (file_path_one - data_start)
    mov ecx, 0x80              ; O_EXCL without O_CREAT
    xor edx, edx
    int 0x80
    cmp eax, -22               ; EINVAL
    jne .fail
    mov esi, 31
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (file_path_one - data_start)
    mov ecx, 0x200             ; O_TRUNC with O_RDONLY
    xor edx, edx
    int 0x80
    cmp eax, -22               ; EINVAL
    jne .fail
    mov esi, 32
    mov eax, SYS_CLOSE
    mov ebx, 99
    int 0x80
    cmp eax, -9                ; EBADF
    jne .fail

    mov esi, 11
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (checks_message - data_start)
    mov edx, checks_message_end - checks_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; read(0, NULL, 0) is valid; read one arbitrary key into user stack.
    mov esi, 12
    mov eax, SYS_READ
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 13
    mov eax, SYS_READ
    xor ebx, ebx
    mov ecx, USER_STACK_TOP - 16
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail

    mov esi, 14
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (read_message - data_start)
    mov edx, read_message_end - read_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Create a per-process FAT12 file, write across a sector boundary,
    ; seek and read it back, then close and reopen it read-only.
    mov esi, 15
    mov eax, SYS_GETPID
    int 0x80
    cmp eax, 1
    je .file_pid_one
    mov ebx, USER_DATA_BASE + (file_path_two - data_start)
    jmp .file_open
.file_pid_one:
    mov ebx, USER_DATA_BASE + (file_path_one - data_start)
.file_open:
    mov [USER_DATA_BASE + (file_path_selected - data_start)], ebx
    mov esi, 15
    mov eax, SYS_OPEN
    xor ecx, ecx               ; O_RDONLY
    xor edx, edx
    int 0x80
    cmp eax, -2                ; ENOENT
    jne .fail
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    mov eax, SYS_OPEN
    mov ecx, 0x242             ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov [USER_DATA_BASE + (file_fd - data_start)], eax
    mov edi, eax               ; retain descriptor across syscalls
    mov esi, 16
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, file_payload_end - file_payload
    int 0x80
    cmp eax, file_payload_end - file_payload
    jne .fail

    mov esi, 33
    mov eax, SYS_LSEEK
    mov ebx, edi               ; valid descriptor, invalid whence
    xor ecx, ecx
    mov edx, 99
    int 0x80
    cmp eax, -22               ; EINVAL, position unchanged
    jne .fail

    mov esi, 17
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    mov ecx, 0x0C2             ; O_CREAT | O_EXCL | O_RDWR
    xor edx, edx
    int 0x80
    cmp eax, -17               ; EEXIST
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    mov ecx, 1                 ; O_WRONLY
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 1
    int 0x80
    cmp eax, -9                ; EBADF for a write-only descriptor
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail

    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    mov ecx, 0x401             ; O_WRONLY | O_APPEND
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov esi, 18
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (append_payload - data_start)
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    mov edx, 2                 ; SEEK_END
    int 0x80
    cmp eax, file_payload_end - file_payload + 1
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail

    mov edi, [USER_DATA_BASE + (file_fd - data_start)]
    mov esi, 19
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx               ; SEEK_SET
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 20
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, file_payload_end - file_payload
    int 0x80
    cmp eax, file_payload_end - file_payload
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, file_payload_end - file_payload
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail

    mov esi, 21
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    mov edx, 2                 ; SEEK_END
    int 0x80
    cmp eax, file_payload_end - file_payload + 1
    jne .fail
    mov esi, 22
    mov eax, SYS_LSEEK
    mov ebx, edi
    mov ecx, -1
    mov edx, 2                 ; seek to appended byte
    int 0x80
    cmp eax, file_payload_end - file_payload
    jne .fail
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    cmp byte [USER_DATA_BASE + (file_readback - data_start)], 'B'
    jne .fail
    mov esi, 23
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 24
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    xor ecx, ecx               ; O_RDONLY
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov esi, 24
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    cmp byte [USER_DATA_BASE + (file_readback - data_start)], 'A'
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (append_payload - data_start)
    mov edx, 1
    int 0x80
    cmp eax, -9                ; EBADF for a read-only descriptor
    jne .fail
    mov esi, 25
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 21
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (file_path_selected - data_start)]
    xor ecx, ecx               ; O_RDONLY
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov esi, 22
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 4
    int 0x80
    cmp eax, 4
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, 4
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov esi, 23
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail

    mov esi, 24
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (file_io_message - data_start)
    mov edx, file_io_message_end - file_io_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; unlink/rename/stat/fstat, including open files that are removed or
    ; renamed. Per-process names (patched with the pid digit) so the two
    ; instances never collide; shared Q-names are never created.
    mov esi, 34
    mov eax, SYS_GETPID
    int 0x80
    add eax, '0'                   ; '1' or '2' (range proven above)
    mov [USER_DATA_BASE + (unlink_path - data_start) + 1], al
    mov [USER_DATA_BASE + (rename_src - data_start) + 1], al
    mov [USER_DATA_BASE + (rename_dst - data_start) + 1], al
    mov [USER_DATA_BASE + (rename_dst2 - data_start) + 1], al
    mov [USER_DATA_BASE + (extra_path - data_start) + 1], al
    mov ebx, USER_DATA_BASE + (unlink_path - data_start)
    mov [USER_DATA_BASE + (ops_unlink - data_start)], ebx
    mov ebx, USER_DATA_BASE + (rename_src - data_start)
    mov [USER_DATA_BASE + (ops_src - data_start)], ebx
    mov ebx, USER_DATA_BASE + (rename_dst - data_start)
    mov [USER_DATA_BASE + (ops_dst - data_start)], ebx
    mov ebx, USER_DATA_BASE + (rename_dst2 - data_start)
    mov [USER_DATA_BASE + (ops_dst2 - data_start)], ebx
    mov ebx, USER_DATA_BASE + (extra_path - data_start)
    mov [USER_DATA_BASE + (ops_extra - data_start)], ebx
    ; stat on a missing name -> ENOENT
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    cmp eax, -2
    jne .fail
    ; fstat on a bad descriptor -> EBADF, stat to nowhere -> EFAULT
    mov esi, 35
    mov eax, SYS_FSTAT
    mov ebx, 99
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    cmp eax, -9
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, 0x1000                ; unmapped
    int 0x80
    cmp eax, -14
    jne .fail

    ; create, then stat size+mode and fstat the open fd
    mov esi, 36
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, 0x242               ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 10
    int 0x80
    cmp eax, 10
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start) + 20], 10
    jne .fail
    cmp word [USER_DATA_BASE + (stat_buf - data_start) + 8], STAT_MODE
    jne .fail
    cmp word [USER_DATA_BASE + (stat_buf - data_start) + 10], 1
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start) + 24], 512
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, 2                   ; O_RDWR
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_FSTAT
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start) + 20], 10
    jne .fail
    cmp word [USER_DATA_BASE + (stat_buf - data_start) + 8], STAT_MODE
    jne .fail

    ; Linux-compatible stat64 path: fstatat64(300) with AT_FDCWD fills the
    ; 96-byte i386 "struct stat64" layout (mode at 16, size at 44).
    mov eax, SYS_FSTATAT64
    mov ebx, -100                ; AT_FDCWD
    mov ecx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov edx, USER_DATA_BASE + (stat64_buf - data_start)
    xor esi, esi                 ; flags
    int 0x80
    test eax, eax
    jnz .stat64_failed
    cmp dword [USER_DATA_BASE + (stat64_buf - data_start) + 16], STAT_MODE
    jne .stat64_failed
    cmp dword [USER_DATA_BASE + (stat64_buf - data_start) + 44], 10
    jne .stat64_failed
    jmp .stat64_done
.stat64_failed:
    mov esi, 160
    jmp .fail
.stat64_done:

    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail

    ; statx(383): the modern metadata call musl uses for stat() on i386.
    ; The mask is the 4th argument (esi) and the buffer the 5th (edi), so
    ; this runs after the descriptor in edi is no longer needed.
    mov eax, SYS_STATX
    mov ebx, -100                ; AT_FDCWD
    mov ecx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov edx, 0x800               ; AT_NO_AUTOMOUNT
    mov esi, 0x7FF               ; STATX_BASIC_STATS
    mov edi, USER_DATA_BASE + (statx_buf - data_start)
    int 0x80
    test eax, eax
    jnz .statx_failed
    cmp dword [USER_DATA_BASE + (statx_buf - data_start)], 0x7FF
    jne .statx_failed
    cmp word [USER_DATA_BASE + (statx_buf - data_start) + 28], STAT_MODE
    jne .statx_failed
    cmp dword [USER_DATA_BASE + (statx_buf - data_start) + 40], 10
    jne .statx_failed
    jmp .statx_done
.statx_failed:
    mov esi, 161
    jmp .fail
.statx_done:

    ; unlink: the name and its stat entry die together
    mov esi, 37
    mov eax, SYS_UNLINK
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -2
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    cmp eax, -2
    jne .fail

    ; removed-but-open: the descriptor keeps working until close
    mov esi, 38
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    mov ecx, 0x242
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 10
    int 0x80
    cmp eax, 10
    jne .fail
    mov eax, SYS_UNLINK
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_LSEEK            ; rewind: overwrite, not append
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_WRITE            ; overwrite 5 through the open fd
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 5
    int 0x80
    cmp eax, 5
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 5
    int 0x80
    cmp eax, 5
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, 5
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov eax, SYS_FSTAT            ; size still 10: overwrite, no extend
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start) + 20], 10
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN            ; name stayed dead after close
    mov ebx, [USER_DATA_BASE + (ops_unlink - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -2
    jne .fail

    ; rename: old name dies, new name serves the same bytes
    mov esi, 39
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_src - data_start)]
    mov ecx, 0x242
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 8
    int 0x80
    cmp eax, 8
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_RENAME
    mov ebx, [USER_DATA_BASE + (ops_src - data_start)]
    mov ecx, [USER_DATA_BASE + (ops_dst - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_src - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -2
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_src - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    cmp eax, -2
    jne .fail
    mov eax, SYS_STAT
    mov ebx, [USER_DATA_BASE + (ops_dst - data_start)]
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    cmp dword [USER_DATA_BASE + (stat_buf - data_start) + 20], 8
    jne .fail

    ; renamed-but-open: the old descriptor follows the entry
    mov esi, 40
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_dst - data_start)]
    mov ecx, 2
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_RENAME
    mov ebx, [USER_DATA_BASE + (ops_dst - data_start)]
    mov ecx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 8
    int 0x80
    cmp eax, 8
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, 8
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov eax, SYS_OPEN
    mov ebx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov [USER_DATA_BASE + (ops_fd - data_start)], eax
    mov eax, SYS_READ
    mov ebx, [USER_DATA_BASE + (ops_fd - data_start)]
    mov ecx, USER_DATA_BASE + (file_readback - data_start)
    mov edx, 8
    int 0x80
    cmp eax, 8
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (file_payload - data_start)
    mov edi, USER_DATA_BASE + (file_readback - data_start)
    mov ecx, 8
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, [USER_DATA_BASE + (ops_fd - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN            ; intermediate name stayed dead
    mov ebx, [USER_DATA_BASE + (ops_dst - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -2
    jne .fail

    mov esi, 41
    mov eax, SYS_RENAME          ; missing source -> ENOENT
    mov ebx, USER_DATA_BASE + (missing_q9 - data_start)
    mov ecx, USER_DATA_BASE + (missing_q8 - data_start)
    int 0x80
    cmp eax, -2
    jne .fail
    mov eax, SYS_RENAME          ; same name -> no-op success
    mov ebx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    mov ecx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_OPEN            ; fresh file for the EEXIST probe
    mov ebx, [USER_DATA_BASE + (ops_extra - data_start)]
    mov ecx, 0x242
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_RENAME          ; onto an existing name -> EEXIST
    mov ebx, [USER_DATA_BASE + (ops_extra - data_start)]
    mov ecx, [USER_DATA_BASE + (ops_dst2 - data_start)]
    int 0x80
    cmp eax, -17
    jne .fail
    mov eax, SYS_UNLINK          ; missing name -> ENOENT
    mov ebx, USER_DATA_BASE + (missing_q9 - data_start)
    int 0x80
    cmp eax, -2
    jne .fail

    mov esi, 42
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (file_ops_message - data_start)
    mov edx, file_ops_message_end - file_ops_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; dup/dup2: descriptors may share one open file description (offset,
    ; access mode, and a refcount that keeps the file alive until the last
    ; of them closes). Per-process D-names avoid collisions between the two
    ; instances.
    mov esi, 50
    mov eax, SYS_GETPID
    int 0x80
    add eax, '0'
    mov [USER_DATA_BASE + (dup_path - data_start) + 1], al
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (dup_path - data_start)
    mov ecx, 0x242             ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov edi, eax               ; fd_a
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (file_payload - data_start)
    mov edx, 4
    int 0x80
    cmp eax, 4
    jne .fail

    ; dup -> lowest free fd, distinct from the original
    mov esi, 51
    mov eax, SYS_DUP
    mov ebx, edi
    int 0x80
    cmp eax, 3
    jb .fail
    cmp eax, edi
    je .fail
    mov [USER_DATA_BASE + (dup_fd_b - data_start)], eax

    ; shared offset: rewind through fd_a, fd_b reads from the same position
    mov esi, 52
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx               ; SEEK_SET
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_READ
    mov ebx, [USER_DATA_BASE + (dup_fd_b - data_start)]
    mov ecx, USER_DATA_BASE + (dup_buf - data_start)
    mov edx, 4
    int 0x80
    cmp eax, 4
    jne .fail
    cmp byte [USER_DATA_BASE + (dup_buf - data_start)], 'A'
    jne .fail

    ; dup2 onto a chosen free fd returns that fd
    mov esi, 53
    mov eax, SYS_DUP2
    mov ebx, edi
    mov ecx, 5
    int 0x80
    cmp eax, 5
    jne .fail

    ; dup2(fd, fd) is a no-op that returns fd
    mov esi, 54
    mov eax, SYS_DUP2
    mov ebx, edi
    mov ecx, edi
    int 0x80
    cmp eax, edi
    jne .fail

    ; dup and dup2 with an invalid source -> EBADF (target left intact)
    mov esi, 55
    mov eax, SYS_DUP
    mov ebx, 99
    int 0x80
    cmp eax, -9
    jne .fail
    mov esi, 56
    mov eax, SYS_DUP2
    mov ebx, 99
    mov ecx, 5
    int 0x80
    cmp eax, -9
    jne .fail

    ; closing the original keeps the duplicates usable (refcount)
    mov esi, 57
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, 5
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 58
    mov eax, SYS_READ
    mov ebx, [USER_DATA_BASE + (dup_fd_b - data_start)]
    mov ecx, USER_DATA_BASE + (dup_buf - data_start)
    mov edx, 4
    int 0x80
    cmp eax, 4
    jne .fail
    cmp byte [USER_DATA_BASE + (dup_buf - data_start)], 'A'
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, 5
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, [USER_DATA_BASE + (dup_fd_b - data_start)]
    int 0x80
    test eax, eax
    jne .fail

    mov esi, 59
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (dup_message - data_start)
    mov edx, dup_message_end - dup_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Standard descriptors 0/1/2 are real per-process handles backed by the
    ; console device, so dup2 can redirect them (a shell-style redirect) and
    ; close() makes them unusable (EBADF) until dup2 restores them.
    mov esi, 70
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (stdio_path - data_start)
    mov ecx, 0x242             ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fail
    mov [USER_DATA_BASE + (stdio_fd - data_start)], eax

    ; save stdout, then point fd 1 at the file.
    mov eax, SYS_DUP
    mov ebx, 1
    int 0x80
    cmp eax, 3
    jb .fail
    mov [USER_DATA_BASE + (stdio_saved - data_start)], eax
    mov eax, SYS_DUP2
    mov ebx, [USER_DATA_BASE + (stdio_fd - data_start)]
    mov ecx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (stdio_payload - data_start)
    mov edx, stdio_payload_end - stdio_payload
    int 0x80
    cmp eax, stdio_payload_end - stdio_payload
    jne .fail

    ; restore stdout, then prove the redirected bytes landed in the file.
    mov eax, SYS_DUP2
    mov ebx, [USER_DATA_BASE + (stdio_saved - data_start)]
    mov ecx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, [USER_DATA_BASE + (stdio_saved - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, [USER_DATA_BASE + (stdio_fd - data_start)]
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_READ
    mov ebx, [USER_DATA_BASE + (stdio_fd - data_start)]
    mov ecx, USER_DATA_BASE + (stdio_buf - data_start)
    mov edx, stdio_payload_end - stdio_payload
    int 0x80
    cmp eax, stdio_payload_end - stdio_payload
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (stdio_payload - data_start)
    mov edi, USER_DATA_BASE + (stdio_buf - data_start)
    mov ecx, stdio_payload_end - stdio_payload
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, [USER_DATA_BASE + (stdio_fd - data_start)]
    int 0x80
    test eax, eax
    jne .fail

    ; A closed standard descriptor is unusable (EBADF) until dup2 restores it.
    mov esi, 71
    mov eax, SYS_DUP
    mov ebx, 1
    int 0x80
    cmp eax, 3
    jb .fail
    mov [USER_DATA_BASE + (stdio_saved - data_start)], eax
    mov eax, SYS_CLOSE
    mov ebx, 1
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (stdio_payload - data_start)
    mov edx, 1
    int 0x80
    cmp eax, -9                ; EBADF: stdout is closed
    jne .fail
    mov eax, SYS_DUP2
    mov ebx, [USER_DATA_BASE + (stdio_saved - data_start)]
    mov ecx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    mov eax, SYS_CLOSE
    mov ebx, [USER_DATA_BASE + (stdio_saved - data_start)]
    int 0x80
    test eax, eax
    jne .fail

    mov esi, 72
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (stdio_message - data_start)
    mov edx, stdio_message_end - stdio_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; waitpid validates options and user status pointers before blocking.
    mov esi, 59
    mov eax, SYS_WAITPID
    mov ebx, -1
    mov ecx, USER_DATA_BASE + (child_status - data_start)
    xor edx, edx
    int 0x80
    cmp eax, -10              ; ECHILD
    jne .fail
    mov eax, SYS_WAITPID
    mov ebx, -1
    mov ecx, 0x1000
    xor edx, edx
    int 0x80
    cmp eax, -14              ; EFAULT
    jne .fail
    mov eax, SYS_WAITPID
    mov ebx, -1
    mov ecx, USER_DATA_BASE + (child_status - data_start)
    mov edx, 1
    int 0x80
    cmp eax, -22              ; EINVAL
    jne .fail

    ; Open a file and leave it at fd 3 so the spawned child inherits it.
    mov esi, 73
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (inherit_path - data_start)
    mov ecx, 0x242             ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jne .fail
    mov edi, eax
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (inherit_payload - data_start)
    mov edx, inherit_payload_end - inherit_payload
    int 0x80
    cmp eax, inherit_payload_end - inherit_payload
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail

    mov eax, SYS_SPAWN
    mov ebx, USER_DATA_BASE + (child_path - data_start)
    int 0x80
    cmp eax, 2
    jne .fail
    mov [USER_DATA_BASE + (child_pid - data_start)], eax
    mov esi, 60
    mov eax, SYS_WAITPID
    mov ebx, [USER_DATA_BASE + (child_pid - data_start)]
    mov ecx, USER_DATA_BASE + (child_status - data_start)
    xor edx, edx
    int 0x80
    cmp eax, [USER_DATA_BASE + (child_pid - data_start)]
    jne .fail
    cmp dword [USER_DATA_BASE + (child_status - data_start)], 0x2A00
    jne .fail
    mov eax, SYS_WAITPID
    mov ebx, [USER_DATA_BASE + (child_pid - data_start)]
    mov ecx, USER_DATA_BASE + (child_status - data_start)
    xor edx, edx
    int 0x80
    cmp eax, -10              ; a successful wait reaps the child
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (waitpid_message - data_start)
    mov edx, waitpid_message_end - waitpid_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; The child inherited fd 3, read it, and closed it. The parent's copy is
    ; still usable because both descriptors shared one open file description,
    ; and the child's descriptors were all released when it exited.
    mov esi, 74
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (inherit_buf - data_start)
    mov edx, inherit_payload_end - inherit_payload
    int 0x80
    cmp eax, inherit_payload_end - inherit_payload
    jne .fail
    push esi
    push edi
    mov esi, USER_DATA_BASE + (inherit_payload - data_start)
    mov edi, USER_DATA_BASE + (inherit_buf - data_start)
    mov ecx, inherit_payload_end - inherit_payload
    cld
    repe cmpsb
    pop edi
    pop esi
    jne .fail
    mov esi, 75
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_UNLINK
    mov ebx, USER_DATA_BASE + (inherit_path - data_start)
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (inherit_message - data_start)
    mov edx, inherit_message_end - inherit_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; fork(2): the child gets a copy of the address space and the parent's
    ; descriptors. It writes a file and exits with 42; the parent waits for
    ; it with wait4(2) and checks the reported status.
    mov esi, 162
    mov eax, SYS_FORK
    int 0x80
    test eax, eax
    jz .fork_child
    jl .fork_failed
    mov [USER_DATA_BASE + (fork_pid - data_start)], eax
    mov eax, SYS_WAIT4
    mov ebx, [USER_DATA_BASE + (fork_pid - data_start)]
    mov ecx, USER_DATA_BASE + (fork_status - data_start)
    xor edx, edx                ; options
    xor esi, esi                ; rusage
    int 0x80
    cmp eax, [USER_DATA_BASE + (fork_pid - data_start)]
    jne .fork_failed
    cmp dword [USER_DATA_BASE + (fork_status - data_start)], 0x2A00
    jne .fork_failed
    ; wait4 on a reaped child reports ECHILD
    mov eax, SYS_WAIT4
    mov ebx, [USER_DATA_BASE + (fork_pid - data_start)]
    mov ecx, USER_DATA_BASE + (fork_status - data_start)
    xor edx, edx
    xor esi, esi
    int 0x80
    cmp eax, -10
    jne .fork_failed
    jmp .fork_done
.fork_child:
    ; the child writes its marker file and leaves with status 42
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (fork_path - data_start)
    mov ecx, 0x242              ; O_CREAT | O_RDWR | O_TRUNC
    xor edx, edx
    int 0x80
    cmp eax, 3
    jb .fork_child_failed
    mov edi, eax
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, USER_DATA_BASE + (fork_payload - data_start)
    mov edx, fork_payload_end - fork_payload
    int 0x80
    cmp eax, fork_payload_end - fork_payload
    jne .fork_child_failed
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fork_child_failed
    mov eax, SYS_EXIT_GROUP
    mov ebx, 42
    int 0x80
.fork_child_failed:
    mov eax, SYS_EXIT_GROUP
    mov ebx, 1
    int 0x80
.fork_failed:
    mov esi, 162
    jmp .fail
.fork_done:
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (fork_message - data_start)
    mov edx, fork_message_end - fork_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Copy-on-write: fork shares the pages, so both sides see the prototype
    ; value until they write. The child's write is private and the parent's
    ; own write faults once and gets a private copy too.
    mov dword [USER_DATA_BASE + (cow_word - data_start)], 0x11111111
    mov esi, 1630
    mov eax, SYS_FORK
    int 0x80
    test eax, eax
    jz .cow_child
    jl .cow_failed
    mov [USER_DATA_BASE + (cow_pid - data_start)], eax
    mov eax, SYS_WAIT4
    mov ebx, [USER_DATA_BASE + (cow_pid - data_start)]
    mov ecx, USER_DATA_BASE + (cow_status - data_start)
    xor edx, edx
    xor esi, esi
    int 0x80
    mov esi, 1631
    cmp eax, [USER_DATA_BASE + (cow_pid - data_start)]
    jne .cow_failed
    mov esi, 1632
    cmp dword [USER_DATA_BASE + (cow_status - data_start)], 0
    jne .cow_failed
    mov esi, 1633
    cmp dword [USER_DATA_BASE + (cow_word - data_start)], 0x11111111
    jne .cow_failed
    ; writing after the fork breaks the share for the parent as well
    mov dword [USER_DATA_BASE + (cow_word - data_start)], 0x33333333
    mov esi, 1634
    cmp dword [USER_DATA_BASE + (cow_word - data_start)], 0x33333333
    jne .cow_failed
    jmp .cow_done
.cow_child:
    mov dword [USER_DATA_BASE + (cow_word - data_start)], 0x22222222
    mov ebx, 1
    cmp dword [USER_DATA_BASE + (cow_word - data_start)], 0x22222222
    jne .cow_child_exit
    xor ebx, ebx
.cow_child_exit:
    mov eax, SYS_EXIT
    int 0x80
.cow_failed:
    jmp .fail
.cow_done:
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (cow_message - data_start)
    mov edx, cow_message_end - cow_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; vfork(2): the child borrows the parent's address space and the parent
    ; stays parked until the child exits, then resumes with the child pid.
    mov dword [USER_DATA_BASE + (vfork_word - data_start)], 0
    mov esi, 164
    mov eax, SYS_VFORK
    int 0x80
    test eax, eax
    jz .vfork_child
    jl .vfork_failed
    ; what the child wrote is visible: the space was shared, not copied
    cmp eax, [USER_DATA_BASE + (vfork_pid - data_start)]
    jne .vfork_failed
    cmp dword [USER_DATA_BASE + (vfork_word - data_start)], 0x5A5A5A5A
    jne .vfork_failed
    ; the child is a normal waitable zombie
    mov eax, SYS_WAIT4
    mov ebx, [USER_DATA_BASE + (vfork_pid - data_start)]
    mov ecx, USER_DATA_BASE + (vfork_status - data_start)
    xor edx, edx
    xor esi, esi
    int 0x80
    cmp eax, [USER_DATA_BASE + (vfork_pid - data_start)]
    jne .vfork_failed
    cmp dword [USER_DATA_BASE + (vfork_status - data_start)], 0x900
    jne .vfork_failed
    jmp .vfork_done
.vfork_child:
    mov dword [USER_DATA_BASE + (vfork_word - data_start)], 0x5A5A5A5A
    mov eax, SYS_GETPID
    int 0x80
    mov [USER_DATA_BASE + (vfork_pid - data_start)], eax
    mov eax, SYS_EXIT
    mov ebx, 9
    int 0x80
.vfork_failed:
    mov esi, 164
    jmp .fail
.vfork_done:
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (vfork_message - data_start)
    mov edx, vfork_message_end - vfork_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; vfork(2) + execve(2): the parent must wake up when the child replaces
    ; its borrowed image, and the child then runs in its own address space.
    mov esi, 165
    mov eax, SYS_VFORK
    int 0x80
    test eax, eax
    jz .vfork_exec_child
    jl .vfork_exec_failed
    mov [USER_DATA_BASE + (vfork_exec_pid - data_start)], eax
    mov eax, SYS_WAIT4
    mov ebx, [USER_DATA_BASE + (vfork_exec_pid - data_start)]
    mov ecx, USER_DATA_BASE + (vfork_exec_status - data_start)
    xor edx, edx
    xor esi, esi
    int 0x80
    cmp eax, [USER_DATA_BASE + (vfork_exec_pid - data_start)]
    jne .vfork_exec_failed
    cmp dword [USER_DATA_BASE + (vfork_exec_status - data_start)], 0x700
    jne .vfork_exec_failed
    jmp .vfork_exec_done
.vfork_exec_child:
    mov eax, SYS_EXECVE
    mov ebx, USER_DATA_BASE + (vfork_exec_path - data_start)
    xor ecx, ecx
    xor edx, edx
    int 0x80
    mov eax, SYS_EXIT
    mov ebx, 111
    int 0x80
.vfork_exec_failed:
    mov esi, 165
    jmp .fail
.vfork_exec_done:
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (vfork_exec_message - data_start)
    mov edx, vfork_exec_message_end - vfork_exec_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Time: gettimeofday reads the tick counter and nanosleep blocks for
    ; at least the requested interval (the task is parked until its wake
    ; tick passes, not busy-waited).
    mov esi, 76
    mov eax, SYS_GETTIMEOFDAY
    mov ebx, USER_DATA_BASE + (timeval1 - data_start)
    xor ecx, ecx
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 77
    mov eax, SYS_GETTIMEOFDAY
    mov ebx, 0x1000           ; EFAULT for an unmapped timeval
    xor ecx, ecx
    int 0x80
    cmp eax, -14
    jne .fail
    mov eax, SYS_GETTIMEOFDAY ; a NULL timeval is allowed (no-op query)
    xor ebx, ebx
    xor ecx, ecx
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 78
    mov eax, SYS_NANOSLEEP
    xor ebx, ebx
    xor ecx, ecx
    int 0x80
    cmp eax, -14              ; EFAULT for a NULL request
    jne .fail
    mov esi, 79
    mov eax, SYS_NANOSLEEP
    mov ebx, USER_DATA_BASE + (timespec_bad - data_start)
    xor ecx, ecx
    int 0x80
    cmp eax, -22              ; EINVAL: tv_nsec is out of range
    jne .fail
    mov esi, 80
    mov eax, SYS_NANOSLEEP
    mov ebx, USER_DATA_BASE + (timespec_zero - data_start)
    xor ecx, ecx
    int 0x80
    test eax, eax             ; a zero interval succeeds immediately
    jne .fail
    mov esi, 81
    mov eax, SYS_NANOSLEEP
    mov ebx, USER_DATA_BASE + (timespec_50ms - data_start)
    xor ecx, ecx
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 82
    mov eax, SYS_GETTIMEOFDAY
    mov ebx, USER_DATA_BASE + (timeval2 - data_start)
    xor ecx, ecx
    int 0x80
    test eax, eax
    jne .fail
    ; The elapsed interval must cover the requested 50 ms: either the
    ; seconds field advanced, or the microseconds grew by >= 30000.
    mov eax, [USER_DATA_BASE + (timeval2 - data_start)]
    sub eax, [USER_DATA_BASE + (timeval1 - data_start)]
    jne .time_ok
    mov eax, [USER_DATA_BASE + (timeval2 - data_start) + 4]
    sub eax, [USER_DATA_BASE + (timeval1 - data_start) + 4]
    cmp eax, 30000
    jb .fail
.time_ok:
    mov esi, 83
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (time_message - data_start)
    mov edx, time_message_end - time_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; brk grows a zero-filled heap; mmap maps anonymous zeroed pages.
    mov esi, 84
    mov eax, SYS_BRK
    xor ebx, ebx                ; query the initial program break
    int 0x80
    cmp eax, 0x50000000
    jne .fail
    mov [USER_DATA_BASE + (brk_base - data_start)], eax
    mov esi, 85
    mov eax, SYS_BRK
    mov ebx, [USER_DATA_BASE + (brk_base - data_start)]
    add ebx, 0x2000
    int 0x80
    mov edx, [USER_DATA_BASE + (brk_base - data_start)]
    add edx, 0x2000
    cmp eax, edx
    jne .fail
    mov esi, 86
    mov edi, [USER_DATA_BASE + (brk_base - data_start)]
    cmp dword [edi], 0          ; freshly grown pages read as zero
    jne .fail
    cmp dword [edi + 0xFFC], 0
    jne .fail
    mov dword [edi], 0xDEADBEEF ; and are writable
    cmp dword [edi], 0xDEADBEEF
    jne .fail
    mov esi, 87                 ; a break below the heap base is invalid
    mov eax, SYS_BRK
    mov ebx, 0x40000000
    int 0x80
    cmp eax, -22
    jne .fail
    mov esi, 88                 ; a break past the heap limit is out of memory
    mov eax, SYS_BRK
    mov ebx, 0x54001000
    int 0x80
    cmp eax, -12
    jne .fail
    mov esi, 89
    mov eax, SYS_MMAP
    mov ebx, USER_DATA_BASE + (mmap_arg - data_start)
    int 0x80
    cmp eax, 0x60000000
    jne .fail
    mov [USER_DATA_BASE + (mmap_first - data_start)], eax
    mov esi, 90
    mov edi, eax
    cmp dword [edi], 0          ; anonymous page reads as zero
    jne .fail
    mov dword [edi], 0x12345678 ; and is writable
    cmp dword [edi], 0x12345678
    jne .fail
    mov esi, 91
    mov eax, SYS_MMAP
    mov ebx, USER_DATA_BASE + (mmap_arg - data_start)
    int 0x80
    mov edx, [USER_DATA_BASE + (mmap_first - data_start)]
    add edx, 0x1000
    cmp eax, edx                ; the next mapping follows the first
    jne .fail
    mov esi, 92
    mov eax, SYS_MMAP
    mov ebx, USER_DATA_BASE + (mmap_bad_len - data_start)
    int 0x80
    cmp eax, -22
    jne .fail
    mov esi, 93
    mov eax, SYS_MMAP
    mov ebx, USER_DATA_BASE + (mmap_bad_flags - data_start)
    int 0x80
    cmp eax, -22
    jne .fail
    mov esi, 94
    mov eax, SYS_MMAP
    xor ebx, ebx                ; NULL argument block
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 95                 ; a PROT_NONE reservation is allowed
    mov eax, SYS_MMAP
    mov ebx, USER_DATA_BASE + (mmap_prot_none - data_start)
    int 0x80
    test eax, eax
    js .fail                    ; the mapping must be created
    mov esi, 166
    mov edi, eax
    mov eax, SYS_MUNMAP
    mov ebx, edi
    mov ecx, 0x1000
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 115                ; shrink the break and release the top page
    mov eax, SYS_BRK
    mov ebx, [USER_DATA_BASE + (brk_base - data_start)]
    add ebx, 0x1000
    int 0x80
    mov edx, [USER_DATA_BASE + (brk_base - data_start)]
    add edx, 0x1000
    cmp eax, edx
    jne .fail
    mov esi, 116
    mov eax, SYS_BRK
    xor ebx, ebx                ; query confirms the lower break
    int 0x80
    mov edx, [USER_DATA_BASE + (brk_base - data_start)]
    add edx, 0x1000
    cmp eax, edx
    jne .fail
    mov esi, 117
    mov edi, [USER_DATA_BASE + (brk_base - data_start)]
    cmp dword [edi], 0xDEADBEEF ; the surviving page is untouched
    jne .fail
    mov esi, 118                ; regrow: the released page returns zeroed
    mov eax, SYS_BRK
    mov ebx, [USER_DATA_BASE + (brk_base - data_start)]
    add ebx, 0x2000
    int 0x80
    mov edx, [USER_DATA_BASE + (brk_base - data_start)]
    add edx, 0x2000
    cmp eax, edx
    jne .fail
    mov esi, 119
    mov edi, [USER_DATA_BASE + (brk_base - data_start)]
    add edi, 0x1000
    cmp dword [edi], 0
    jne .fail
    mov esi, 120                ; munmap releases the first anonymous page
    mov eax, SYS_MUNMAP
    mov ebx, [USER_DATA_BASE + (mmap_first - data_start)]
    mov ecx, 0x1000
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 121                ; the next mmap reuses the freed address
    mov eax, SYS_MMAP
    mov ebx, USER_DATA_BASE + (mmap_arg - data_start)
    int 0x80
    cmp eax, [USER_DATA_BASE + (mmap_first - data_start)]
    jne .fail
    mov esi, 122
    mov edi, eax
    cmp dword [edi], 0          ; and maps a fresh zero page
    jne .fail
    mov esi, 123                ; a zero length is invalid
    mov eax, SYS_MUNMAP
    mov ebx, [USER_DATA_BASE + (mmap_first - data_start)]
    xor ecx, ecx
    int 0x80
    cmp eax, -22
    jne .fail
    mov esi, 124                ; so is an address outside the mmap region
    mov eax, SYS_MUNMAP
    mov ebx, 0x50000000
    mov ecx, 0x1000
    int 0x80
    cmp eax, -22
    jne .fail
    mov esi, 125                ; map three pages, cut out the middle
    mov eax, SYS_MMAP
    mov ebx, USER_DATA_BASE + (mmap_three_pages - data_start)
    int 0x80
    mov edx, [USER_DATA_BASE + (mmap_first - data_start)]
    add edx, 0x2000
    cmp eax, edx
    jne .fail
    mov esi, 126
    lea ebx, [edx + 0x1000]
    mov ecx, 0x1000
    mov eax, SYS_MUNMAP
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 127                ; the hole is picked up by the next mmap
    mov eax, SYS_MMAP
    mov ebx, USER_DATA_BASE + (mmap_arg - data_start)
    int 0x80
    mov edx, [USER_DATA_BASE + (mmap_first - data_start)]
    add edx, 0x3000
    cmp eax, edx
    jne .fail
    mov esi, 128
    cmp dword [eax], 0
    jne .fail
    mov esi, 129
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (mem_message - data_start)
    mov edx, mem_message_end - mem_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Subdirectories and basic paths, including "." and ".." rules.
    mov esi, 96
    mov eax, SYS_MKDIR
    mov ebx, USER_DATA_BASE + (dir_path - data_start)      ; "DIR1"
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 97
    mov eax, SYS_MKDIR
    mov ebx, USER_DATA_BASE + (dir_path - data_start)
    int 0x80
    cmp eax, -17                ; a repeated mkdir is EEXIST
    jne .fail
    mov esi, 98
    mov eax, SYS_STAT
    mov ebx, USER_DATA_BASE + (dir_path - data_start)
    mov ecx, USER_DATA_BASE + (stat_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    movzx eax, word [USER_DATA_BASE + (stat_buf - data_start) + 8]
    and eax, 0xF000
    cmp eax, HAKU_S_IFDIR       ; stat reports the directory mode
    jne .fail
    mov esi, 99
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (dir_file_path - data_start) ; "DIR1/T.TXT"
    mov ecx, HAKU_O_RDWR | HAKU_O_CREAT
    int 0x80
    cmp eax, 3
    jne .fail
    mov [USER_DATA_BASE + (dir_fd - data_start)], eax
    mov esi, 100
    mov eax, SYS_WRITE
    mov ebx, [USER_DATA_BASE + (dir_fd - data_start)]
    mov ecx, USER_DATA_BASE + (dir_payload - data_start)
    mov edx, dir_payload_end - dir_payload
    int 0x80
    cmp eax, dir_payload_end - dir_payload
    jne .fail
    mov esi, 101
    mov eax, SYS_CLOSE
    mov ebx, [USER_DATA_BASE + (dir_fd - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 102
    mov eax, SYS_OPEN                ; "./DIR1/../DIR1/T.TXT"
    mov ebx, USER_DATA_BASE + (dir_dotdot_path - data_start)
    xor ecx, ecx
    int 0x80
    cmp eax, 3
    jne .fail
    mov [USER_DATA_BASE + (dir_fd - data_start)], eax
    mov esi, 103
    mov eax, SYS_READ
    mov ebx, [USER_DATA_BASE + (dir_fd - data_start)]
    mov ecx, USER_DATA_BASE + (dir_readback - data_start)
    mov edx, dir_payload_end - dir_payload
    int 0x80
    cmp eax, dir_payload_end - dir_payload
    jne .fail
    mov ecx, dir_payload_end - dir_payload
    xor edi, edi
.dir_cmp:
    mov al, [USER_DATA_BASE + (dir_readback - data_start) + edi]
    cmp al, [USER_DATA_BASE + (dir_payload - data_start) + edi]
    jne .fail
    inc edi
    cmp edi, ecx
    jb .dir_cmp
    mov esi, 104
    mov eax, SYS_CLOSE
    mov ebx, [USER_DATA_BASE + (dir_fd - data_start)]
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 105
    mov eax, SYS_OPEN                ; open on a directory is EISDIR
    mov ebx, USER_DATA_BASE + (dir_path - data_start)
    xor ecx, ecx
    int 0x80
    cmp eax, -21
    jne .fail
    mov esi, 106
    mov eax, SYS_OPEN                ; a file used as a directory is ENOTDIR
    mov ebx, USER_DATA_BASE + (dir_notdir_path - data_start) ; "HAKU.ELF/X"
    xor ecx, ecx
    int 0x80
    cmp eax, -20
    jne .fail
    mov esi, 107
    mov eax, SYS_MKDIR               ; a missing parent is ENOENT
    mov ebx, USER_DATA_BASE + (dir_missing - data_start) ; "NOPE/DIR"
    int 0x80
    cmp eax, -2
    jne .fail
    mov esi, 108
    mov eax, SYS_UNLINK              ; unlink on a directory is EISDIR
    mov ebx, USER_DATA_BASE + (dir_path - data_start)
    int 0x80
    cmp eax, -21
    jne .fail
    mov esi, 109
    mov eax, SYS_RMDIR               ; a non-empty directory is ENOTEMPTY
    mov ebx, USER_DATA_BASE + (dir_path - data_start)
    int 0x80
    cmp eax, -39
    jne .fail
    mov esi, 110
    mov eax, SYS_UNLINK
    mov ebx, USER_DATA_BASE + (dir_file_path - data_start)
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 111
    mov eax, SYS_RMDIR
    mov ebx, USER_DATA_BASE + (dir_path - data_start)
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 112
    mov eax, SYS_MKDIR               ; a plain empty directory round-trips
    mov ebx, USER_DATA_BASE + (dir_empty_path - data_start) ; "EMPTY"
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 113
    mov eax, SYS_RMDIR
    mov ebx, USER_DATA_BASE + (dir_empty_path - data_start)
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 114
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (dir_message - data_start)
    mov edx, dir_message_end - dir_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Per-process memory and resource accounting.
    mov esi, 130
    mov eax, SYS_GETUSAGE
    mov ebx, USER_DATA_BASE + (usage_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    mov edi, USER_DATA_BASE + (usage_buf - data_start)
    mov esi, 131
    mov eax, SYS_GETPID
    int 0x80
    cmp eax, [edi]                  ; the snapshot is for this process
    jne .fail
    mov esi, 132
    cmp dword [edi + 36], 2         ; two kernel-stack pages per task
    jne .fail
    mov esi, 133
    cmp dword [edi + 24], 0         ; resident user pages were counted
    je .fail
    mov esi, 134
    mov eax, [edi + 28]
    cmp eax, [edi + 24]             ; peak >= current
    jb .fail
    mov esi, 135
    mov eax, [edi + 24]
    shl eax, 12
    cmp eax, [edi + 48]             ; resident = pages * 4 KiB
    jne .fail
    mov esi, 136
    mov eax, [edi + 32]
    add eax, 3                      ; tables + directory + 2 stacks
    shl eax, 12
    cmp eax, [edi + 52]
    jne .fail
    mov esi, 137
    mov eax, [edi + 48]
    add eax, [edi + 52]
    cmp eax, [edi + 56]
    jne .fail
    mov esi, 138
    mov eax, [edi + 28]
    shl eax, 12
    cmp eax, [edi + 60]
    jne .fail
    mov esi, 139
    cmp dword [edi + 44], 0x5000    ; five live anonymous pages
    jne .fail
    mov esi, 140
    cmp dword [edi + 40], 0x2000    ; two heap pages from the earlier tests
    jne .fail
    mov esi, 141
    cmp dword [edi + 16], 0         ; read/nanosleep switched voluntarily
    je .fail
    mov esi, 142
    mov eax, [edi + 4]
    mov [USER_DATA_BASE + (usage_ticks - data_start)], eax
    mov eax, [edi + 12]
    mov [USER_DATA_BASE + (usage_syscalls - data_start)], eax
    mov eax, [edi + 24]
    mov [USER_DATA_BASE + (usage_pages - data_start)], eax
    mov eax, [edi + 28]
    mov [USER_DATA_BASE + (usage_peak - data_start)], eax
    mov eax, SYS_BRK
    mov ebx, [USER_DATA_BASE + (brk_base - data_start)]
    add ebx, 0x4000
    int 0x80
    mov edx, [USER_DATA_BASE + (brk_base - data_start)]
    add edx, 0x4000
    cmp eax, edx
    jne .fail
    mov esi, 143
    mov eax, SYS_GETUSAGE
    mov ebx, USER_DATA_BASE + (usage_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    mov edi, USER_DATA_BASE + (usage_buf - data_start)
    mov esi, 144
    mov eax, [edi + 24]
    sub eax, [USER_DATA_BASE + (usage_pages - data_start)]
    cmp eax, 2                      ; two new heap pages
    jne .fail
    mov esi, 145
    cmp dword [edi + 40], 0x4000    ; heap grew by two pages
    jne .fail
    mov esi, 146
    mov eax, [edi + 12]
    cmp eax, [USER_DATA_BASE + (usage_syscalls - data_start)]
    jbe .fail                       ; syscall counter advanced
    mov esi, 147
    mov eax, [edi + 28]
    mov edx, [USER_DATA_BASE + (usage_peak - data_start)]
    add edx, 2
    cmp eax, edx                    ; peak kept the new high-water mark
    jb .fail
    mov esi, 148
    mov ecx, 20000000
.usage_spin:
    dec ecx
    jnz .usage_spin
    mov eax, SYS_GETUSAGE
    mov ebx, USER_DATA_BASE + (usage_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    mov edi, USER_DATA_BASE + (usage_buf - data_start)
    mov esi, 149
    mov eax, [edi + 4]
    cmp eax, [USER_DATA_BASE + (usage_ticks - data_start)]
    jbe .fail                       ; ring-3 busy loop accrued user ticks
    mov esi, 150
    mov eax, SYS_BRK
    mov ebx, [USER_DATA_BASE + (brk_base - data_start)]
    add ebx, 0x2000
    int 0x80
    mov edx, [USER_DATA_BASE + (brk_base - data_start)]
    add edx, 0x2000
    cmp eax, edx
    jne .fail
    mov esi, 151
    mov eax, SYS_MUNMAP
    mov ebx, [USER_DATA_BASE + (mmap_first - data_start)]
    add ebx, 0x4000
    mov ecx, 0x1000
    int 0x80
    test eax, eax
    jne .fail
    mov esi, 152
    mov eax, SYS_GETUSAGE
    mov ebx, USER_DATA_BASE + (usage_buf - data_start)
    int 0x80
    test eax, eax
    jne .fail
    mov edi, USER_DATA_BASE + (usage_buf - data_start)
    mov esi, 153
    cmp dword [edi + 44], 0x4000    ; one mmap page was released
    jne .fail
    mov esi, 154
    cmp dword [edi + 40], 0x2000    ; break is back at two pages
    jne .fail
    mov esi, 155
    mov eax, SYS_GETUSAGE
    xor ebx, ebx                    ; NULL usage buffer
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 156
    mov eax, SYS_GETUSAGE
    mov ebx, 0x1000                 ; unmapped usage buffer
    int 0x80
    cmp eax, -14
    jne .fail
    mov esi, 157
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (usage_message - data_start)
    mov edx, usage_message_end - usage_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Hardware page protections: each fault child makes one illegal access
    ; and the kernel must terminate it. abort_user encodes the #PF error
    ; code into the exit status byte (0x80 | err), so waitpid can tell a
    ; read-only write (0x87), an unmapped access (0x84), and a write to a
    ; released mapping (0x86) apart. The mode byte travels through the
    ; inherited fd 3, the same descriptor path the child test exercises.
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (prot_path - data_start)
    mov ecx, 0x242
    xor edx, edx
    int 0x80
    cmp eax, 3
    jne .fail
    mov edi, eax
    mov esi, USER_DATA_BASE + (fault_modes - data_start)
    mov ebp, USER_DATA_BASE + (fault_statuses - data_start)
.prot_loop:
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, edi
    mov ecx, esi
    mov edx, 1
    int 0x80
    cmp eax, 1
    jne .fail
    mov eax, SYS_LSEEK
    mov ebx, edi
    xor ecx, ecx
    xor edx, edx
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_SPAWN
    mov ebx, USER_DATA_BASE + (fault_path - data_start)
    int 0x80
    test eax, eax
    jle .fail
    mov ebx, eax
    mov eax, SYS_WAITPID
    mov ecx, USER_DATA_BASE + (fault_status - data_start)
    xor edx, edx
    int 0x80
    test eax, eax
    jle .fail
    mov eax, [ebp]
    cmp dword [USER_DATA_BASE + (fault_status - data_start)], eax
    jne .fail
    inc esi
    add ebp, 4
    cmp esi, USER_DATA_BASE + (fault_modes_end - data_start)
    jb .prot_loop
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_UNLINK
    mov ebx, USER_DATA_BASE + (prot_path - data_start)
    int 0x80
    test eax, eax
    jne .fail
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, USER_DATA_BASE + (prot_message - data_start)
    mov edx, prot_message_end - prot_message
    int 0x80
    cmp eax, edx
    jne .fail

    ; Failed exec attempts must leave this process image intact.
    mov esi, 61
    mov eax, SYS_EXECVE
    mov ebx, 0x1000             ; unmapped pathname
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -14
    jne .fail
    mov eax, SYS_EXECVE
    mov ebx, USER_DATA_BASE + (missing_exec_path - data_start)
    xor ecx, ecx
    xor edx, edx
    int 0x80
    cmp eax, -2
    jne .fail
    mov eax, SYS_EXECVE
    mov ebx, USER_DATA_BASE + (exec_path - data_start)
    mov ecx, 0x1000
    xor edx, edx
    int 0x80
    cmp eax, -14
    jne .fail
    mov eax, SYS_EXECVE
    mov ebx, USER_DATA_BASE + (exec_path - data_start)
    mov ecx, USER_DATA_BASE + (exec_bad_argv - data_start)
    xor edx, edx
    int 0x80
    cmp eax, -14
    jne .fail
    mov eax, SYS_EXECVE
    mov ebx, USER_DATA_BASE + (exec_path - data_start)
    mov ecx, USER_DATA_BASE + (exec_argv - data_start)
    mov edx, 0x1000
    int 0x80
    cmp eax, -14
    jne .fail
    mov eax, SYS_EXECVE
    mov ebx, USER_DATA_BASE + (exec_path - data_start)
    mov ecx, USER_DATA_BASE + (exec_many_argv - data_start)
    xor edx, edx
    int 0x80
    cmp eax, -7
    jne .fail
    mov eax, SYS_EXECVE
    mov ebx, USER_DATA_BASE + (exec_path - data_start)
    mov ecx, USER_DATA_BASE + (exec_large_argv - data_start)
    xor edx, edx
    int 0x80
    cmp eax, -7
    jne .fail

    ; Keep one open file across exec. The replacement ELF validates fd 3.
    mov eax, SYS_OPEN
    mov ebx, USER_DATA_BASE + (exec_preserved_path - data_start)
    xor ecx, ecx                ; O_RDONLY
    xor edx, edx
    int 0x80
    cmp eax, 3
    jne .fail
    mov eax, SYS_EXECVE
    mov ebx, USER_DATA_BASE + (exec_path - data_start)
    mov ecx, USER_DATA_BASE + (exec_argv - data_start)
    mov edx, USER_DATA_BASE + (exec_envp - data_start)
    int 0x80
    ; A successful execve never resumes this instruction.
    mov esi, 62
    jmp .fail
.hang:
    jmp .hang

.fail:
    mov eax, SYS_WRITE
    mov ebx, 2
    mov ecx, USER_DATA_BASE + (failure_message - data_start)
    mov edx, failure_message_end - failure_message
    int 0x80
    mov eax, SYS_EXIT
    mov ebx, esi
    int 0x80
    jmp .hang
code_end:

    times 0x4000 - ($ - $$) db 0
data_start:
startup_message:
    db "USERMODE-OK ELF32 RX/RW + POSIX-style file syscalls", 10
startup_message_end:
read_message:
    db "USER-READ-OK ELF data and stack copy_to_user", 10
read_message_end:
file_io_message:
    db "USER-FILE-IO-OK create/write/read/lseek/close", 10
file_io_message_end:
checks_message:
    db "USER-SYSCALL-CHECKS-OK", 10
checks_message_end:
failure_message:
    db "USER-SYSCALL-FAIL", 10
failure_message_end:
file_path_one:
    db "P1.TXT", 0
file_path_two:
    db "P2.TXT", 0
file_path_selected:
    dd 0
file_fd:
    dd 0
file_ops_message:
    db "USER-FILE-OPS-OK unlink/rename/stat/fstat", 10
file_ops_message_end:
unlink_path:
    db "U0.TXT", 0
rename_src:
    db "R0A.TXT", 0
rename_dst:
    db "R0B.TXT", 0
rename_dst2:
    db "R0C.TXT", 0
extra_path:
    db "X0.TXT", 0
missing_q9:
    db "Q9.TXT", 0
missing_q8:
    db "Q8.TXT", 0
ops_unlink:
    dd 0
ops_src:
    dd 0
ops_dst:
    dd 0
ops_dst2:
    dd 0
ops_extra:
    dd 0
ops_fd:
    dd 0
dup_message:
    db "USER-DUP-OK dup/dup2 shared file offset", 10
dup_message_end:
waitpid_message:
    db "USER-WAITPID-OK child status=42 reaped", 10
waitpid_message_end:
child_path:
    db "CHILD.ELF", 0
child_pid:
    dd 0
child_status:
    dd 0
dup_path:
    db "D0.TXT", 0
dup_fd_b:
    dd 0
dup_buf:
    times 4 db 0
stdio_path:
    db "S1.TXT", 0
stdio_fd:
    dd 0
stdio_saved:
    dd 0
stdio_payload:
    db "STDIO-REDIRECT-PAYLOAD"
stdio_payload_end:
stdio_buf:
    times stdio_payload_end - stdio_payload db 0
stdio_message:
    db "USER-STDIO-OK stdio 0/1/2 redirect, close, restore", 10
stdio_message_end:
inherit_path:
    db "I1.TXT", 0
fork_path:
    db "F1.TXT", 0
fork_payload:
    db "FORK"
fork_payload_end:
fork_pid:
    dd 0
fork_status:
    dd 0
fork_message:
    db "USER-FORK-OK fork/wait4/exit-group", 10
fork_message_end:
cow_word:
    dd 0
cow_pid:
    dd 0
cow_status:
    dd 0
cow_message:
    db "USER-COW-OK fork shares pages copy-on-write", 10
cow_message_end:
vfork_word:
    dd 0
vfork_pid:
    dd 0
vfork_status:
    dd 0
vfork_message:
    db "USER-VFORK-OK vfork shares the address space", 10
vfork_message_end:
vfork_exec_path:
    db "VFORK.ELF", 0
vfork_exec_pid:
    dd 0
vfork_exec_status:
    dd 0
vfork_exec_message:
    db "USER-VFORK-OK vfork parent resumed on child execve", 10
vfork_exec_message_end:
inherit_payload:
    db "INHERITED-FD"
inherit_payload_end:
inherit_buf:
    times inherit_payload_end - inherit_payload db 0
inherit_message:
    db "USER-FD-INHERIT-OK child shared inherited fd 3", 10
inherit_message_end:
timeval1:
    times 8 db 0
timeval2:
    times 8 db 0
timespec_zero:
    dd 0, 0
timespec_50ms:
    dd 0, 50000000
timespec_bad:
    dd 0, 1000000000
time_message:
    db "USER-TIME-OK gettimeofday and nanosleep advance", 10
time_message_end:
brk_base:
    dd 0
mmap_first:
    dd 0
mmap_arg:
    dd 0, 0x1000, HAKU_PROT_READ | HAKU_PROT_WRITE, HAKU_MAP_PRIVATE | HAKU_MAP_ANONYMOUS, -1, 0
mmap_bad_len:
    dd 0, 0, HAKU_PROT_READ | HAKU_PROT_WRITE, HAKU_MAP_PRIVATE | HAKU_MAP_ANONYMOUS, -1, 0
mmap_bad_flags:
    dd 0, 0x1000, HAKU_PROT_READ | HAKU_PROT_WRITE, HAKU_MAP_PRIVATE, -1, 0
mmap_prot_none:
    dd 0, 0x1000, HAKU_PROT_NONE, HAKU_MAP_PRIVATE | HAKU_MAP_ANONYMOUS, -1, 0
mmap_three_pages:
    dd 0, 0x3000, HAKU_PROT_READ | HAKU_PROT_WRITE, HAKU_MAP_PRIVATE | HAKU_MAP_ANONYMOUS, -1, 0
mem_message:
    db "USER-MEM-OK brk grows/shrinks and mmap allocates/releases", 10
mem_message_end:
dir_path:
    db "DIR1", 0
dir_file_path:
    db "DIR1/T.TXT", 0
dir_dotdot_path:
    db "./DIR1/../DIR1/T.TXT", 0
dir_notdir_path:
    db "HAKU.ELF/X", 0
dir_missing:
    db "NOPE/DIR", 0
dir_empty_path:
    db "EMPTY", 0
dir_fd:
    dd 0
dir_payload:
    db "DIR-HELLO"
dir_payload_end:
dir_readback:
    times dir_payload_end - dir_payload db 0
dir_message:
    db "USER-DIR-OK subdirectories, paths, and . / .. resolution", 10
dir_message_end:
usage_buf:
    times HAKU_USAGE_SIZE db 0
usage_ticks:
    dd 0
usage_syscalls:
    dd 0
usage_pages:
    dd 0
usage_peak:
    dd 0
usage_message:
    db "USER-USAGE-OK per-process memory and resource accounting", 10
usage_message_end:
stat_buf:
    times 64 db 0
stat64_buf:
    times 96 db 0
statx_buf:
    times 256 db 0
fault_path:
    db "FAULT.ELF", 0
prot_path:
    db "PROT.TMP", 0
fault_modes:
    db 'C', 'R', 'U', 'F'
fault_modes_end:
fault_statuses:
    dd 0x8700, 0x8700, 0x8400, 0x8600
fault_status:
    dd 0
prot_message:
    db "USER-PROT-OK read-only, unmapped, and freed-page faults", 10
prot_message_end:
exec_path:
    db "NEXT.ELF", 0
exec_preserved_path:
    db "HAKU.ELF", 0
missing_exec_path:
    db "NOFILE.ELF", 0
exec_bad_argv:
    dd 0x1000, 0
exec_many_argv:
%rep 65
    dd USER_DATA_BASE + (exec_empty_arg - data_start)
%endrep
    dd 0
exec_large_argv:
%rep 64
    dd USER_DATA_BASE + (exec_long_arg - data_start)
%endrep
    dd 0
exec_argv:
    dd USER_DATA_BASE + (exec_arg0 - data_start)
    dd USER_DATA_BASE + (exec_arg1 - data_start)
    dd USER_DATA_BASE + (exec_arg2 - data_start)
    dd 0
exec_envp:
    dd USER_DATA_BASE + (exec_env0 - data_start)
    dd USER_DATA_BASE + (exec_env1 - data_start)
    dd 0
exec_arg0:
    db "NEXT.ELF", 0
exec_arg1:
    db "arg one", 0
exec_arg2:
    db "arg two", 0
exec_env0:
    db "MODE=exec-test", 0
exec_env1:
    db "EMPTY=", 0
exec_empty_arg:
    db 0
exec_long_arg:
    times 64 db 'X'
    db 0
append_payload:
    db 'B'
file_payload:
    times 700 db 'A'
file_payload_end:
file_readback:
    times file_payload_end - file_payload db 0
data_file_end:
    times 32 db 0
bss_word:
    dd 0
data_end:
