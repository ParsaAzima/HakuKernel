; Haku romfs module: a read-only filesystem implemented as a loadable
; ring-0 module.
;
; The module registers a vfs_fs_ops table with the kernel and mounts it at
; /rom. The table and every string it points at live in this image, so the
; kernel refuses to unload the module while a descriptor opened through /rom
; is still live; otherwise the VFS would be calling freed code.
;
; The two helpers haku_module_fill_stat/haku_module_fill_dirent fill the
; kernel's VFS structs, which keeps this file free of struct layouts. The
; ops table below is a plain array of 21 pointers, in the order of
; vfs_fs_ops_t in kernel/fs/vfs/vfs.h.

bits 32

%define VFS_MOUNT_RDONLY 1
%define S_IFDIR 0x41ED          ; S_IFDIR | 0755
%define S_IFREG 0x81A4          ; S_IFREG | 0644
%define DT_DIR 4
%define DT_REG 8
%define ERR_ENOENT -2
%define ERR_ENOTDIR -20
%define ERR_EISDIR -21
%define ERR_EINVAL -22
%define ERR_ENOMEM -12
%define ERR_EROFS -30

%define HANDLE_DATA 0
%define HANDLE_SIZE 4
%define HANDLE_POS 8
%define HANDLE_ISDIR 12
%define HANDLE_BYTES 16

section .text
global haku_module_init
global haku_module_exit
extern haku_module_alloc
extern haku_module_free
extern haku_module_register_fs
extern haku_module_mount
extern haku_module_fill_stat
extern haku_module_fill_dirent
extern haku_module_log

; eax = 1 when the NUL-terminated strings at esi and edi are equal.
str_equal:
.loop:
    mov al, [esi]
    cmp al, [edi]
    jne .no
    test al, al
    jz .yes
    inc esi
    inc edi
    jmp .loop
.yes:
    mov eax, 1
    ret
.no:
    xor eax, eax
    ret

; int32_t romfs_mount(void)
romfs_mount:
    mov eax, 1
    ret

; int32_t romfs_open(const char *path, uint32_t flags, void **handle_out)
romfs_open:
    push ebx
    push esi
    push edi
    mov esi, [esp + 16]        ; path
    cmp byte [esi], '.'
    jne .readme
    cmp byte [esi + 1], 0
    jne .readme
    mov eax, ERR_EISDIR
    jmp .done
.readme:
    mov esi, [esp + 16]
    mov edi, name_readme
    call str_equal
    test eax, eax
    jz .version
    mov ebx, data_readme
    mov edi, data_readme_length
    jmp .make
.version:
    mov esi, [esp + 16]
    mov edi, name_version
    call str_equal
    test eax, eax
    jz .not_found
    mov ebx, data_version
    mov edi, data_version_length
.make:
    push HANDLE_BYTES
    call haku_module_alloc
    add esp, 4
    test eax, eax
    jz .no_memory
    mov [eax + HANDLE_DATA], ebx
    mov [eax + HANDLE_SIZE], edi
    mov dword [eax + HANDLE_POS], 0
    mov dword [eax + HANDLE_ISDIR], 0
    mov ecx, [esp + 24]        ; handle_out
    mov [ecx], eax
    mov eax, 1
    jmp .done
.not_found:
    mov eax, ERR_ENOENT
    jmp .done
.no_memory:
    mov eax, ERR_ENOMEM
.done:
    pop edi
    pop esi
    pop ebx
    ret

; int32_t romfs_close(void *handle)
romfs_close:
    push dword [esp + 4]
    call haku_module_free
    add esp, 4
    mov eax, 1
    ret

; int32_t romfs_read(void *handle, void *buffer, uint32_t count,
;                    uint32_t *out_count)
romfs_read:
    push esi
    push edi
    push ebx
    mov ebx, [esp + 16]        ; handle
    cmp dword [ebx + HANDLE_ISDIR], 0
    jne .is_dir
    mov edx, [ebx + HANDLE_SIZE]
    mov ecx, [ebx + HANDLE_POS]
    cmp ecx, edx
    jae .eof
    sub edx, ecx               ; bytes left
    mov eax, [esp + 24]        ; count
    cmp eax, edx
    jbe .have
    mov eax, edx
