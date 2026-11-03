#ifndef VIDEO_CODEC_H
#define VIDEO_CODEC_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed long long s64;

/* ============================================================
 *  Elus OS - Codec de v�deo caseiro (.elusvid)
 *  Formato simples: cabe�alho + seq�ncia de frames RLE.
 *  N�o depende de bibliotecas externas.
 * ============================================================ */

/* Magia do arquivo: "ELUSVID" */
#define ELUSVID_MAGIC   0x44495653  /* "ELVS" em little-endian */
#define ELUSVID_VERSION 1

/* Flags de compress�o */
#define ELUSVID_COMP_NONE  0
#define ELUSVID_COMP_RLE   1

/* Cabe�alho do arquivo (32 bytes) */
struct elusvid_header {
    u32  magic;           /* ELUSVID_MAGIC */
    u32  version;         /* ELUSVID_VERSION */
    u16  width;           /* largura do frame em pixels */
    u16  height;          /* altura do frame em pixels */
    u16  fps;             /* frames por segundo */
    u16  flags;           /* flags de compress�o (ELUSVID_COMP_*) */
    u32  total_frames;    /* n�mero total de frames */
    u32  data_offset;     /* offset para o primeiro frame */
    u32  reserved[4];
};

/* Cabe�alho de cada frame (16 bytes) */
struct elusvid_frame_header {
    u32  frame_size;      /* tamanho dos dados comprimidos deste frame (bytes) */
    u32  frame_index;     /* numero sequencial do frame (0-based) */
    u32  duration_ms;     /* tempo de exibi��o deste frame em ms */
    u32  reserved;
};

/* Estrutura de um frame decodificado (ARGB 32-bit) */
struct elusvid_frame {
    u16 width;
    u16 height;
    u32 *pixels;          /* buffer ARGB, largura*altura u32 */
};

/* Contexto do decoder */
struct elusvid_decoder {
    const u8 *file_data;   /* ponteiro para o arquivo inteiro em mem�ria */
    u32  file_size;
    int  opened;
    struct elusvid_header hdr;
    u32  current_frame;
    u32  last_frame_ms;
};

/* Inicializa o decoder com um buffer de arquivo */
int elusvid_open(struct elusvid_decoder *dec, const u8 *data, u32 size);

/* Decodifica o frame atual para a estrutura frame */
int elusvid_decode_frame(struct elusvid_decoder *dec, struct elusvid_frame *frame);

/* Avan�a para o pr�ximo frame */
int elusvid_next_frame(struct elusvid_decoder *dec);

/* Retorna o total de frames */
u32 elusvid_total_frames(struct elusvid_decoder *dec);

/* Retorna o frame atual */
u32 elusvid_current_frame(struct elusvid_decoder *dec);

/* Retorna largura/altura */
u16 elusvid_width(struct elusvid_decoder *dec);
u16 elusvid_height(struct elusvid_decoder *dec);

/* Libera o buffer de um frame decodificado */
void elusvid_free_frame(struct elusvid_frame *frame);

/* Encoder: comprime um buffer ARGB para RLE */
int elusvid_encode_rle(const u32 *pixels, u16 w, u16 h, u8 *out, u32 out_cap, u32 *out_len);

/* Encoder: monta um arquivo .elusvid completo em mem�ria */
int elusvid_build_file(const struct elusvid_frame *frames, u32 frame_count,
                       u16 w, u16 h, u16 fps, u8 *out, u32 out_cap, u32 *out_len);

#endif
