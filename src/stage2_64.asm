; ============================================================
;  Elus OS - Estagio 2 para MODO LONGO (64-bit)
;  Maximo 8 setores = 4096 bytes
;  1. Ativa A20
;  2. Carrega o kernel em 0x100000
;  3. Configura GDT + paging 4-niveis (identity mapping)
;  4. Entra em modo longo e pula para o kernel
;  Requer compilador x86_64 para o kernel C.
; ============================================================
[BITS 16]
[ORG 0x7E00]

KERNEL_LBA    equ 5
KERNEL_CHUNKS equ 8          ; 8 x 64 setores = 512 KB

PML4_ADDR  equ 0x8000
PDPT_ADDR  equ 0x8100
PD_ADDR    equ 0x8200

stage2:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; ---- Ativar A20 ----
    call enable_a20

    ; ---- Carregar o kernel ----
    mov eax, KERNEL_LBA
    mov ecx, KERNEL_CHUNKS * 64
    mov bx, 0x1000
    mov es, bx
    xor bx, bx
    call read_sectors

    ; ---- GDT para modo longo ----
    lgdt [gdt64_ptr]

    ; ---- Ativar PAE e PGE (CR4) ----
    mov eax, cr4
    or eax, (1 << 5) | (1 << 7)
    mov cr4, eax

    ; ---- PML4 identity mapping ----
    mov dword [PML4_ADDR], PDPT_ADDR | 0x03
    mov dword [PDPT_ADDR], PD_ADDR | 0x03
    mov dword [PD_ADDR], 0x00000000 | 0x03 | (1 << 7)

    ; ---- CR3 com endereco do PML4 ----
    mov eax, PML4_ADDR
    mov cr3, eax

    ; ---- Ativar LME (EFER) ----
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; ---- Ativar paginacao (CR0) ----
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; ---- Salto longo para modo longo ----
    jmp 0x08:modo_longo

enable_a20:
    call wait_kbc
    mov al, 0xAD
    out 0x64, al
    call wait_kbc
    mov al, 0xD0
    out 0x64, al
    call wait_kbc
    in al, 0x60
    call wait_kbc
    mov al, 0xD1
    out 0x64, al
    call wait_kbc
    or al, 2
    out 0x60, al
    call wait_kbc
    mov al, 0xAE
    out 0x64, al
    call wait_kbc
    ret

wait_kbc:
    jmp $+2
    jmp $+2
    in al, 0x64
    test al, 2
    jnz wait_kbc
    ret

read_sectors:
    push es
    push bx
    push ax
    push cx

    movzx edx, word [cds_spt]
    movzx ebp, word [cds_heads]
    movzx edi, word [cds_spt]
    imul edi, ebp

    mov eax, [esp + 4 + 4]
    xor edx, edx
    div edi
    mov [chs_cyl], ax

    movzx eax, dx
    xor edx, edx
    div word [cds_spt]
    mov [chs_head], al
    mov [chs_sec], dl
    inc byte [chs_sec]

    pop cx
    pop ax
    pop bx
    pop es

.read_loop:
    push cx
    push bx
    push es

    mov ah, 0x02
    mov al, 1
    mov ch, [chs_cyl]
    mov cl, [chs_sec]
    shl ch, 6
    or cl, ch
    mov dh, [chs_head]
    mov dl, [cds_drive]
    int 0x13
    jc .read_error

    inc byte [chs_sec]
    cmp byte [chs_sec], 19
    jne .next_sector
    mov byte [chs_sec], 1
    inc byte [chs_head]
    cmp byte [chs_head], 2
    jne .next_sector
    mov byte [chs_head], 0
    inc word [chs_cyl]

.next_sector:
    pop es
    pop bx
    pop cx
    add bx, 512
    loop .read_loop
    ret

.read_error:
    pop cx
    pop bx
    pop es
    hlt
    jmp .read_error

cds_drive:   db 0x80
cds_spt:     dw 18
cds_heads:   dw 2
cds_sec:     db 1
cds_head:    db 0
cds_cyl:     dw 0
chs_sec:     db 1
chs_head:    db 0
chs_cyl:     dw 0

gdt64_start:
    dq 0
gdt64_code:
    dq 0x00209A0000000000
gdt64_data:
    dq 0x0000920000000000
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64_start - 1
    dq gdt64_start

[BITS 64]
modo_longo:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    jmp 0x100000
