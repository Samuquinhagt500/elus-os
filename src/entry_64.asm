; ============================================================
;  Elus OS - Entrada do kernel em MODO LONGO (64-bit)
;  Carregado em 0x100000 (1 MB) pelo stage2_64 em modo longo.
;  Zera .bss e chama kmain().
;  Requer compilador x86_64 para o kernel C.
; ============================================================
[BITS 64]
section .text
global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    cld
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor rax, rax
    rep stosb
    call kmain
.hlt_loop:
    hlt
    jmp .hlt_loop
