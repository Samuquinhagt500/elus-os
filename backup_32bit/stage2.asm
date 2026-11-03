; ============================================================
;  Elus OS - Estagio 2 (maximo 4 setores = 2048 bytes)
;   1. copia a fonte 8x16 da BIOS para 0x30000
;   2. carrega o kernel (atualmente ~43 KB) em 0x10000
;   3. procura e ativa o modo VESA 1024x768x32 (framebuffer linear)
;   4. liga A20, carrega a GDT e entra em modo protegido de 32 bits
;   5. pula para o kernel
; ============================================================
[BITS 16]
[ORG 0x7E00]

KERNEL_LBA    equ 5
KERNEL_CHUNKS equ 3             ; 3 x 64 setores = 96 KB (carrega ate 0x28000; fonte BIOS em 0x30000)
VBE_INFO      equ 0x9000
VBE_MODE      equ 0x9200

stage2:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov si, msg_boot
    call print

    ; ---- 1) fonte 8x16 da BIOS -> 0x3000:0000 ----
    mov ax, 0x1130
    mov bh, 6
    int 0x10                    ; devolve ES:BP apontando pra fonte
    mov ax, es
    mov ds, ax
    mov si, bp
    mov ax, 0x3000
    mov es, ax
    xor di, di
    mov cx, 2048
    cld
    rep movsw
    xor ax, ax
    mov ds, ax
    mov es, ax

    ; ---- 2) carrega o kernel em 0x1000:0000 ----
    mov cx, KERNEL_CHUNKS
.load:
    push cx
    mov si, dap2
    mov ah, 0x42
    mov dl, [0x0500]
    int 0x13
    jc  disk_error
    pop cx
    add word [dap2_seg], 0x0800
    add word [dap2_lba], 64
    loop .load

    ; ---- 3) VESA: procura 1024x768x32 na lista de modos ----
    mov di, VBE_INFO
    mov dword [di], 'VBE2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne vbe_fail
    mov si, [VBE_INFO + 0x0E]   ; ponteiro (off:seg) da lista de modos
    mov ax, [VBE_INFO + 0x10]
    mov fs, ax
.next:
    mov cx, [fs:si]
    add si, 2
    cmp cx, 0xFFFF
    je  vbe_fail
    mov [cur_mode], cx
    push si
    mov di, VBE_MODE
    mov ax, 0x4F01
    int 0x10
    pop si
    cmp ax, 0x004F
    jne .next
    mov ax, [VBE_MODE]
    and ax, 0x0081              ; suportado + framebuffer linear
    cmp ax, 0x0081
    jne .next
    cmp word [VBE_MODE + 0x12], 1024
    jne .next
    cmp word [VBE_MODE + 0x14], 768
    jne .next
    cmp byte [VBE_MODE + 0x19], 32
    jne .next

    mov ax, 0x4F02              ; ativa o modo (bit 14 = LFB)
    mov bx, [cur_mode]
    or  bx, 0x4000
    int 0x10
    cmp ax, 0x004F
    jne vbe_fail

    ; bootinfo em 0x600: fb(4) pitch(2) w(2) h(2) bpp(1)
    mov eax, [VBE_MODE + 0x28]
    mov [0x0600], eax
    mov ax, [VBE_MODE + 0x10]
    mov [0x0604], ax
    mov word [0x0606], 1024
    mov word [0x0608], 768
    mov byte [0x060A], 32

    ; ---- 4) A20 + GDT + modo protegido ----
    cli
    in  al, 0x92
    or  al, 2
    out 0x92, al
    lgdt [gdt_desc]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry

[BITS 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp 0x10000                 ; ---- 5) kernel! ----

[BITS 16]
disk_error:
    mov si, msg_disk
    jmp fatal
vbe_fail:
    mov si, msg_vbe
fatal:
    call print
.h: hlt
    jmp .h

print:
    lodsb
    or al, al
    jz .d
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp print
.d: ret

msg_boot db "Elus OS: carregando...", 13, 10, 0
msg_disk db "Erro ao ler o kernel.", 0
msg_vbe  db "Modo VESA 1024x768x32 nao encontrado.", 0
cur_mode dw 0

align 4
dap2:
    db 0x10, 0
    dw 64
    dw 0x0000
dap2_seg: dw 0x1000
dap2_lba: dd KERNEL_LBA
          dd 0

align 8
gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00   ; 0x08: codigo 32-bit, base 0, 4 GB
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00   ; 0x10: dados 32-bit, base 0, 4 GB
gdt_end:
gdt_desc:
    dw gdt_end - gdt - 1
    dd gdt

times 2048-($-$$) db 0
