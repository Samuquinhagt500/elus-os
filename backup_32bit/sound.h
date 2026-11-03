#ifndef SOUND_H
#define SOUND_H

typedef unsigned char  u8;
typedef signed char    s8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define SND_DEV_NONE       0
#define SND_DEV_PC_SPEAKER 1
#define SND_DEV_SB16       2

struct sound_info {
    int dev_type;          /* SND_DEV_* */
    u8 sb_major;           /* Versao principal do DSP (ex: 4) */
    u8 sb_minor;           /* Versao secundaria do DSP (ex: 13) */
    u16 sb_base;           /* Porta base (0x220) */
    u8 sb_irq;             /* Linha de interrupcao IRQ (5) */
    u8 sb_dma8;            /* Canal DMA 8-bit (1) */
    u8 sb_dma16;           /* Canal DMA 16-bit (5) */
    int volume;            /* 0..100% */
    int mute;              /* 0 ou 1 */
    int is_playing;        /* 1 se esta tocando som no momento */
};

/* Inicializacao e controle de hardware */
void snd_init(void);
void snd_tick(void);               /* Chamado a cada 10ms (100 Hz IRQ0 PIT) */
void snd_irq_handler(void);        /* Chamado na IRQ 5 (Sound Blaster 16) */
void snd_set_volume(int vol);
int  snd_get_volume(void);
void snd_set_mute(int mute);
int  snd_is_muted(void);
const struct sound_info *snd_get_info(void);
const char *snd_get_device_name(void);

/* Sons pontuais e melodias (100% nao-bloqueantes) */
void snd_play_tone(u32 freq, u32 ms);
void snd_play_notes(const u16 *notes, const u16 *durations, int count);
void snd_stop(void);

/* Efeitos sonoros do sistema operacional */
void snd_fx_click(void);           /* Clique sutil de botao / dock */
void snd_fx_ok(void);              /* Acorde de confirmacao / sucesso */
void snd_fx_err(void);             /* Tom de erro / alerta */
void snd_fx_startup(void);         /* Tema musical de inicializacao do Elus OS */
void snd_fx_shutdown(void);        /* Melodia suave de encerramento */
void snd_fx_coin(void);            /* Efeito sonoro retrô */
void snd_fx_laser(void);           /* Sweep sonoro */
void snd_fx_digital_test(void);    /* Teste de audio digital PCM (SB16 / Chime) */
void snd_play_melody(int id);      /* 0 = Elus Theme, 1 = Ode a Alegria, 2 = Fanfarra */
void snd_play_piano_key(int key);  /* 0..7 (Do, Re, Mi, Fa, Sol, La, Si, Do) */

#endif
