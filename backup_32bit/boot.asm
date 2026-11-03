; ============================================================
;  Elus OS - Estagio 1 (boot sector, 512 bytes)
;  A BIOS carrega este setor em 0000:7C00 e pula pra ca.
;  Missao: carregar o estagio 2 (setores 1..4) em 0000:7E00.
; ============================================================
[BITS 16]
[ORG 0x7C00]

STAGE2_SECTORS equ 4

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [0x0500], dl            ; guarda o numero do drive de boot

    mov si, dap                 ; le o estagio 2 via INT 13h estendido (LBA)
    mov ah, 0x42
    int 0x13
    jc  disk_error

    jmp 0x0000:0x7E00

disk_error:
    mov si, msg_err
.p: lodsb
    or al, al
    jz .h
    mov ah, 0x0E
    int 0x10
    jmp .p
.h: hlt
    jmp .h

align 4
dap:                            ; Disk Address Packet
    db 0x10, 0
    dw STAGE2_SECTORS
    dw 0x7E00                   ; offset
    dw 0x0000                   ; segmento
    dd 1                        ; LBA 1
    dd 0

msg_err db "Erro ao ler o disco.", 0

times 510-($-$$) db 0
dw 0xAA55
