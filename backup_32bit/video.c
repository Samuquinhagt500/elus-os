/* ============================================================
 *  Elus OS - Driver de VÍDEO (VESA linear framebuffer)
 *  Acesso direto ao framebuffer de vídeo em modo protegido.
 *  Sem aceleracao por hardware (nao ha drivers de GPU neste SO).
 *  Todas as operacoes sao por software no backbuffer, com copia
 *  para o framebuffer real quando necessario.
 * ============================================================ */
#include "video.h"

u16 video_w = 1024;
u16 video_h = 768;
u8  video_bpp = 32;
u32 *video_fb = 0;
u32 *video_bb = 0;

/* mistura d com s; a = 0..256 */
static inline u32 mix(u32 d, u32 s, u32 a) {
    if (a == 0) return d;
    if (a >= 256) return s;
    u32 rb = (((d & 0xFF00FF) * (256 - a) + (s & 0xFF00FF) * a) >> 8) & 0xFF00FF;
    u32 g  = (((d & 0x00FF00) * (256 - a) + (s & 0x00FF00) * a) >> 8) & 0x00FF00;
    return rb | g;
}

static inline int valid_xy(int x, int y) {
    return (unsigned)x < (unsigned)video_w && (unsigned)y < (unsigned)video_h;
}

void video_init(u32 *fb, u32 *bb, u16 w, u16 h, u8 bpp) {
    video_fb = fb;
    video_bb = bb;
    video_w = w;
    video_h = h;
    video_bpp = bpp;
    if (!video_fb) video_fb = (u32 *)0x700000;   /* fallback */
    if (!video_bb) video_bb = (u32 *)0x400000;   /* fallback */
}

void video_put_pixel(int x, int y, u32 color) {
    if (valid_xy(x, y)) video_bb[y * video_w + x] = color;
}

u32 video_get_pixel(int x, int y) {
    if (valid_xy(x, y)) return video_bb[y * video_w + x];
    return 0;
}

void video_rect(int x, int y, int w, int h, u32 color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)video_w) w = (int)video_w - x;
    if (y + h > (int)video_h) h = (int)video_h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u32 *d = video_bb + (y + j) * video_w + x;
        for (int i = 0; i < w; i++) d[i] = color;
    }
}

void video_rect_alpha(int x, int y, int w, int h, u32 color, u32 alpha) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)video_w) w = (int)video_w - x;
    if (y + h > (int)video_h) h = (int)video_h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u32 *d = video_bb + (y + j) * video_w + x;
        if (alpha >= 256) {
            for (int i = 0; i < w; i++) d[i] = color;
        } else {
            for (int i = 0; i < w; i++) d[i] = mix(d[i], color, alpha);
        }
    }
}

/* tabela de cantos arredondados (32x32), preenchida pelo kernel via video_set_corners */
static u8 video_corner_tab[32][32];
static int video_has_corners = 0;

void video_set_corners(const u8 *corners) {
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++)
            video_corner_tab[i][j] = corners[i * 32 + j];
    video_has_corners = 1;
}

static int clamp_r(int r, int w, int h) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > 32) r = 32;
    if (r < 0) r = 0;
    return r;
}

static int rrc(int i, int j, int w, int h, int r, const u8 *t) {
    if (r <= 0) return 255;
    int ci = i < r ? i : (i >= w - r ? w - 1 - i : -1);
    int cj = j < r ? j : (j >= h - r ? h - 1 - j : -1);
    if (ci < 0 || cj < 0) return 255;
    return t[cj * 32 + ci];
}

void video_fill_rr(int x, int y, int w, int h, int r, u32 color, u32 alpha) {
    r = clamp_r(r, w, h);
    const u8 *t = r > 0 ? (const u8 *)video_corner_tab : 0;
    if (!video_has_corners && r > 0) {
        video_rect_alpha(x, y, w, h, color, alpha);
        return;
    }
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)video_h) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)video_w) continue;
            int c = rrc(i, j, w, h, r, t);
            if (!c) continue;
            u32 aa = (alpha * (u32)c) >> 8;
            if (c == 255 && alpha >= 256) aa = 256;
            video_bb[py * video_w + px] = mix(video_bb[py * video_w + px], color, aa);
        }
    }
}

void video_fill_rr_gradient(int x, int y, int w, int h, int r,
                            u32 c1, u32 c2, u32 alpha) {
    r = clamp_r(r, w, h);
    const u8 *t = r > 0 ? (const u8 *)video_corner_tab : 0;
    if (!video_has_corners && r > 0) {
        /* fallback: degrade vertical sem arredondamento */
        if (h > 1) {
            for (int j = 0; j < h; j++) {
                u32 col = 0;
                for (int s = 0; s <= 16; s += 8) {
                    int ca = (c1 >> s) & 255;
                    int cb = (c2 >> s) & 255;
                    int v = (ca * (h - 1 - j) + cb * j) / (h - 1);
                    col |= (u32)v << s;
                }
                video_rect_alpha(x, y + j, w, 1, col, alpha);
            }
        } else {
            video_rect_alpha(x, y, w, h, c1, alpha);
        }
        return;
    }
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)video_h) continue;
        u32 col = 0;
        if (h > 1) {
            for (int s = 0; s <= 16; s += 8) {
                int ca = (c1 >> s) & 255;
                int cb = (c2 >> s) & 255;
                int v = (ca * (h - 1 - j) + cb * j) / (h - 1);
                col |= (u32)v << s;
            }
        } else {
            col = c1;
        }
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)video_w) continue;
            int c = rrc(i, j, w, h, r, t);
            if (!c) continue;
            u32 aa = (alpha * (u32)c) >> 8;
            if (c == 255 && alpha >= 256) aa = 256;
            video_bb[py * video_w + px] = mix(video_bb[py * video_w + px], col, aa);
        }
    }
}

void video_blit(int dx, int dy, int sw, int sh,
                const u32 *src, int src_pitch_pixels) {
    if (dx < 0) { sw += dx; dx = 0; }
    if (dy < 0) { sh += dy; dy = 0; }
    if (dx + sw > (int)video_w) sw = (int)video_w - dx;
    if (dy + sh > (int)video_h) sh = (int)video_h - dy;
    if (sw <= 0 || sh <= 0) return;
    for (int j = 0; j < sh; j++) {
        u32 *d = video_bb + (dy + j) * video_w + dx;
        const u32 *s = src + j * src_pitch_pixels;
        for (int i = 0; i < sw; i++) d[i] = s[i];
    }
}

void video_copy_to_fb(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)video_w) w = (int)video_w - x;
    if (y + h > (int)video_h) h = (int)video_h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u32 *d = video_fb + (y + j) * video_w + x;
        const u32 *s = video_bb + (y + j) * video_w + x;
        for (int i = 0; i < w; i++) d[i] = s[i];
    }
}

int video_width(void)  { return (int)video_w; }
int video_height(void) { return (int)video_h; }
u32 *video_framebuffer(void) { return video_fb; }
u32 *video_backbuffer(void)  { return video_bb; }

/* ---------- decodador de imagens BMP ---------- */
static u16 bmp_read_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 bmp_read_u32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

int video_decode_bmp(const u8 *data, u32 len, u32 *out_w, u32 *out_h, u32 *out_pixels) {
    if (!data || !out_w || !out_h || !out_pixels || len < 54) return 0;
    if (data[0] != 'B' || data[1] != 'M') return 0;   /* assinatura BM */

    u32 off_bits = bmp_read_u32(data + 10);      /* offset dos pixels */
    u32 header_size = bmp_read_u32(data + 14);   /* tamanho do DIB header */
    if (header_size < 40) return 0;              /* BITMAPINFOHEADER minimo */

    u32 w = bmp_read_u32(data + 18);
    u32 h = bmp_read_u32(data + 22);
    u16 planes = bmp_read_u16(data + 26);
    u16 bit_count = bmp_read_u16(data + 28);
    u32 compression = bmp_read_u32(data + 30);

    if (planes != 1) return 0;
    if (compression != 0) return 0;             /* so BI_RGB (nao comprimido) */
    if (bit_count != 24 && bit_count != 32) return 0;
    if (w == 0 || h == 0) return 0;
    if (off_bits > len) return 0;

    *out_w = w;
    *out_h = h;

    /* BMP armazena linhas de baixo para cima; cada linha alinhada a 4 bytes */
    u32 row_bytes = ((w * bit_count + 31) / 32) * 4;
    const u8 *row_src = data + off_bits + (h - 1) * row_bytes;  /* ultima linha primeiro */

    u32 *dst = out_pixels;
    for (u32 y = 0; y < h; y++) {
        const u8 *row = row_src - y * row_bytes;
        for (u32 x = 0; x < w; x++) {
            u8 b = row[x * 3];
            u8 g = row[x * 3 + 1];
            u8 r = row[x * 3 + 2];
            u8 a = (bit_count == 32) ? row[x * 3 + 3] : 0xFF;
            *dst++ = (u32)((u32)a << 24 | (u32)r << 16 | (u32)g << 8 | (u32)b);
        }
    }
    return 1;
}

void video_draw_bmp(int x, int y, const u32 *pixels, u32 w, u32 h) {
    if (!pixels || w == 0 || h == 0) return;
    int dw = (int)w, dh = (int)h;
    if (x < 0) { dw += x; x = 0; }
    if (y < 0) { dh += y; y = 0; }
    if (x + dw > (int)video_w) dw = (int)video_w - x;
    if (y + dh > (int)video_h) dh = (int)video_h - y;
    if (dw <= 0 || dh <= 0) return;

    for (int j = 0; j < dh; j++) {
        u32 *d = video_bb + (y + j) * video_w + x;
        const u32 *s = pixels + (u32)j * w;
        for (int i = 0; i < dw; i++) {
            u32 c = s[i];
            u32 a = (c >> 24) & 0xFF;
            if (a >= 255) d[i] = c & 0xFFFFFF;
            else if (a > 0) d[i] = mix(d[i], c & 0xFFFFFF, (u32)a);
        }
    }
}
