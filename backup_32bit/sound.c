/* ============================================================
 *  Elus OS - Driver de Som Unificado (Sound Blaster 16 + PC Speaker)
 *  Totalmente assincrono / nao-bloqueante (acionado via PIT IRQ0 e SB16 IRQ5)
 * ============================================================ */
#include "sound.h"

static inline u8 inb(u16 p) {
    u8 r;
    __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p));
    return r;
}

static inline void outb(u16 p, u8 v) {
    __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p));
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

/* Informacoes globais de audio */
static struct sound_info g_snd = {
    .dev_type = SND_DEV_NONE,
    .sb_major = 0,
    .sb_minor = 0,
    .sb_base = 0x220,
    .sb_irq = 5,
    .sb_dma8 = 1,
    .sb_dma16 = 5,
    .volume = 80,
    .mute = 0,
    .is_playing = 0
};

/* ============================================================
 *  Sub-driver: Creative Sound Blaster 16 (ISA / QEMU)
 * ============================================================ */
#define SB_BASE       (g_snd.sb_base)
#define SB_MIX_ADDR   (SB_BASE + 0x04)
#define SB_MIX_DATA   (SB_BASE + 0x05)
#define SB_DSP_RESET  (SB_BASE + 0x06)
#define SB_DSP_READ   (SB_BASE + 0x0A)
#define SB_DSP_WRITE  (SB_BASE + 0x0C)
#define SB_DSP_STATUS (SB_BASE + 0x0E) /* Bit 7 = data ready / ACK IRQ 8-bit */
#define SB_DSP_ACK16  (SB_BASE + 0x0F) /* ACK IRQ 16-bit */

/* Buffer de DMA alinhado em 0x200000 (2 MB, nunca cruza limite de 64 KB) */
#define DMA_BUFFER_ADDR 0x200000
#define DMA_BUFFER_SIZE 16384
static u8 *const dma_buf = (u8 *)DMA_BUFFER_ADDR;

static void sb16_write(u8 v) {
    for (int i = 0; i < 30000; i++) {
        if (!(inb(SB_DSP_WRITE) & 0x80)) break;
        io_wait();
    }
    outb(SB_DSP_WRITE, v);
}

static u8 sb16_read(void) {
    for (int i = 0; i < 30000; i++) {
        if (inb(SB_DSP_STATUS) & 0x80) break;
        io_wait();
    }
    return inb(SB_DSP_READ);
}

static int sb16_reset(void) {
    outb(SB_DSP_RESET, 1);
    for (volatile int i = 0; i < 2000; i++) io_wait();
    outb(SB_DSP_RESET, 0);

    for (int i = 0; i < 30000; i++) {
        if (inb(SB_DSP_STATUS) & 0x80) {
            if (inb(SB_DSP_READ) == 0xAA) return 1; /* Sucesso no handshake */
        }
        io_wait();
    }
    return 0;
}

static void sb16_mixer_write(u8 reg, u8 val) {
    outb(SB_MIX_ADDR, reg);
    io_wait();
    outb(SB_MIX_DATA, val);
    io_wait();
}

static void sb16_apply_volume(int vol, int mute) {
    if (g_snd.dev_type != SND_DEV_SB16) return;
    if (mute || vol <= 0) {
        sb16_mixer_write(0x30, 0); /* Master L 5-bit */
        sb16_mixer_write(0x31, 0); /* Master R 5-bit */
        sb16_mixer_write(0x32, 0); /* Voice L 5-bit */
        sb16_mixer_write(0x33, 0); /* Voice R 5-bit */
        sb16_mixer_write(0x22, 0); /* Master SBPro comp */
        sb16_mixer_write(0x04, 0); /* Voice SBPro comp */
    } else {
        u8 v5 = (u8)((vol * 31) / 100);
        u8 val5 = (u8)(v5 << 3);
        u8 v4 = (u8)((vol * 15) / 100);
        u8 val4 = (u8)((v4 << 4) | v4);
        sb16_mixer_write(0x30, val5);
        sb16_mixer_write(0x31, val5);
        sb16_mixer_write(0x32, val5);
        sb16_mixer_write(0x33, val5);
        sb16_mixer_write(0x22, val4);
        sb16_mixer_write(0x04, val4);
    }
}

