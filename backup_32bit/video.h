#ifndef VIDEO_H
#define VIDEO_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* Modo de vídeo ativo (setado pelo stage2 via VESA; preenchido em video_init) */
extern u16 video_w;
extern u16 video_h;
extern u8  video_bpp;
extern u32 *video_fb;      /* framebuffer real (VESA linear) */
extern u32 *video_bb;      /* backbuffer para desenho com dupla buffer */

/* Inicializa o driver de vídeo (chamado uma vez no kmain) */
void video_init(u32 *fb, u32 *bb, u16 w, u16 h, u8 bpp);
/* Preenche a tabela de cantos arredondados (32x32) para desenho com borda arredondada */
void video_set_corners(const u8 *corners);

/* ---------- primitivas de desenho ---------- */
void video_put_pixel(int x, int y, u32 color);
u32  video_get_pixel(int x, int y);
void video_rect(int x, int y, int w, int h, u32 color);
void video_rect_alpha(int x, int y, int w, int h, u32 color, u32 alpha);
/* alpha: 0..256 (0 = nao mistura, 256 = substitui totalmente) */
void video_fill_rr(int x, int y, int w, int h, int r, u32 color, u32 alpha);
void video_fill_rr_gradient(int x, int y, int w, int h, int r,
                            u32 c1, u32 c2, u32 alpha);
void video_blit(int dx, int dy, int sw, int sh,
                const u32 *src, int src_pitch_pixels);
/* copia uma região do backbuffer para o framebuffer real */
void video_copy_to_fb(int x, int y, int w, int h);

/* ---------- informacoes ---------- */
int  video_width(void);
int  video_height(void);
int  video_width(void);
int  video_height(void);
u32 *video_framebuffer(void);   /* ponteiro bruto para o framebuffer */
u32 *video_backbuffer(void);    /* ponteiro bruto para o backbuffer */

/* ---------- decodador de imagens ---------- */
/* Decodifica BMP nao comprimido (BI_RGB, 24 ou 32 bits) de um buffer na memoria.
 * Retorna 1 se sucesso; escreve largura/altura em out_w/out_h e pixels ARGB em out_pixels
 * (buffer de saida ja alocado pelo chamador, com pelo menos w*h u32).
 * Os pixels do BMP estao em BGR e de baixo para cima; sao convertidos para ARGB. */
int video_decode_bmp(const u8 *data, u32 len, u32 *out_w, u32 *out_h, u32 *out_pixels);

/* Desenha uma imagem BMP ja decodificada na posicao (x,y) do backbuffer.
 * Se os pixels nao couberem, recorta. */
void video_draw_bmp(int x, int y, const u32 *pixels, u32 w, u32 h);

#endif
