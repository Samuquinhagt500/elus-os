Elus OS v0.1 'Elus Glass' - SO de aventura com visual de vidro liquido (ASM + C)

Requisitos no Windows:
  - NASM        (no PATH)
  - QEMU        (qemu-system-i386)
  - MinGW i686  (i686-w64-mingw32-gcc/ld/objcopy no PATH)
    -> MSYS2: pacman -S mingw-w64-i686-gcc mingw-w64-i686-binutils
       e adicione C:\msys64\mingw32\bin ao PATH

Uso:
  build.bat   -> compila e gera build\os.img
  run.bat     -> abre no QEMU
  VS Code: Ctrl+Shift+B (tarefa Build)

Estrutura:
  src\boot.asm    estagio 1 (boot sector)
  src\stage2.asm  fonte BIOS, carrega kernel, VESA 1024x768x32, modo protegido
  src\entry.asm   entrada do kernel (zera .bss, chama kmain)
  src\sound.h     driver de som unificado (Sound Blaster 16 + PC Speaker)
  src\sound.c     implementacao SB16 (DSP/DMA/Mixer) e sintetizador nao-bloqueante
  src\kernel.c    graficos (vidro/desfoque), interrupcoes, mouse, teclado, janelas, dock
  src\linker.ld   mapa de memoria do kernel (carregado em 0x10000)

Novidades da v0.5 (Subsistema e Driver de Som):
  - Driver de Som Completo e Unificado (src\sound.h e src\sound.c):
    * Creative Sound Blaster 16: deteccao automatica via DSP (porta 0x220), versao 4.xx,
      roteamento de IRQ 5 e canais DMA 1 (8-bit) / 5 (16-bit), controle de mixer estereo,
      e reproducao de amostras de audio digital PCM via DMA.
    * Alto-falante do PC (PIT 8254 Canal 2): fallback automatico em qualquer maquina x86.
    * 100% assincrono e nao-bloqueante: notas, melodias e efeitos tocam em segundo plano
      via temporizador de 100 Hz (IRQ0) sem travar a interface grafica.
  - Novo aplicativo 'Som & Áudio' no Dock:
    * Informacoes e status do hardware em tempo real (SB16 vs PC Speaker, DSP, IRQ, DMA).
    * Controle de volume geral (0 a 100%) e chave de Mudo com persistencia na CMOS (regs 0x38-0x39).
    * Soundboard com efeitos sonoros (Inicio, Sucesso, Erro, Moeda, Laser, Melodia, Teste PCM).
    * Mini sintetizador / piano interativo com 8 notas (Do a Do agudo), tocavel com mouse ou teclado (teclas 1 a 8).
  - Correcoes de erros no sistema:
    * build.bat corrigido para detectar ferramentas MinGW sem prefixo (ld/objcopy/gcc).
    * stage2.asm ampliado para carregar ate 96 KB de kernel com folga.
    * run.bat atualizado com suporte a audio no QEMU (DirectSound, SB16 e PC Speaker).
    * Removido atraso sincrono de beeps que congelava a interface ao clicar.
    * Corrigida sobreposicao potencial de buffers de desfoque (TMPA/TMPB).
    * Sincronizacao com flag UIP da CMOS para leitura estavel do relogio RTC.

Novidades da v0.2:
  - IDT + PIC + timer (100 Hz): teclado e mouse por interrupcao, CPU em hlt quando ociosa
  - Tela azul para excecoes da CPU (qualquer tecla reinicia)
  - Fonte com acentos (a, e, i, o, u, c e til/circunflexo/crase em maiusculas)
  - Teclado ABNT2 com teclas mortas (acento + letra) ; F12 alterna PT/US
  - Duplo clique nos icones, botao minimizar, cursor piscando no Bloco de Notas

Novidades da v0.4:
  - Sistema de aplicativos modular (struct App com draw/click/drag/key em src/kernel.c):
    para criar um app novo basta escrever as callbacks e adicionar uma linha em APPS[]
  - Novo app Personalizar: 6 papeis de parede, 6 cores de destaque, tema escuro/claro,
    transparencia do vidro (controle deslizante), desfoque e widget liga/desliga, teclado PT/US
  - Configuracoes agora sao gravadas na CMOS (registros 0x30-0x37) durante a sessao;
    persistencia em disco fica a cargo do driver de disco (em desenvolvimento)

Novidades da v0.3 (visual Elus Glass):
  - Paineis de vidro: desfoque do que esta atras, translucidez, borda de luz, sombras suaves
  - Dock flutuante (substitui a barra de tarefas), barra superior em capsula, widget de relogio e data
  - Papel de parede em degrade gerado por codigo; icones em quadrados arredondados
  - Texto suavizado (antialias) e proporcional; botoes vermelho/amarelo = fechar/minimizar
  - Ao arrastar uma janela o desfoque fica mais leve para manter a fluidez

