; Embeds the ELF64 test image in the kernel so boot64.c can load it from
; memory. The build assembles ring3_elf64.asm to a flat binary, runs
; tools/mkelf64.py over it, and points USER64_IMAGE at the result:
;
;   nasm -f win64 -DUSER64_IMAGE=build\ring3_elf64.elf ...
;
; %defstr quotes the path for incbin, so the command line needs no embedded
; quotes (which Windows argument handling and NASM's -D parsing disagree
; about). .rdata is copied verbatim by tools/pe2bin.py, so the image is
; readable from the boot path without a filesystem.
[bits 64]

%ifndef USER64_IMAGE
%error "USER64_IMAGE must name the ELF64 image to embed"
%endif

%defstr USER64_IMAGE_STR USER64_IMAGE

section .rdata align=16

global user64_image
global user64_image_end

user64_image:
    incbin USER64_IMAGE_STR
user64_image_end:
