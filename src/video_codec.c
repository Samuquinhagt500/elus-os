/* ============================================================
 *  Elus OS - Codec de v�deo caseiro (.elusvid)
 *  Implementa��o: RLE simples sobre frames ARGB 32-bit.
 * ============================================================ */
#include "video_codec.h"
#include <string.h>

/* ---------- helpers de leitura little-endian ---------- */
static u16 rd_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void wr_u16(u8 *p, u16 v) { p[0] = (u8)(v & 0xFF); p[1] = (u8)((v >> 8) & 0xFF); }
static void wr_u32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

/* ---------- decoder ---------- */
int elusvid_open(struct elusvid_decoder *dec, const u8 *data, u32 size) {
    if (!dec || !data || size < 32) return 0;
    dec->file_data = data;
    dec->file_size = size;
    dec->opened = 0;
    dec->current_frame = 0;
    dec->last_frame_ms = 0;

    struct elusvid_header *h = (struct elusvid_header *)data;
    if (h->magic != ELUSVID_MAGIC) return 0;
    if (h->version != ELUSVID_VERSION) return 0;
    if (h->width == 0 || h->height == 0) return 0;
    if (h->total_frames == 0) return 0;
    if (h->data_offset >= size) return 0;

    dec->hdr = *h;
    dec->opened = 1;
    return 1;
}

u32 elusvid_total_frames(struct elusvid_decoder *dec) {
    return dec->opened ? dec->hdr.total_frames : 0;
}
u32 elusvid_current_frame(struct elusvid_decoder *dec) {
    return dec->opened ? dec->current_frame : 0;
}
u16 elusvid_width(struct elusvid_decoder *dec) {
    return dec->opened ? dec->hdr.width : 0;
}
u16 elusvid_height(struct elusvid_decoder *dec) {
    return dec->opened ? dec->hdr.height : 0;
}

/* Decodifica RLE de um frame para pixels ARGB */
static int elusvid_decode_rle(const u8 *data, u32 len, u16 w, u16 h, u32 *out) {
    u32 pos = 0;
    u32 px = 0;
    u32 total = (u32)w * (u32)h;

    while (pos < len && px < total) {
        u8 flag = data[pos++];
        if (flag & 0x80) {
            /* Run: (flag & 0x7F) + 1 pixels iguais */
            if (pos + 4 > len) return 0;
            u32 color = rd_u32(data + pos);
            pos += 4;
            u32 run = (u32)(flag & 0x7F) + 1;
            if (px + run > total) run = total - px;
            for (u32 i = 0; i < run; i++) out[px++] = color;
        } else {
            /* Raw: (flag + 1) pixels individuais */
            u32 raw = (u32)flag + 1;
            if (pos + raw * 4 > len) return 0;
            for (u32 i = 0; i < raw; i++) {
                out[px++] = rd_u32(data + pos);
                pos += 4;
            }
        }
    }
    return (px == total) ? 1 : 0;
}

int elusvid_decode_frame(struct elusvid_decoder *dec, struct elusvid_frame *frame) {
    if (!dec->opened || !frame) return 0;

    u32 offset = dec->hdr.data_offset + dec->current_frame * 16;
    if (offset + 16 > dec->file_size) return 0;

    struct elusvid_frame_header *fh = (struct elusvid_frame_header *)(dec->file_data + offset);
    u32 fsize = fh->frame_size;
    u32 fdata_off = offset + 16;
    if (fdata_off + fsize > dec->file_size) return 0;

    const u8 *fdata = dec->file_data + fdata_off;
    u32 total = (u32)dec->hdr.width * (u32)dec->hdr.height;

    if (frame->pixels == 0) return 0;  /* chamador deve alocar */

    if (dec->hdr.flags == ELUSVID_COMP_RLE) {
        if (!elusvid_decode_rle(fdata, fsize, dec->hdr.width, dec->hdr.height, frame->pixels))
            return 0;
    } else {
        /* Sem compressao: copia direto (ARGB bruto) */
        if (fsize < total * 4) return 0;
        for (u32 i = 0; i < total; i++) frame->pixels[i] = rd_u32(fdata + i * 4);
    }

    frame->width = dec->hdr.width;
    frame->height = dec->hdr.height;
    return 1;
}

int elusvid_next_frame(struct elusvid_decoder *dec) {
    if (!dec->opened) return 0;
    dec->current_frame++;
    if (dec->current_frame >= dec->hdr.total_frames) {
        dec->current_frame = 0;  /* loop */
    }
    return 1;
}

void elusvid_free_frame(struct elusvid_frame *frame) {
    /* No kernel freestanding, o chamador gerencia a mem�ria.
       Esta funcao so zera os campos para seguranca. */
    if (frame) {
        frame->width = 0;
        frame->height = 0;
        frame->pixels = 0;
    }
}

/* ---------- encoder ---------- */
int elusvid_encode_rle(const u32 *pixels, u16 w, u16 h, u8 *out, u32 out_cap, u32 *out_len) {
    if (!pixels || !out || !out_len) return 0;
    u32 total = (u32)w * (u32)h;
    u32 pos = 0;
    u32 px = 0;

    while (px < total) {
        u32 color = pixels[px];
        u32 run = 1;
        while (px + run < total && pixels[px + run] == color && run < 127) run++;

        if (run >= 3 && pos + 5 <= out_cap) {
            /* escreve run */
            out[pos++] = (u8)(0x80 | ((run - 1) & 0x7F));
            wr_u32(out + pos, color);
            pos += 4;
        } else {
            /* escreve raw ate mudar a cor ou encher bloco de 127 */
            u8 raw_start = pos;
            pos++; /* espaco para o flag */
            u32 raw_count = 0;
            while (px < total && raw_count < 127 && pos + 4 <= out_cap) {
                /* verifica se o proximo inicia um run de 3+ */
                u32 next_color = pixels[px];
                u32 next_run = 1;
                while (px + next_run < total && pixels[px + next_run] == next_color && next_run < 127) next_run++;
                if (next_run >= 3) break; /* deixa para o proximo bloco de run */
                out[pos++] = (u8)(next_color & 0xFF);
                out[pos++] = (u8)((next_color >> 8) & 0xFF);
                out[pos++] = (u8)((next_color >> 16) & 0xFF);
                out[pos++] = (u8)((next_color >> 24) & 0xFF);
                px++;
                raw_count++;
            }
            out[raw_start] = (u8)(raw_count - 1);
        }
    }

    *out_len = pos;
    return 1;
}

int elusvid_build_file(const struct elusvid_frame *frames, u32 frame_count,
                       u16 w, u16 h, u16 fps, u8 *out, u32 out_cap, u32 *out_len) {
    if (!frames || !out || !out_len || frame_count == 0) return 0;

    u32 pos = 0;

    /* cabecalho */
    if (pos + 32 > out_cap) return 0;
    wr_u32(out + pos, ELUSVID_MAGIC); pos += 4;
    wr_u32(out + pos, ELUSVID_VERSION); pos += 4;
    wr_u16(out + pos, w); pos += 2;
    wr_u16(out + pos, h); pos += 2;
    wr_u16(out + pos, fps); pos += 2;
    wr_u16(out + pos, ELUSVID_COMP_RLE); pos += 2;
    wr_u32(out + pos, frame_count); pos += 4;
    wr_u32(out + pos, 0); pos += 4; /* data_offset preenchido depois */
    for (int i = 0; i < 4; i++) { wr_u32(out + pos, 0); pos += 4; }

    u32 data_offset = pos;

    /* cabecalhos de frames + dados */
    for (u32 i = 0; i < frame_count; i++) {
        const struct elusvid_frame *f = &frames[i];
        u32 frame_data_start = pos + 16;

        /* espaco para cabecalho do frame */
        if (pos + 16 > out_cap) return 0;
        pos += 16;

        /* comprime o frame */
        u8 *comp_buf = out + pos;
        u32 comp_cap = out_cap - pos;
        u32 comp_len = 0;
        if (!elusvid_encode_rle(f->pixels, f->width, f->height, comp_buf, comp_cap, &comp_len))
            return 0;

        pos += comp_len;

        /* preenche cabecalho do frame (volta 16 bytes) */
        u8 *fh = out + frame_data_start - 16;
        wr_u32(fh, comp_len);           /* frame_size */
        wr_u32(fh + 4, i);              /* frame_index */
        wr_u32(fh + 8, 1000 / fps);     /* duration_ms aproximado */
        wr_u32(fh + 12, 0);             /* reserved */
    }

    /* ajusta data_offset no cabecalho principal */
    wr_u32(out + 28, data_offset);

    *out_len = pos;
    return 1;
}