static void sb16_play_pcm(const u8 *data, u32 len, u32 sample_rate) {
    if (g_snd.dev_type != SND_DEV_SB16 || g_snd.mute || len == 0) return;
    if (len > DMA_BUFFER_SIZE) len = DMA_BUFFER_SIZE;

    /* Copia os dados para o buffer de DMA no limite seguro de ISA */
    for (u32 i = 0; i < len; i++) dma_buf[i] = data[i];

    /* Configura o Canal 1 de DMA (8-bit ISA) */
    outb(0x0A, 4 | 1);               /* Mascara canal 1 */
    outb(0x0C, 0);                   /* Reseta flip-flop de byte pointer */
    outb(0x0B, 0x49);                /* Single mode, incremento, leitura de memoria, canal 1 */
    u32 phys = DMA_BUFFER_ADDR;
    outb(0x02, (u8)(phys & 0xFF));
    outb(0x02, (u8)((phys >> 8) & 0xFF));
    outb(0x83, (u8)((phys >> 16) & 0xFF)); /* Registrador de pagina (porta 0x83) */

    u16 count = (u16)(len - 1);
    outb(0x03, (u8)(count & 0xFF));
    outb(0x03, (u8)((count >> 8) & 0xFF));
    outb(0x0A, 1);                   /* Desmascara canal 1 */

    /* Configura taxa de amostragem no DSP */
    sb16_write(0x41);
    sb16_write((u8)((sample_rate >> 8) & 0xFF));
    sb16_write((u8)(sample_rate & 0xFF));

    /* Inicia transferencia DMA 8-bit mono sem sinal */
    sb16_write(0xC0);                /* Comando SB16 8-bit output single cycle */
    sb16_write(0x00);                /* Modo: unsigned mono */
    sb16_write((u8)(count & 0xFF));
    sb16_write((u8)((count >> 8) & 0xFF));

    g_snd.is_playing = 1;
}

/* ============================================================
 *  Sub-driver: PC Speaker (PIT Canal 2, porta 0x61)
 * ============================================================ */
static void speaker_tone(u32 freq) {
    if (freq == 0 || g_snd.mute || g_snd.volume == 0) {
        u8 t = inb(0x61);
        outb(0x61, t & ~3);
        return;
    }
    u32 div = 1193180 / freq;
    if (div < 20) div = 20;
    if (div > 65535) div = 65535;

    outb(0x43, 0xB6);                /* Canal 2, LSB/MSB, square wave */
    outb(0x42, (u8)(div & 0xFF));
    outb(0x42, (u8)((div >> 8) & 0xFF));

    u8 t = inb(0x61);
    if ((t & 3) != 3) outb(0x61, t | 3);
}

static void speaker_off(void) {
    u8 t = inb(0x61);
    outb(0x61, t & ~3);
}

/* ============================================================
 *  Sequenciador assincrono (fila de notas nao-bloqueante)
 * ============================================================ */
#define MAX_SEQ_NOTES 96
struct seq_note {
    u16 freq;
    u16 ticks;                       /* Duracao em passos de 10ms */
};

static struct seq_note seq_queue[MAX_SEQ_NOTES];
static int seq_len = 0;
static int seq_idx = 0;
static int seq_tick = 0;

void snd_stop(void) {
    seq_len = 0;
    seq_idx = 0;
    seq_tick = 0;
    speaker_off();
    g_snd.is_playing = 0;
}

void snd_play_notes(const u16 *notes, const u16 *durations, int count) {
    if (count <= 0) return;
    if (count > MAX_SEQ_NOTES) count = MAX_SEQ_NOTES;

    snd_stop();

    for (int i = 0; i < count; i++) {
        seq_queue[i].freq = notes[i];
        seq_queue[i].ticks = durations[i] ? durations[i] : 1;
    }
    seq_len = count;
    seq_idx = 0;
    seq_tick = 0;
    g_snd.is_playing = 1;

    speaker_tone(seq_queue[0].freq);
}

void snd_play_tone(u32 freq, u32 ms) {
    u16 dur = (u16)((ms + 9) / 10);
    if (dur == 0) dur = 1;
    u16 f = (u16)freq;
    snd_play_notes(&f, &dur, 1);
}

/* Chamado pelo timer IRQ0 a cada 10ms */
void snd_tick(void) {
    if (seq_idx < seq_len) {
        seq_tick++;
        if (seq_tick >= seq_queue[seq_idx].ticks) {
            seq_tick = 0;
            seq_idx++;
            if (seq_idx < seq_len) {
                speaker_tone(seq_queue[seq_idx].freq);
            } else {
                speaker_off();
                g_snd.is_playing = 0;
            }
        }
    }
}

/* Chamado na IRQ 5 da Sound Blaster */
void snd_irq_handler(void) {
    inb(SB_DSP_STATUS);              /* Reconhece IRQ 8-bit */
    inb(SB_DSP_ACK16);               /* Reconhece IRQ 16-bit */
    g_snd.is_playing = 0;
}

/* ============================================================
 *  Inicializacao Geral do Sistema de Som
 * ============================================================ */
void snd_init(void) {
    /* Tenta detectar a Sound Blaster 16 na porta padrao 0x220 */
    if (sb16_reset()) {
        sb16_write(0xE1);            /* Comando: obter versao do DSP */
        g_snd.sb_major = sb16_read();
        g_snd.sb_minor = sb16_read();

        if (g_snd.sb_major >= 4) {
            g_snd.dev_type = SND_DEV_SB16;
            /* Configura o Mixer: liga IRQ 5 e DMA 1 / 5 */
            sb16_mixer_write(0x80, 0x02); /* Bit 1 = IRQ 5 */
            sb16_mixer_write(0x81, 0x22); /* Bit 1 = DMA 1, Bit 5 = DMA 5 */
            sb16_apply_volume(g_snd.volume, g_snd.mute);
            return;
        }
    }

    /* Fallback: Alto-falante do PC */
    g_snd.dev_type = SND_DEV_PC_SPEAKER;
    speaker_off();
}

void snd_set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_snd.volume = vol;
    sb16_apply_volume(vol, g_snd.mute);
}

int snd_get_volume(void) {
    return g_snd.volume;
}

void snd_set_mute(int mute) {
    g_snd.mute = mute ? 1 : 0;
    if (g_snd.mute) speaker_off();
    sb16_apply_volume(g_snd.volume, g_snd.mute);
}

int snd_is_muted(void) {
    return g_snd.mute;
}

const struct sound_info *snd_get_info(void) {
    return &g_snd;
}

const char *snd_get_device_name(void) {
    if (g_snd.dev_type == SND_DEV_SB16) return "Creative Sound Blaster 16";
    if (g_snd.dev_type == SND_DEV_PC_SPEAKER) return "Alto-falante do PC (PIT 8254)";
    return "Nenhum dispositivo de som";
}

/* ============================================================
 *  Sintese e Efeitos Sonoros
 * ============================================================ */

/* Gerador de onda senoidal simples aproximada para audio PCM */
static void synth_sine(u8 *dst, u32 len, u32 sample_rate, u32 freq, u32 decay_rate) {
    /* Tabela de 16 pontos de um quarto de seno [0..255] */
    static const u8 s16[17] = { 128, 140, 152, 164, 175, 186, 196, 204, 212, 218, 222, 225, 227, 228, 227, 225, 222 };
    u32 phase_step = (freq * 64 * 256) / sample_rate;
    u32 phase = 0;
    u32 amp = 256;

    for (u32 i = 0; i < len; i++) {
        u32 p = (phase >> 8) & 63;
        int s = 0;
        if (p < 16) s = s16[p] - 128;
        else if (p < 32) s = s16[32 - p] - 128;
        else if (p < 48) s = -(s16[p - 32] - 128);
        else s = -(s16[64 - p] - 128);

        s = (s * (int)amp) >> 8;
        int val = 128 + s;
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        dst[i] = (u8)val;

        phase += phase_step;
        if (amp > decay_rate) amp -= decay_rate;
        else amp = 0;
    }
}

void snd_fx_click(void) {
    if (g_snd.mute || g_snd.volume <= 0) return;
    if (g_snd.dev_type == SND_DEV_SB16) {
        static u8 pcm_click[300];
        synth_sine(pcm_click, 300, 11025, 950, 1);
        sb16_play_pcm(pcm_click, 300, 11025);
    } else {
        u16 f = 900;
        u16 d = 1; /* 10 ms */
        snd_play_notes(&f, &d, 1);
    }
}

void snd_fx_ok(void) {
    if (g_snd.mute || g_snd.volume <= 0) return;
    if (g_snd.dev_type == SND_DEV_SB16) {
        static u8 pcm_ok[1400];
        synth_sine(pcm_ok, 1400, 11025, 784, 1);
        sb16_play_pcm(pcm_ok, 1400, 11025);
    } else {
        static const u16 notes[2] = { 587, 880 };
        static const u16 durs[2] = { 3, 6 };
        snd_play_notes(notes, durs, 2);
    }
}

void snd_fx_err(void) {
    if (g_snd.mute || g_snd.volume <= 0) return;
    static const u16 notes[3] = { 220, 0, 165 };
    static const u16 durs[3] = { 4, 2, 6 };
    snd_play_notes(notes, durs, 3);
}

void snd_fx_startup(void) {
    if (g_snd.mute || g_snd.volume <= 0) return;
    /* Acorde majestoso de boot: Do4, Sol4, Do5, Mi5, Sol5 */
    static const u16 notes[6] = { 262, 392, 523, 659, 784, 1046 };
    static const u16 durs[6] = { 5, 5, 6, 7, 9, 14 };
    snd_play_notes(notes, durs, 6);
}

void snd_fx_shutdown(void) {
    if (g_snd.mute || g_snd.volume <= 0) return;
    static const u16 notes[4] = { 784, 659, 523, 392 };
    static const u16 durs[4] = { 6, 6, 8, 12 };
    snd_play_notes(notes, durs, 4);
}

void snd_fx_coin(void) {
    if (g_snd.mute || g_snd.volume <= 0) return;
    static const u16 notes[2] = { 988, 1319 };
    static const u16 durs[2] = { 4, 12 };
    snd_play_notes(notes, durs, 2);
}

void snd_fx_laser(void) {
    if (g_snd.mute || g_snd.volume <= 0) return;
    static const u16 notes[6] = { 1200, 1000, 800, 600, 450, 250 };
    static const u16 durs[6] = { 1, 1, 1, 1, 1, 2 };
    snd_play_notes(notes, durs, 6);
}

void snd_fx_digital_test(void) {
    if (g_snd.mute || g_snd.volume <= 0) return;
    if (g_snd.dev_type == SND_DEV_SB16) {
        static u8 pcm_bell[4000];
        synth_sine(pcm_bell, 4000, 11025, 660, 1);
        sb16_play_pcm(pcm_bell, 4000, 11025);
    } else {
        snd_fx_ok();
    }
}

void snd_play_piano_key(int key) {
    static const u16 piano_freqs[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };
    if (key < 0 || key >= 8) return;
    u16 f = piano_freqs[key];
    u16 d = 10; /* 100 ms */
    snd_play_notes(&f, &d, 1);
}

void snd_play_melody(int id) {
    if (id == 0) {
        /* Tema Elus Glass */
        static const u16 n[] = { 392, 523, 659, 784, 659, 784, 1046, 0, 784, 1046 };
        static const u16 d[] = { 4, 4, 4, 6, 4, 6, 10, 3, 5, 14 };
        snd_play_notes(n, d, sizeof(n) / sizeof(n[0]));
    } else if (id == 1) {
        /* Ode a Alegria */
        static const u16 n[] = { 330, 330, 349, 392, 392, 349, 330, 294, 262, 262, 294, 330, 330, 294, 294 };
        static const u16 d[] = { 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 9, 3, 12 };
        snd_play_notes(n, d, sizeof(n) / sizeof(n[0]));
    } else if (id == 2) {
        /* Fanfarra da Vitoria */
        static const u16 n[] = { 392, 392, 392, 523, 0, 659, 0, 784 };
        static const u16 d[] = { 3, 3, 3, 8, 2, 8, 2, 16 };
        snd_play_notes(n, d, sizeof(n) / sizeof(n[0]));
    }
}
