; Haku x86-64 system call numbers used by the 64-bit test images.
;
; The 64-bit kernel is still on the small INT 0x80 table in
; kernel/arch/x86_64/kernel/syscall64.c (RAX number, RDI/RSI/RDX arguments),
; not the i386 numbers in abi.inc. Keep these in step with K64_SYS_* in
; kernel/arch/x86_64/kernel/haku64.h.

%define HAKU64_SYSCALL_VECTOR 0x80

%define HAKU64_SYS_WRITE 1
%define HAKU64_SYS_EXIT  2