.have:
    mov edi, [esp + 20]        ; buffer
    mov esi, [ebx + HANDLE_DATA]
    add esi, ecx
    mov edx, eax               ; bytes delivered
    add [ebx + HANDLE_POS], eax
    mov ecx, eax
    cld
    rep movsb
    mov ecx, [esp + 28]        ; out_count
    mov [ecx], edx
    mov eax, 1
    jmp .done
.eof:
    mov ecx, [esp + 28]
    mov dword [ecx], 0
    mov eax, 1
    jmp .done
.is_dir:
    mov eax, ERR_EISDIR
.done:
    pop ebx
    pop edi
    pop esi
    ret

; int32_t romfs_write(const void *buffer, uint32_t count, uint32_t *out_count)
; The backend never reaches this on a read-only mount, but the table must
; carry a non-NULL entry.
romfs_write:
    mov eax, ERR_EROFS
    ret

; int32_t romfs_seek(void *handle, int32_t offset, uint32_t whence,
;                    uint32_t *out_pos)
romfs_seek:
    mov ecx, [esp + 4]         ; handle
    cmp dword [ecx + HANDLE_ISDIR], 0
    jne .bad
    mov edx, [esp + 8]         ; offset
    mov eax, [esp + 12]        ; whence
    cmp eax, 0
    je .set
    cmp eax, 1
    je .cur
    cmp eax, 2
    je .end
.bad:
    mov eax, ERR_EINVAL
    ret
.cur:
    add edx, [ecx + HANDLE_POS]
    jmp .set
.end:
    add edx, [ecx + HANDLE_SIZE]
.set:
    test edx, edx
    js .bad
    mov [ecx + HANDLE_POS], edx
    mov ecx, [esp + 16]        ; out_pos
    mov [ecx], edx
    mov eax, 1
    ret

; Read-only namespace operations. The VFS blocks most of these at the mount
; flag, and the backend says the same thing if it is ever reached directly.
romfs_readonly_path:
    mov eax, ERR_EROFS
    ret

romfs_truncate:
    mov eax, ERR_EROFS
    ret

; int32_t romfs_stat(const char *path, vfs_stat_t *out)
romfs_stat:
    push ebx
    push esi
    push edi
    mov ebx, [esp + 20]        ; out
    mov esi, [esp + 16]        ; path
    cmp byte [esi], '.'
    jne .readme
    cmp byte [esi + 1], 0
    jne .readme
    push 1                     ; ino
    push 0                     ; size
    push S_IFDIR
    push ebx
    call haku_module_fill_stat
    add esp, 16
    mov eax, 1
    jmp .done
.readme:
    mov esi, [esp + 16]
    mov edi, name_readme
    call str_equal
    test eax, eax
    jz .version
    push 2
    push data_readme_length
    push S_IFREG
    push ebx
    call haku_module_fill_stat
    add esp, 16
    mov eax, 1
    jmp .done
.version:
    mov esi, [esp + 16]
    mov edi, name_version
    call str_equal
    test eax, eax
    jz .not_found
    push 3
    push data_version_length
    push S_IFREG
    push ebx
    call haku_module_fill_stat
    add esp, 16
    mov eax, 1
    jmp .done
.not_found:
    mov eax, ERR_ENOENT
.done:
    pop edi
    pop esi
    pop ebx
    ret

; int32_t romfs_fstat(void *handle, vfs_stat_t *out)
romfs_fstat:
    mov ecx, [esp + 4]         ; handle
    mov edx, [esp + 8]         ; out
    cmp dword [ecx + HANDLE_ISDIR], 0
    jne .dir
    push 2
    push dword [ecx + HANDLE_SIZE]
    push S_IFREG
    push edx
    call haku_module_fill_stat
    add esp, 16
    mov eax, 1
    ret