Dica: para testar a tela azul no Windows: set EXTRA=-DTEST_UD2 e rode build.bat (depois: set EXTRA=).
No WSL/MSYS2: EXTRA=-DTEST_UD2 ./build.sh

Novidades da v0.6 (Pilha de Rede + Navegador):
  - Driver de Rede Realtek RTL8139 (PCI) em src\net.c:
    * Detecao automatica da placa via BAR0 (portas de E/S), IRQ 11 e endereco MAC.
    * Empacotamento/desempacotamento de frames Ethernet com checagem de endereco MAC.
    * ARP (resolucao de MAC via broadcast com cache), IPv4 (cabecalho, fragmentos de padding).
    * ICMP: Echo Request/Reply (ping) com tempo de resposta (RTT).
    * UDP: envio e recepcao com checksum de cabecalho pseudo e de dados.
    * TCP cliente: handshake SYN/SYN-ACK, envio de segmentos PSH+ACK com retransmissao,
      janela de recepcao, ACKs, tratamento de FIN e RST, reconexao com timeout.
    * DNS: consulta A (tipo 1, classe IN) para o servidor configurado, com reintentos.
    * HTTP/1.0 cliente: GET com cabecalho Host/User-Agent, suporte a redirects (3xx,
      ate 5 saltos), Transfer-Encoding: chunked e Content-Length, conexao keep-alive
      ate o FIN do servidor, e entrega parcial em caso de timeout.
  - Novo aplicativo 'Navegador' no Dock:
    * Barra de endereco com edicao de texto, botoes Voltar, Inicio, Recarregar e Ir.
    * Renderizador HTML proprio: h1-h3, p, ul/li, br, hr, strong/b, a (links), pre,
      blockquote, img (alt), td/th, tabela simples, entidades HTML (&amp; &lt; &gt; ...)
      e quebra de linha automática com justificativa de glifos.
    * Barra de rolagem funcional (arrastar/clicar), teclas de seta, Page Up/Down,
      Home/End para navegar na pagina.
    * Historico de ate 6 paginas, resolucao de URLs relativos (links href), e pagina
      inicial embutida ('Elus Inicio').
    * HTTPS nao e suportado (use enderecos http://).
  - App 'Rede' ja existente: exibe MAC, IP, gateway, DNS, contadores TX/RX e botao Ping.

Exemplo de rede no QEMU (host = 10.0.2.2):
  -build.bat (gera os.img)
  -run.bat (QEMU com -netdev user,id=u1 -device rtl8139,netdev=u1)
  -No navegador do Elus OS: digite http://example.com/ ou http://10.0.2.2:8000/ (servidor local do host)

Novidades da v0.7 (Driver de Video + Imagens no Navegador):
  - Driver de Video (src\video.h e src\video.c):
    * Abstrai o framebuffer VESA linear (modo 1024x768x32 setado pelo estagio 2).
    * Operacoes por software no backbuffer com copia para o framebuffer real:
      put_pixel, get_pixel, rect, rect_alpha, fill_rr (borda arredondada),
      fill_rr_gradient (degrade vertical), blit, copy_to_fb.
    * Nao há aceleracao por hardware (nao existem drivers de GPU neste SO);
      tudo e renderizado por CPU. Para melhor desempenho, evite redesenhar
      a tela inteira a cada frame.
  - Imagens no Navegador:
    * Suporta BMP nao comprimido (BI_RGB, 24 ou 32 bits) via tag <img src="...">
      com resolucao de URLs relativos.
    * Outros formatos (PNG, JPEG, GIF, WebP) mostram um placeholder "[imagem]";
      seriam necessarios decodificadores dedicados para cada formato.
    * Videos da web (MP4, WebM, H.264, etc.) NAO sao suportados: nao ha
      codecs de video neste SO. Uma tag <video> ou link para video mostra
      um aviso de "nao suportado".

Novidades da v0.8 (Codec de vídeo caseiro + Reprodutor + Preparação 64-bit):
  - Codec de vídeo caseiro (src\video_codec.h e src\video_codec.c):
    * Formato .elusvid: cabecalho (32 bytes) + sequencia de frames comprimidos RLE.
    * Formato de pixel: ARGB 32-bit, sem dependencias externas.
    * Compressao RLE (run-length encoding) simples para frames com areas de cor solida.
    * Funcoes: elusvid_open(), elusvid_decode_frame(), elusvid_next_frame(),
      elusvid_encode_rle(), elusvid_build_file().
    * Limitacao: formato proprio do Elus OS, nao e compatível com MP4/WebM/H.264.
  - Reprodutor de vídeo (app "Video" no Dock, integrado em src\kernel.c):
    * Carrega arquivos .elusvid em memoria e reproduz quadro a quadro.
    * Controles: espaco = play/pause, N = proximo frame.
    * Barra de titulo, informacao de resolucao, botoes de controle.
    * Buffer estatico maximo: 320x240 pixels (ajustavel em VP_MAX_W/VP_MAX_H).
  - Preparacao para 64-bit (modo longo):
    * src\entry_64.asm: entrada do kernel em modo longo (64-bit).
    * src\stage2_64.asm: estagio 2 com paging PML4/PDPT/PD e entrada em modo longo.
    * src\linker_64.ld: mapa de memoria para kernel em 0x100000 (1 MB).
    * Para completar a migracao 64-bit eh necessario:
        1. Instalar compilador x86_64: pacman -S mingw-w64-x86_64-gcc (MSYS2)
        2. Adaptar todo o codigo C para ponteiros de 64-bit e calling convention System V AMD64
        3. Usar entry_64.asm, stage2_64.asm e linker_64.ld no build
    * Backup do estado 32-bit funcional: backup_32bit/

Novidades da v0.9 (Player de Som):
  - Novo aplicativo "Player de Som" no Dock (icone 15):
    * Toca tons com frequencia personalizada (digite Hz e pressione Enter)
    * Tons presets: 440 Hz (La), 523 Hz (Do), 659 Hz (Mi), 784 Hz (Sol), 880 Hz (La), 1047 Hz (Do)
    * Melodias: Inicio, Ode a Alegria, Fanfarra
    * Efeitos sonoros: Clique, OK, Erro, Moeda, Laser
    * Piano interativo: teclas 1 a 8 do teclado (Do, Re, Mi, Fa, Sol, La, Si, Do)
    * Espaco = tocar tom de teste 440 Hz
    * Controle de volume e Mudo (persistido na CMOS)
    * Barra de progresso visual durante a reproducao
    * Indicador de dispositivo ativo (SB16 DSP ou PC Speaker)
    * Todo o audio e 100% nao-bloqueante (driver de som assincrono via IRQ0)

Novidades da v1.0 (Driver de Disco + Sistema de Arquivos FAT):
  - Driver de Disco IDE/ATA (src\disk.h e src\disk.c):
    * Modo PIO (Programmed I/O) para leitura/escrita de setores via portas 0x1F0-0x1F7.
    * Detecção automática de disco master e slave no canal primário.
    * Comando IDENTIFY para ler modelo, tamanho e total de setores (LBA28).
    * Suporte a discos até 128 GB (LBA28); escrita desabilitada por segurança.
    * Estrutura disk_info com modelo, setores totais, tamanho em MB.
  - Sistema de Arquivos FAT12/FAT16/FAT32 (src\fs.h e src\fs.c):
    * Somente leitura (suficiente para carregar arquivos de mídia, config, etc.).
    * Análise do BPB (BIOS Parameter Block) do setor de boot.
    * Navegação por diretório raiz (incluindo subdiretórios via encadeamento de clusters FAT32).
    * APIs: fs_mount(), fs_open(), fs_read(), fs_close(), fs_size(), fs_exists(), fs_list_root().
    * Cache de setor de 512 bytes para leitura sequencial.
    * Até 8 arquivos abertos simultaneamente.
  - Inicialização no kmain: disk_init() detecta o disco; fs_mount(0) monta FAT no disco primário master.
  - Limite do kernel aumentado para 5 chunks (160 KB) para acomodar o novo código.

Novidades da v1.1 (Gerenciador de Arquivos):
  - Novo aplicativo "Arquivos" no Dock (icone 16, pasta laranja):
    * Lista arquivos do diretório raiz do disco FAT montado
    * Navegação com setas cima/baixo, selecao visual destacada
    * Botao "Atualizar" para reler o diretorio
    * Botao "Abrir" para abrir o arquivo selecionado
    * Exibicao de arquivos de texto diretamente na janela
    * Arquivos binarios sao identificados (nao sao texto)
    * Barra de rolagem funcional quando ha mais arquigos que cabem na tela
    * Teclas: setas para navegar, Enter para abrir, R para atualizar
  - Integracao com o sistema de arquivos FAT via fs_list_root(), fs_open(), fs_read()
  - Barra de status com mensagens temporarias (ex: "Arquivos: 5 item(s)")

Novidades da v1.2 (Escrita em Disco):
  - Driver de Disco IDE/ATA: escrita de setores habilitada (CMD_WRITE_PIO, 0x30).
    Agora e possivel gravar dados diretamente no disco, nao apenas ler.
  - Sistema de Arquivos FAT: funcao fs_write_file() para criar ou sobrescrever
    arquivos no diretorio raiz.
    * Alocacao encadeada de clusters na FAT (marca EOC no ultimo cluster).
    * Escritura de dados cluster por cluster.
    * Criacao de entrada no diretorio raiz (slot livre ou fim do diretorio).
    * Suporta FAT12, FAT16 e FAT32.
  - Exemplo de uso: fs_write_file("NOTA.TXT", "Hello Elus", 12);

Exemplo de rede no QEMU (host = 10.0.2.2):
  -build.bat (gera os.img)
  -run.bat (QEMU com -netdev user,id=u1 -device rtl8139,netdev=u1)
  -No navegador do Elus OS: digite http://example.com/ ou http://10.0.2.2:8000/ (servidor local do host)
