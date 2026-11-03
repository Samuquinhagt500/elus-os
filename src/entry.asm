; Entrada do kernel (modo protegido, 0x10000):
;   - zera o .bss e chama kmain()
;   - stubs das 48 interrupcoes (32 excecoes da CPU + 16 IRQs do PIC)
[BITS 32]
section .text
global _start
global biosfont_save
extern kmain
extern isr_handler
extern __bss_start
extern __bss_end

_start:
    cld
    ; salva a fonte 8x16 da BIOS antes de zerar o .bss
    mov esi, 0x40000
    mov edi, biosfont_save
    mov ecx, 1024
    rep movsd
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    call kmain
.h: hlt
    jmp .h

%macro ISR_NOERR 1
isr%1:
    push dword 0                ; codigo de erro falso
    push dword %1
    jmp isr_common
%endmacro
%macro ISR_ERR 1
isr%1:                          ; a CPU ja empilhou o codigo de erro
    push dword %1
    jmp isr_common
%endmacro

%assign i 0
%rep 48
%if (i == 8) || (i >= 10 && i <= 14) || (i == 17) || (i == 30)
    ISR_ERR %[i]
%else
    ISR_NOERR %[i]
%endif
%assign i i+1
%endrep

isr_common:
    pushad
    cld
    push esp                    ; ponteiro para struct regs
    call isr_handler
    add esp, 4
    popad
    add esp, 8                  ; descarta vetor + codigo de erro
    iretd

section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dd isr %+ i
%assign i i+1
%endrep

section .data
global biosfont_save
biosfont_save: times 4096 db 0