.dir:
    push 1
    push 0
    push S_IFDIR
    push edx
    call haku_module_fill_stat
    add esp, 16
    mov eax, 1
    ret

; int32_t romfs_opendir(const char *path, void **handle_out)
romfs_opendir:
    mov eax, [esp + 4]
    cmp byte [eax], '.'
    jne .not_dir
    cmp byte [eax + 1], 0
    jne .not_dir
    push HANDLE_BYTES
    call haku_module_alloc
    add esp, 4
    test eax, eax
    jz .no_memory
    mov dword [eax + HANDLE_DATA], 0
    mov dword [eax + HANDLE_SIZE], 0
    mov dword [eax + HANDLE_POS], 0
    mov dword [eax + HANDLE_ISDIR], 1
    mov ecx, [esp + 8]         ; handle_out
    mov [ecx], eax
    mov eax, 1
    ret
.not_dir:
    mov eax, ERR_ENOTDIR
    ret
.no_memory:
    mov eax, ERR_ENOMEM
    ret

; int32_t romfs_readdir(void *handle, uint32_t index, vfs_dirent_t *out,
;                       uint32_t *next_index)
romfs_readdir:
    push ebx
    mov ebx, [esp + 16]        ; out
    mov eax, [esp + 12]        ; index
    cmp eax, 0
    je .readme
    cmp eax, 1
    je .version
    xor eax, eax               ; end of directory
    pop ebx
    ret
.readme:
    push name_readme
    push DT_REG
    push 2
    push ebx
    call haku_module_fill_dirent
    add esp, 16
    jmp .advance
.version:
    push name_version
    push DT_REG
    push 3
    push ebx
    call haku_module_fill_dirent
    add esp, 16
.advance:
    mov ecx, [esp + 20]        ; next_index
    mov edx, [esp + 12]
    inc edx
    mov [ecx], edx
    mov eax, 1
    pop ebx
    ret

haku_module_init:
    push dword romfs_ops
    call haku_module_register_fs
    add esp, 4
    test eax, eax
    js .fail
    push dword VFS_MOUNT_RDONLY
    push dword fs_name
    push dword mount_prefix
    call haku_module_mount
    add esp, 12
    test eax, eax
    js .fail
    push dword romfs_loaded
    call haku_module_log
    add esp, 4
    xor eax, eax
    ret
.fail:
    ret

haku_module_exit:
    push dword romfs_exit
    call haku_module_log
    add esp, 4
    ret

section .rodata
fs_name db 'romfs', 0
mount_prefix db 'rom', 0
name_readme db 'README', 0
name_version db 'VERSION', 0
data_readme db 'Haku romfs: a read-only filesystem served by a '
            db 'loadable module.', 10
data_readme_end:
data_readme_length equ data_readme_end - data_readme
data_version db 'romfs 1.0 (haku module)', 10
data_version_end:
data_version_length equ data_version_end - data_version
romfs_loaded db 'ROMFS-MODULE-OK mounted at /rom', 10, 0
romfs_exit db 'ROMFS-MODULE-EXIT', 10, 0

; vfs_fs_ops_t: name, mount, open, close, read, write, seek, unlink, rename,
; mkdir, rmdir, stat, fstat, opendir, readdir, closedir, truncate, symlink,
; readlink, lstat, is_tty.
section .data
romfs_ops:
    dd fs_name
    dd romfs_mount
    dd romfs_open
    dd romfs_close
    dd romfs_read
    dd romfs_write
    dd romfs_seek
    dd romfs_readonly_path     ; unlink
    dd romfs_readonly_path     ; rename
    dd romfs_readonly_path     ; mkdir
    dd romfs_readonly_path     ; rmdir
    dd romfs_stat
    dd romfs_fstat
    dd romfs_opendir
    dd romfs_readdir
    dd romfs_close             ; closedir
    dd romfs_truncate
    dd 0                       ; symlink
    dd 0                       ; readlink
    dd 0                       ; lstat
    dd 0                       ; is_tty
