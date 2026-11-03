/* ============================================================
 *  Elus OS - kernel + interface "Elus Glass" (v0.4)
 *  Vidro liquido sutil: desfoque, translucidez, borda de luz,
 *  sombras suaves, dock flutuante, texto suavizado e proporcional.
 * ============================================================ */
#include "sound.h"
#include "net.h"
#include "video.h"

void kmain(void) __asm__("kmain");

struct bootinfo { u32 fb; u16 pitch; u16 w; u16 h; u8 bpp; } __attribute__((packed));
#define BI ((volatile struct bootinfo *)0x600)

/* mapa de memoria (acima de 1 MB, tudo livre no QEMU com 128 MB) */
#define WALL ((u32 *)0x700000)      /* papel de parede pre-renderizado */
#define TMPA ((u32 *)0x1000000)     /* buffers temporarios do desfoque (16 MB) */
#define TMPB ((u32 *)0x1400000)     /* (20 MB, cada um com 4 MB dedicados) */
#define GCOV ((u8 *)0xF00000)       /* [256][16*8] cobertura suavizada das letras */
#define BIGC ((u8 *)0xF08000)       /* [11][24*48] digitos grandes do relogio */
#define CTAB ((u8 *)0xF10000)       /* [33][32*32] cantos arredondados */

static u32 *fb, *bb;                /* framebuffer real e backbuffer */
static int SW, SH, pitch32;
static u8 fontbuf[4096];            /* fonte 8x16 binaria (BIOS + glifos novos) */
static u8 gadv[256], ginkw[256];
static s8 gdx[256];

/* ---------- estado global (declarado antes de qualquer uso) ---------- */
static volatile u32 ticks;                                /* contador do PIT (IRQ0) */
static int lay;                                             /* 0 = PT-BR, 1 = US */
static struct { int wall, accent, theme, opac, blur, widget; } cfg = { 0, 0, 0, 50, 1, 1 };
#define NWP 6
static int gfast;  /* 1 durante arrasto: pula sombra e blur para fluidez maxima */
static int clip_on, clip_y0, clip_y1;   /* recorte vertical usado pelo navegador */

static inline u8 inb(u16 p) { u8 r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static void io_wait(void) { outb(0x80, 0); }

void *memcpy(void *d, const void *s, unsigned n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}
void *memset(void *d, int c, unsigned n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

/* ---------- texto UTF-8 -> indice de glifo ---------- */
static u32 utf8_next(const char **ps) {
    const u8 *s = (const u8 *)*ps;
    u32 c = s[0];
    if (c < 0x80) { *ps += 1; return c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { *ps += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    *ps += 1; return '?';
}

/* codepoint Latin-1 -> glifo (os marcados "novo" sao desenhados em font_init) */
static const struct { u16 cp; u8 g; } gmap[] = {
    {0xC0, 1}, {0xC1, 2}, {0xC2, 3}, {0xC3, 4}, {0xC7, 0x80}, {0xC9, 0x90}, {0xCA, 5},
    {0xCD, 6}, {0xD3, 7}, {0xD4, 16}, {0xD5, 17}, {0xDA, 18}, {0xDC, 0x9A}, {0xD1, 0xA5},
    {0xE0, 0x85}, {0xE1, 0xA0}, {0xE2, 0x83}, {0xE3, 14}, {0xE7, 0x87}, {0xE9, 0x82},
    {0xEA, 0x88}, {0xED, 0xA1}, {0xF1, 0xA4}, {0xF3, 0xA2}, {0xF4, 0x93}, {0xF5, 15},
    {0xFA, 0xA3}, {0xFC, 0x81}, {0xE4, 0x84}, {0xEB, 0x89}, {0xEF, 0x8B}, {0xF6, 0x94},
    {0xC4, 0x8E}, {0xD6, 0x99}, {0xE8, 0x8A}, {0xEC, 0x8D}, {0xF2, 0x95}, {0xF9, 0x97},
    {0xAA, 0xA6}, {0xBA, 0xA7}, {0xB0, 0xF8}, {0xB7, 0xFA}, {0xBD, 0xAB},
};
static int glyph_of(u32 cp) {
    if (cp < 0x80) return (int)cp;
    for (unsigned i = 0; i < sizeof gmap / sizeof gmap[0]; i++) if (gmap[i].cp == cp) return gmap[i].g;
    return '?';
}

/* acentos desenhados sobre letras da fonte da BIOS */
static const u8 ACC[5][2] = {
    {0x18, 0x30},   /* agudo     */
    {0x30, 0x18},   /* grave     */
    {0x18, 0x66},   /* circunflexo */
    {0x76, 0xDC},   /* til       */
    {0x66, 0x66},   /* trema     */
};
enum { A_ACUTE, A_GRAVE, A_CIRC, A_TILDE, A_DIAER };
static int inside(int x, int y, int w, int h, int px, int py) {
    return px >= x && py >= y && px < x + w && py < y + h;
}
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static u32 isqrt(u32 n) {
    u32 r = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) { if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

/* ============================================================
 *  Primitivas de desenho com alpha
 * ============================================================ */
/* mistura d com s; a = 0..256 */
static inline u32 mix(u32 d, u32 s, u32 a) {
    if (a == 0) return d;
    if (a >= 256) return s;
    u32 rb = (((d & 0xFF00FF) * (256 - a) + (s & 0xFF00FF) * a) >> 8) & 0xFF00FF;
    u32 g  = (((d & 0x00FF00) * (256 - a) + (s & 0x00FF00) * a) >> 8) & 0x00FF00;
    return rb | g;
}
static u32 lerp(u32 a, u32 b, int i, int n) {
    if (n < 2) return a;
    u32 out = 0;
    for (int s = 0; s <= 16; s += 8) {
        int ca = (a >> s) & 255, cb = (b >> s) & 255;
        out |= (u32)((ca * (n - 1 - i) + cb * i) / (n - 1)) << s;
    }
    return out;
}
static inline void bpx(int x, int y, u32 c, u32 a) {
    if (clip_on && (y < clip_y0 || y >= clip_y1)) return;
    if ((unsigned)x < (unsigned)SW && (unsigned)y < (unsigned)SH) bb[y * SW + x] = mix(bb[y * SW + x], c, a);
}

static void rect_a(int x, int y, int w, int h, u32 c, u32 a) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u32 *d = bb + (y + j) * SW + x;
        if (a >= 256) { int n = w; __asm__ volatile("rep stosl" : "+D"(d), "+c"(n) : "a"(c) : "memory"); }
        else for (int i = 0; i < w; i++) d[i] = mix(d[i], c, a);
    }
}
static void rect(int x, int y, int w, int h, u32 c) { rect_a(x, y, w, h, c, 256); }

static void copy_to_fb(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        u32 *s = bb + (y + j) * SW + x;
        u32 *d = fb + (y + j) * pitch32 + x;
        int n = w;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    }
}

/* ---- cantos arredondados com antialias ---- */
static u8 cready[33];
static const u8 *corner(int r) {
    u8 *t = CTAB + r * 1024;
    if (!cready[r]) {
        for (int j = 0; j < r; j++)
            for (int i = 0; i < r; i++) {
                int dx = (2 * r - 2 * i - 1) * 8, dy = (2 * r - 2 * j - 1) * 8;
                int d = (int)isqrt((u32)(dx * dx + dy * dy));
                int c = r * 16 - d + 8;
                if (c < 0) c = 0;
                if (c > 16) c = 16;
                t[j * r + i] = (u8)(c * 255 / 16);
            }
        cready[r] = 1;
    }
    return t;
}
static inline int rrc(int i, int j, int w, int h, int r, const u8 *t) {
    if (!t) return 255;
    int ci = i < r ? i : (i >= w - r ? w - 1 - i : -1);
    int cj = j < r ? j : (j >= h - r ? h - 1 - j : -1);
    if (ci < 0 || cj < 0) return 255;
    return t[cj * r + ci];
}
static int clampr(int r, int w, int h) { r = imin(r, imin(w, h) / 2); return r > 32 ? 32 : r; }

static void fill_rr(int x, int y, int w, int h, int r, u32 col, u32 a) {
    r = clampr(r, w, h);
    const u8 *t = r > 0 ? corner(r) : 0;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)SH) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)SW) continue;
            int c = rrc(i, j, w, h, r, t);
            if (!c) continue;
            u32 aa = (a * (u32)c) >> 8;
            if (c == 255 && a >= 256) aa = 256;
            bb[py * SW + px] = mix(bb[py * SW + px], col, aa);
        }
    }
}
static void fill_rr_v(int x, int y, int w, int h, int r, u32 c1, u32 c2, u32 a) {
    r = clampr(r, w, h);
    const u8 *t = r > 0 ? corner(r) : 0;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)SH) continue;
        u32 col = lerp(c1, c2, j, h);
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)SW) continue;
            int c = rrc(i, j, w, h, r, t);
            if (!c) continue;
            u32 aa = (a * (u32)c) >> 8;
            if (c == 255 && a >= 256) aa = 256;
            bb[py * SW + px] = mix(bb[py * SW + px], col, aa);
        }
    }
}
static void fill_circle(int cx, int cy, int r, u32 col, u32 a) { fill_rr(cx - r, cy - r, 2 * r, 2 * r, r, col, a); }

/* ---- borda de luz (rim): mais forte no canto superior esquerdo ---- */
static void rim(int x, int y, int w, int h, int r, int a_tl, int a_br) {
    r = clampr(r, w, h);
    const u8 *to = r > 0 ? corner(r) : 0;
    const u8 *ti = r > 1 ? corner(r - 1) : 0;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)SH) continue;
        int full = (j < r + 1 || j >= h - r - 1);
        for (int i = 0; i < w; i++) {
            if (!full && i >= 2 && i < w - 2) { i = w - 3; continue; }
            int px = x + i;
            if ((unsigned)px >= (unsigned)SW) continue;
            int c0 = rrc(i, j, w, h, r, to);
            int c1 = (i >= 1 && j >= 1 && i < w - 1 && j < h - 1) ? rrc(i - 1, j - 1, w - 2, h - 2, r > 1 ? r - 1 : 0, ti) : 0;
            int band = c0 - c1;
            if (band <= 0) continue;
            int t = i * 128 / w + j * 128 / h;
            int a = a_tl + (a_br - a_tl) * t / 255;
            a = a * band / 255;
            bpx(px, py, 0xFFFFFF, (u32)a);
        }
    }
}

/* ---- sombra suave fora da forma ---- */
static void shadow(int x, int y, int w, int h, int r, int dy, int S, int amax) {
    if (gfast) return; /* Durante arrasto, pula calculo de sombra para manter 60 FPS fluido */
    r = clampr(r, w, h);
    const u8 *st = r > 0 ? corner(r) : 0;
    int S2 = S * S;
    if (S2 <= 0) return;
    u32 inv_S2 = (1u << 20) / (u32)S2;
    int ix0 = x + r, ix1 = x + w - r, iy0 = y + dy + r, iy1 = y + dy + h - r;
    for (int pj = y - S + dy; pj < y + h + S + dy; pj++) {
        if ((unsigned)pj >= (unsigned)SH) continue;
        for (int pi = x - S; pi < x + w + S; pi++) {
            if ((unsigned)pi >= (unsigned)SW) continue;
            if (pi >= x && pi < x + w && pj >= y && pj < y + h) {      /* dentro do retangulo: so os cantos importam */
                int ii = pi - x, jj = pj - y;
                if (jj >= r && jj < h - r) { pi = x + w - 1; continue; }
                if (ii >= r && ii < w - r) { pi = x + w - r - 1; continue; }
                if (rrc(ii, jj, w, h, r, st) >= 250) continue;
            }
            int dxm = pi < ix0 ? ix0 - pi : (pi >= ix1 ? pi - ix1 + 1 : 0);
            int dym = pj < iy0 ? iy0 - pj : (pj >= iy1 ? pj - iy1 + 1 : 0);
            int d = imax(dxm, dym) + imin(dxm, dym) * 3 / 8 - r;
            if (d >= S) continue;
            if (d < 0) d = 0;
            int k = S - d;
            u32 a = ((u32)(amax * k * k) * inv_S2) >> 20;
            bb[pj * SW + pi] = mix(bb[pj * SW + pi], 0x000000, a);
        }
    }
}

/* ---- desfoque (caixa, raio fixo) otimizado com multiplicacao inversa ---- */
#define BR 6
static void hblur(const u32 *s, u32 *d, int n) {
    int rs = 0, gs = 0, bs = 0;
    for (int k = -BR; k <= BR; k++) {
        u32 p = s[k < 0 ? 0 : (k >= n ? n - 1 : k)];
        rs += (p >> 16) & 255; gs += (p >> 8) & 255; bs += p & 255;
    }
    for (int i = 0; i < n; i++) {
        d[i] = (((u32)((rs * 5042) >> 16)) << 16) | (((u32)((gs * 5042) >> 16)) << 8) | ((u32)((bs * 5042) >> 16));
        u32 a = s[i + BR + 1 >= n ? n - 1 : i + BR + 1], b = s[i - BR < 0 ? 0 : i - BR];
        rs += (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
        gs += (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
        bs += (int)(a & 255) - (int)(b & 255);
    }
}
static void vblur(const u32 *s, u32 *d, int w, int h) {
    for (int x = 0; x < w; x++) {
        int rs = 0, gs = 0, bs = 0;
        for (int k = -BR; k <= BR; k++) {
            u32 p = s[(k < 0 ? 0 : (k >= h ? h - 1 : k)) * w + x];
            rs += (p >> 16) & 255; gs += (p >> 8) & 255; bs += p & 255;
        }
        for (int y = 0; y < h; y++) {
            d[y * w + x] = (((u32)((rs * 5042) >> 16)) << 16) | (((u32)((gs * 5042) >> 16)) << 8) | ((u32)((bs * 5042) >> 16));
            int ia = y + BR + 1; if (ia >= h) ia = h - 1;
            int ib = y - BR;     if (ib < 0) ib = 0;
            u32 a = s[ia * w + x], b = s[ib * w + x];
            rs += (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
            gs += (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
            bs += (int)(a & 255) - (int)(b & 255);
        }
    }
}

/* ---- painel de vidro: desfoca o que esta atras, tinge, clareia o topo ---- */
static void glass(int x, int y, int w, int h, int r, u32 tint, int ta, int grad, int blur) {
    r = clampr(r, w, h);
    const u8 *t = r > 0 ? corner(r) : 0;
    int x0 = imax(x, 0), y0 = imax(y, 0), x1 = imin(x + w, SW), y1 = imin(y + h, SH);
    int cw = x1 - x0, ch = y1 - y0;
    if (cw <= 0 || ch <= 0) return;
    if (blur) {
        for (int j = 0; j < ch; j++) hblur(bb + (y0 + j) * SW + x0, TMPA + j * cw, cw);
        vblur(TMPA, TMPB, cw, ch);
    }
    u32 inv_h = h > 0 ? ((1u << 16) / (u32)h) : 0;
    for (int j = 0; j < ch; j++) {
        int jj = y0 + j - y;
        u32 grad_factor = ((u32)(grad * (h - jj)) * inv_h) >> 16;
        for (int i = 0; i < cw; i++) {
            int ii = x0 + i - x;
            int c = rrc(ii, jj, w, h, r, t);
            if (!c) continue;
            u32 *p = bb + (y0 + j) * SW + x0 + i;
            u32 base = blur ? TMPB[j * cw + i] : *p;
            base = mix(base, tint, (u32)ta);
            base = mix(base, 0xFFFFFF, grad_factor);
            int ed = imin(imin(ii, w - 1 - ii), imin(jj, h - 1 - jj));
            if (ed < 6) base = mix(base, 0xFFFFFF, (u32)((6 - ed) * 4));
            *p = (c == 255) ? base : mix(*p, base, (u32)(c + (c >> 7)));
        }
    }
}

/* ============================================================
 *  Fonte: suavizacao (Scale2x), metricas proporcionais, digitos grandes
 * ============================================================ */
static void scale2x(const u8 *s, int w, int h, u8 *d) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u8 P = s[y * w + x];
            u8 A = y ? s[(y - 1) * w + x] : 0;
            u8 B = x < w - 1 ? s[y * w + x + 1] : 0;
            u8 C = x ? s[y * w + x - 1] : 0;
            u8 D = y < h - 1 ? s[(y + 1) * w + x] : 0;
            u8 e0 = (C == A && C != D && A != B) ? A : P;
            u8 e1 = (A == B && A != C && B != D) ? B : P;
            u8 e2 = (D == C && D != B && C != A) ? C : P;
            u8 e3 = (B == D && B != A && D != C) ? D : P;
            d[(2 * y) * 2 * w + 2 * x] = e0;     d[(2 * y) * 2 * w + 2 * x + 1] = e1;
            d[(2 * y + 1) * 2 * w + 2 * x] = e2; d[(2 * y + 1) * 2 * w + 2 * x + 1] = e3;
        }
}
static void mkglyph(int slot, char base, int acc, int row0) {
    u8 *d = fontbuf + slot * 16;
    memcpy(d, fontbuf + (u8)base * 16, 16);
    d[row0] |= ACC[acc][0];
    d[row0 + 1] |= ACC[acc][1];
}
static void font_init(void) {
    memcpy(fontbuf, (const void *)0x30000, 4096);
    mkglyph(1, 'A', A_GRAVE, 0);  mkglyph(2, 'A', A_ACUTE, 0);
    mkglyph(3, 'A', A_CIRC, 0);   mkglyph(4, 'A', A_TILDE, 0);
    mkglyph(5, 'E', A_CIRC, 0);   mkglyph(6, 'I', A_ACUTE, 0);
    mkglyph(7, 'O', A_ACUTE, 0);  mkglyph(16, 'O', A_CIRC, 0);
    mkglyph(17, 'O', A_TILDE, 0); mkglyph(18, 'U', A_ACUTE, 0);
    mkglyph(0x90, 'E', A_ACUTE, 0);
    mkglyph(14, 'a', A_TILDE, 2); mkglyph(15, 'o', A_TILDE, 2);

    u8 bits[128], s1[512], s2[2048];
    for (int g = 0; g < 256; g++) {
        int minc = 8, maxc = -1;
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 8; c++) {
                int on = (fontbuf[g * 16 + r] >> (7 - c)) & 1;
                bits[r * 8 + c] = (u8)on;
                if (on) { if (c < minc) minc = c; if (c > maxc) maxc = c; }
            }
        scale2x(bits, 8, 16, s1);                                   /* 16x32 suavizado */
        u8 *cv = GCOV + g * 128;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++)
                cv[y * 8 + x] = (u8)((s1[(2 * y) * 16 + 2 * x] + s1[(2 * y) * 16 + 2 * x + 1] +
                                      s1[(2 * y + 1) * 16 + 2 * x] + s1[(2 * y + 1) * 16 + 2 * x + 1]) * 255 / 4);
        int ink = maxc < 0 ? 0 : maxc - minc + 1;
        int adv = (maxc < 0) ? 5 : ink + 2;
        if (g >= '0' && g <= '9') adv = 7;
        ginkw[g] = (u8)ink; gadv[g] = (u8)adv;
        gdx[g] = (s8)(maxc < 0 ? 0 : (adv - ink) / 2 - minc);
        if (g >= '0' && g <= '9') {                                  /* digitos grandes 24x48 */
            scale2x(s1, 16, 32, s2);                                 /* 32x64 */
            u8 *bg = BIGC + (g - '0') * 1152;
            for (int y = 0; y < 48; y++)
                for (int x = 0; x < 24; x++) {
                    int sy0 = y * 4 / 3, sy1 = ((y + 1) * 4 + 2) / 3, sx0 = x * 4 / 3, sx1 = ((x + 1) * 4 + 2) / 3;
                    if (sy1 > 64) sy1 = 64;
                    if (sx1 > 32) sx1 = 32;
                    int sum = 0, n = 0;
                    for (int yy = sy0; yy < sy1; yy++) for (int xx = sx0; xx < sx1; xx++) { sum += s2[yy * 32 + xx]; n++; }
                    bg[y * 24 + x] = (u8)(n ? sum * 255 / n : 0);
                }
        } else if (g == ':') {
            scale2x(s1, 16, 32, s2);
            u8 *bg = BIGC + 10 * 1152;
            for (int y = 0; y < 48; y++)
                for (int x = 0; x < 24; x++) {
                    int sy0 = y * 4 / 3, sy1 = ((y + 1) * 4 + 2) / 3, sx0 = x * 4 / 3, sx1 = ((x + 1) * 4 + 2) / 3;
                    if (sy1 > 64) sy1 = 64;
                    if (sx1 > 32) sx1 = 32;
                    int sum = 0, n = 0;
                    for (int yy = sy0; yy < sy1; yy++) for (int xx = sx0; xx < sx1; xx++) { sum += s2[yy * 32 + xx]; n++; }
                    bg[y * 24 + x] = (u8)(n ? sum * 255 / n : 0);
                }
        }
    }
    /* suaviza os digitos grandes (filtro 1-2-1 nos dois eixos) */
    for (int idx = 0; idx < 11; idx++) {
        u8 *c = BIGC + idx * 1152, tmp[1152];
        for (int y = 0; y < 48; y++)
            for (int x = 0; x < 24; x++) {
                int l = x ? c[y * 24 + x - 1] : 0, m = c[y * 24 + x], r = x < 23 ? c[y * 24 + x + 1] : 0;
                tmp[y * 24 + x] = (u8)((l + 2 * m + r) / 4);
            }
        for (int y = 0; y < 48; y++)
            for (int x = 0; x < 24; x++) {
                int u = y ? tmp[(y - 1) * 24 + x] : 0, m = tmp[y * 24 + x], d = y < 47 ? tmp[(y + 1) * 24 + x] : 0;
                int v = (u + 2 * m + d) / 4 * 5 / 4;
                c[y * 24 + x] = (u8)(v > 255 ? 255 : v);
            }
    }
}

/* ---------- texto ---------- */
static void glyph_a(int x, int y, int g, u32 col, u32 al) {
    const u8 *c = GCOV + g * 128;
    x += gdx[g];
    for (int r = 0; r < 16; r++)
        for (int k = 0; k < 8; k++) {
            u32 v = c[r * 8 + k];
            if (!v) continue;
            u32 a = (v * al) >> 8;
            if (a) bpx(x + k, y + r, col, a);
        }
}
static void text_a(int x, int y, const char *s, u32 col, u32 al) {
    while (*s) { int g = glyph_of(utf8_next(&s)); glyph_a(x, y, g, col, al); x += gadv[g]; }
}
static void text(int x, int y, const char *s, u32 col) { text_a(x, y, s, col, 256); }
static void text_b(int x, int y, const char *s, u32 col, u32 al) { text_a(x, y, s, col, al); text_a(x + 1, y, s, col, al); }
static int twidth(const char *s) { int n = 0; while (*s) n += gadv[glyph_of(utf8_next(&s))]; return n; }
static void text_c(int cx, int y, const char *s, u32 col, u32 al) { text_a(cx - twidth(s) / 2, y, s, col, al); }

static void itoa2(int v, char *o) { o[0] = '0' + (v / 10) % 10; o[1] = '0' + v % 10; o[2] = 0; }
static int utoa(int v, char *o) {
    char t[12]; int n = 0, k = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = '0' + v % 10; v /= 10; }
    while (n > 0) o[k++] = t[--n];
    o[k] = 0; return k;
}
static int scat(char *o, const char *s) { int n = 0; while (s[n]) { o[n] = s[n]; n++; } o[n] = 0; return n; }
static void hex32(u32 v, char *o) {
    o[0] = '0'; o[1] = 'x';
    for (int i = 0; i < 8; i++) o[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    o[10] = 0;
}

/* ============================================================
 *  CPU: GDT, IDT, PIC, PIT
 * ============================================================ */
struct regs { u32 edi, esi, ebp, esp_, ebx, edx, ecx, eax, vec, err, eip, cs, eflags; };
void isr_handler(struct regs *r) __asm__("isr_handler");
extern u32 isr_stub_table[48] __asm__("isr_stub_table");

static struct { u16 lo, base_lo; u8 mid, access, gran, hi; } __attribute__((packed)) gdt[3];
static struct { u16 limit; u32 base; } __attribute__((packed)) gdtp, idtp;
static struct { u16 lo, sel; u8 zero, flags; u16 hi; } __attribute__((packed)) idt[256];

static void gdt_init(void) {
    gdt[1].lo = 0xFFFF; gdt[1].access = 0x9A; gdt[1].gran = 0xCF;
    gdt[2].lo = 0xFFFF; gdt[2].access = 0x92; gdt[2].gran = 0xCF;
    gdtp.limit = sizeof gdt - 1; gdtp.base = (u32)gdt;
    __asm__ volatile("lgdt %0\n\t"
                     "ljmp $0x08, $1f\n"
                     "1:\n\t"
                     "mov $0x10, %%ax\n\t"
                     "mov %%ax, %%ds\n\tmov %%ax, %%es\n\tmov %%ax, %%fs\n\t"
                     "mov %%ax, %%gs\n\tmov %%ax, %%ss"
                     :: "m"(gdtp) : "eax", "memory");
}
static void idt_init(void) {
    for (int i = 0; i < 48; i++) {
        u32 a = isr_stub_table[i];
        idt[i].lo = a & 0xFFFF; idt[i].sel = 0x08; idt[i].zero = 0;
        idt[i].flags = 0x8E; idt[i].hi = a >> 16;
    }
    idtp.limit = sizeof idt - 1; idtp.base = (u32)idt;
    __asm__ volatile("lidt %0" :: "m"(idtp));
}
static void pic_init(void) {
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();   /* IRQ0-7 -> 32.., IRQ8-15 -> 40.. */
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xD8);   /* libera IRQ0 (timer), IRQ1 (teclado), IRQ2 (cascata) e IRQ5 (SB16) */
    outb(0xA1, 0xE7);   /* libera IRQ11 (RTL8139) e IRQ12 (mouse) na slave PIC */
}
static void pit_init(void) {            /* 100 Hz */
    outb(0x43, 0x36);
    outb(0x40, 11931 & 255); outb(0x40, 11931 >> 8);
}

/* ---------- filas de entrada preenchidas pelas IRQs ---------- */
#define QN 256
static volatile u8 mq[QN], kq[QN];
static volatile u32 mqh, mqt, kqh, kqt;

static void kbc_drain(void) {
    for (;;) {
        u8 st = inb(0x64);
        if (!(st & 1)) break;
        u8 d = inb(0x60);
        if (st & 0x20) { mq[mqh & (QN - 1)] = d; mqh++; }
        else           { kq[kqh & (QN - 1)] = d; kqh++; }
    }
}

/* ---------- tela azul ---------- */
static const char *exc_name[20] = {
    "Divisão por zero", "Depuração", "Interrupção NMI", "Breakpoint", "Overflow",
    "Limite excedido", "Instrução inválida", "Coprocessador indisponível", "Falha dupla",
    "Segmento do coprocessador", "TSS inválido", "Segmento ausente", "Falha de pilha",
    "Proteção geral", "Falha de página", "Reservada", "Erro de ponto flutuante",
    "Alinhamento", "Machine check", "SIMD",
};
static void reboot(void) {
    for (int t = 0; t < 100000 && (inb(0x64) & 2); t++) {}
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile("hlt");
}
static void panic(struct regs *r) {
    char b[16];
    __asm__ volatile("cli");
    rect(0, 0, SW, SH, 0x0000AA);
    rect(SW / 2 - 100, 120, 200, 20, 0xC0C0C0);
    text_c(SW / 2, 122, "Elus OS", 0x000080, 256);
    text(80, 180, "Ocorreu um erro fatal e o sistema foi interrompido.", 0xFFFFFF);
    text(80, 220, "Exceção:", 0xFFFFFF);
    text(80 + 80, 220, r->vec < 20 ? exc_name[r->vec] : "Desconhecida", 0xFFFFFF);
    text(80, 244, "Vetor:", 0xFFFFFF);       hex32(r->vec, b); text(80 + 80, 244, b, 0xFFFFFF);
    text(80, 260, "EIP:", 0xFFFFFF);        hex32(r->eip, b); text(80 + 80, 260, b, 0xFFFFFF);
    text(80, 276, "Erro:", 0xFFFFFF);       hex32(r->err, b); text(80 + 80, 276, b, 0xFFFFFF);
    text(80, 320, "Pressione qualquer tecla para reiniciar o computador.", 0xFFFFFF);
    copy_to_fb(0, 0, SW, SH);
    for (;;) {                                  /* espera uma tecla (polling) */
        u8 st = inb(0x64);
        if (st & 1) { u8 d = inb(0x60); if (!(st & 0x20) && !(d & 0x80)) reboot(); }
    }
}

void isr_handler(struct regs *r) {
    u32 v = r->vec;
    if (v < 32) panic(r);
    if (v == 32) {
        ticks++;
        snd_tick();
        net_tick();
    }
    else if (v == 33 || v == 44) kbc_drain();
    else if (v == 37) snd_irq_handler();
    else if (v == 43) net_irq_handler();   /* IRQ11 = RTL8139 */
    if (v >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}


/* ============================================================
 *  Relogio e data (CMOS)
 * ============================================================ */
static int clk_h, clk_m, clk_d, clk_mo, clk_y;
static u8 cmos(u8 r) { outb(0x70, r & 0x7F); return inb(0x71); }
static void cmosw(u8 r, u8 v) { outb(0x70, r & 0x7F); outb(0x71, v); }
static int rtc_busy(void) { outb(0x70, 0x0A); return inb(0x71) & 0x80; }
static int bcd(u8 v, u8 b) { return (b & 4) ? v : (v & 15) + (v >> 4) * 10; }
static int read_clock(void) {
    for (int t = 0; t < 1000 && rtc_busy(); t++) io_wait();
    u8 b = cmos(0x0B);
    int h = bcd(cmos(4) & 0x7F, b), m = bcd(cmos(2), b), d = bcd(cmos(7), b), mo = bcd(cmos(8), b), y = 2000 + bcd(cmos(9), b);
    int ch = (h != clk_h || m != clk_m || d != clk_d);
    clk_h = h; clk_m = m; clk_d = d; clk_mo = mo; clk_y = y;
    return ch;
}

/* ============================================================
 *  Configuracoes persistentes (CMOS)
 * ============================================================ */
static void cfg_save(void) {
    /* Usa CMOS registros 0x30-0x3F (livres) */
    cmosw(0x30, 0xE5);        /* magic */
    cmosw(0x31, (u8)cfg.wall);
    cmosw(0x32, (u8)cfg.accent);
    cmosw(0x33, (u8)cfg.theme);
    cmosw(0x34, (u8)cfg.opac);
    cmosw(0x35, (u8)cfg.blur);
    cmosw(0x36, (u8)cfg.widget);
    cmosw(0x37, (u8)lay);
    cmosw(0x38, (u8)snd_get_volume());
    cmosw(0x39, (u8)snd_is_muted());
}
static void cfg_load(void) {
    if (cmos(0x30) == 0xE5) {
        cfg.wall = cmos(0x31) % NWP;
        cfg.accent = cmos(0x32) % 6;
        cfg.theme = cmos(0x33) & 1;
        cfg.opac = cmos(0x34);
        if (cfg.opac > 100) cfg.opac = 50;
        cfg.blur = cmos(0x35) & 1;
        cfg.widget = cmos(0x36) & 1;
        lay = cmos(0x37) & 1;
        int vol = cmos(0x38);
        if (vol > 100) vol = 80;
        snd_set_volume(vol);
        snd_set_mute(cmos(0x39) & 1);
    }
}
static const char *wd_name[7] = { "domingo", "segunda-feira", "terça-feira", "quarta-feira", "quinta-feira", "sexta-feira", "sábado" };
static const char *mo_name[12] = { "janeiro", "fevereiro", "março", "abril", "maio", "junho", "julho", "agosto", "setembro", "outubro", "novembro", "dezembro" };
static int weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

/* ============================================================
 *  Papel de parede (6 predefinicoes geradas por codigo)
 * ============================================================ */
typedef struct { int x, y, r; u32 col; int k; } Blob;
typedef struct { const char *name; u32 top, bot; Blob b[5]; } WPreset;
#define NWP 6
#define THUMB_W 140
#define THUMB_H 68
#define THUMB(p) ((u32 *)(0xE00000 + (p) * 0x10000))
static const WPreset WP[NWP] = {
    { "Aurora", 0x0D0F2E, 0x2A1760, { { 830, 150, 560, 0xE23D7A, 175 }, { 170, 640, 600, 0x2E7BFF, 185 },
        { 520, 330, 440, 0x8A4DFF, 150 }, { 930, 720, 420, 0x16C7C2, 140 }, { 120, 110, 320, 0xFF9A3D, 125 } } },
    { "Oceano", 0x04152B, 0x0A3355, { { 200, 600, 620, 0x14B8A6, 170 }, { 760, 250, 560, 0x2563EB, 170 },
        { 900, 700, 420, 0x22D3EE, 120 }, { 480, 420, 380, 0x4F46E5, 120 }, { 120, 120, 300, 0x6EE7B7, 90 } } },
    { "Pôr do sol", 0x1B0B2E, 0x3A1030, { { 850, 650, 620, 0xFF7A3D, 185 }, { 250, 200, 560, 0xF43F8E, 170 },
        { 600, 380, 420, 0x7C3AED, 140 }, { 950, 120, 300, 0xFBBF24, 110 }, { 100, 700, 360, 0xEF4444, 110 } } },
    { "Floresta", 0x06140F, 0x0D2A1E, { { 180, 200, 580, 0x22C55E, 150 }, { 800, 640, 600, 0x14B8A6, 150 },
        { 900, 150, 320, 0xA3E635, 90 }, { 500, 450, 380, 0x0EA5E9, 100 }, { 100, 700, 360, 0x10B981, 110 } } },
    { "Grafite", 0x0F0F12, 0x1C1C22, { { 800, 200, 560, 0x5B6B8C, 120 }, { 200, 650, 600, 0x3B4A63, 130 },
        { 500, 350, 380, 0x8B8FA8, 80 }, { 950, 720, 400, 0x2F3E57, 110 }, { 120, 120, 300, 0x6B7280, 70 } } },
    { "Lavanda", 0x1A1030, 0x30174F, { { 800, 160, 560, 0xF0ABFC, 150 }, { 180, 620, 600, 0xA78BFA, 160 },
        { 520, 360, 420, 0xF9A8D4, 130 }, { 940, 720, 420, 0x93C5FD, 120 }, { 120, 120, 300, 0xFDBA74, 90 } } },
};
static void wall_render(u32 *dst, int dw, int dh, int p) {
    const WPreset *w = &WP[p];
    for (int y = 0; y < dh; y++) {
        u32 c = lerp(w->top, w->bot, y, dh);
        for (int x = 0; x < dw; x++) dst[y * dw + x] = c;
    }
    for (int b = 0; b < 5; b++) {
        int cx = w->b[b].x * dw / 1024, cy = w->b[b].y * dh / 768, R = w->b[b].r * dw / 1024;
        if (R < 2) continue;
        u32 R2 = (u32)R * R, inv = (256u << 16) / R2, col = w->b[b].col;
        int k = w->b[b].k;
        int y0 = imax(0, cy - R), y1 = imin(dh, cy + R), x0 = imax(0, cx - R), x1 = imin(dw, cx + R);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                int dx = x - cx, dy = y - cy;
                u32 d2 = (u32)(dx * dx + dy * dy);
                if (d2 >= R2) continue;
                u32 wt = ((R2 - d2) * inv) >> 16;
                wt = (wt * wt) >> 8;
                wt = (wt * (u32)k) >> 8;
                u32 p0 = dst[y * dw + x];
                u32 r = ((p0 >> 16) & 255) + ((((col >> 16) & 255) * wt) >> 8);
                u32 g = ((p0 >> 8) & 255) + ((((col >> 8) & 255) * wt) >> 8);
                u32 bl = (p0 & 255) + (((col & 255) * wt) >> 8);
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (bl > 255) bl = 255;
                dst[y * dw + x] = (r << 16) | (g << 8) | bl;
            }
    }
}
static void wall_init(int p) { wall_render(WALL, SW, SH, p); }
static void thumbs_init(void) { for (int p = 0; p < NWP; p++) wall_render(THUMB(p), THUMB_W, THUMB_H, p); }
static void blit_rr(int x, int y, int w, int h, int r, const u32 *src) {
    r = clampr(r, w, h);
    const u8 *t = r > 0 ? corner(r) : 0;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)SH) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)SW) continue;
            int c = rrc(i, j, w, h, r, t);
            if (!c) continue;
            bb[py * SW + px] = (c >= 255) ? src[j * w + i] : mix(bb[py * SW + px], src[j * w + i], (u32)(c + (c >> 7)));
        }
    }
}

/* ============================================================
 *  Tema e configuracoes (editaveis no app Personalizar)
 * ============================================================ */
static const u32 ACCENTS[6] = { 0x0A84FF, 0x8B5CF6, 0xEC4899, 0xF97316, 0x22C55E, 0x14B8A6 };
#define GL_TINT (cfg.theme ? 0xF2F2F8u : 0x14142Au)
#define TXT     (cfg.theme ? 0x1C1C22u : 0xF5F5F7u)
#define TXT2    (cfg.theme ? 0x55555Fu : 0xA9A9B8u)
#define OVC     (cfg.theme ? 0x000000u : 0xFFFFFFu)         /* cor de sobreposicao (cartoes, hover) */
#define OVA(a)  ((u32)(cfg.theme ? (a) * 2 / 3 : (a)))
#define ACCENT  (ACCENTS[cfg.accent])
#define BLUR    (!gfast && cfg.blur)
#define R_WIN   20
static int ta_of(int o) { int a = o + cfg.opac + (cfg.theme ? 30 : 0); return a < 0 ? 0 : (a > 250 ? 250 : a); }
static u32 acc_hi(void) { return mix(ACCENT, 0xFFFFFF, 70); }

/* ---------- icones (quadrados arredondados com degrade) ---------- */
static const u32 icg[14][2] = {
    {0x8B7BFF, 0x3B2FD9}, {0x58B0FF, 0x1D5BDB}, {0xFFDD55, 0xFF9A0A}, {0x9A9AA6, 0x4E4E5A}, {0x62E283, 0x1FA84C},
    {0xFF7A70, 0xD93A33}, {0xB4B4C0, 0x6A6A78}, {0x6C8BFF, 0x2B3DB5}, {0xC4C4D0, 0x84849A}, {0x7A7A88, 0x42424E},
    {0xFF9A6B, 0xE0407A}, {0x38BDF8, 0x0284C7}, {0x34D399, 0x059669}, {0x60A5FA, 0x1D4ED8},
};
static void draw_icon(int k, int x, int y, int S) {
#define Q(v) ((S) * (v) / 48)
    int r = S * 11 / 48;
    fill_rr_v(x, y, S, S, r, icg[k][0], icg[k][1], 256);
    switch (k) {
    case 0:
        for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++)
            fill_rr(x + Q(11) + a * Q(14), y + Q(11) + b * Q(14), Q(11), Q(11), Q(4), 0xFFFFFF, 235);
        break;
    case 1:
        fill_rr(x + Q(8), y + Q(9), Q(32), Q(23), Q(4), 0xFFFFFF, 240);
        fill_rr_v(x + Q(11), y + Q(12), Q(26), Q(17), Q(2), 0x4C9BFF, 0x1B4FC8, 256);
        rect_a(x + Q(20), y + Q(32), Q(8), Q(4), 0xFFFFFF, 220);
        fill_rr(x + Q(14), y + Q(36), Q(20), Q(3), Q(1), 0xFFFFFF, 230);
        break;
    case 2:
        fill_rr(x + Q(11), y + Q(8), Q(26), Q(32), Q(4), 0xFFFFFF, 245);
        for (int i = 0; i < 4; i++) rect_a(x + Q(15), y + Q(15 + i * 6), Q(18), imax(1, Q(2)), 0xC9B27A, 255);
        break;
    case 3:
        text_b(x + S / 2 - 3, y + S / 2 - 8, "i", 0xFFFFFF, 256);
        break;
    case 4:
        fill_circle(x + S / 2, y + S / 2, Q(13), 0xFFFFFF, 245);
        fill_circle(x + S / 2, y + S / 2, Q(8), 0x34BE5A, 256);
        rect_a(x + Q(24), y + Q(9), Q(10), Q(8), 0x34BE5A, 256);
        break;
    case 5:
        fill_circle(x + S / 2, y + S / 2 + Q(1), Q(13), 0xFFFFFF, 245);
        fill_circle(x + S / 2, y + S / 2 + Q(1), Q(8), 0xE0443E, 256);
        rect_a(x + Q(21), y + Q(8), Q(6), Q(16), 0xE0443E, 256);
        rect_a(x + Q(22), y + Q(9), Q(4), Q(15), 0xFFFFFF, 255);
        break;
    case 6:
        fill_rr(x + Q(8), y + Q(17), Q(32), Q(15), Q(4), 0xFFFFFF, 235);
        rect_a(x + Q(12), y + Q(25), Q(14), imax(1, Q(2)), 0x9090A0, 255);
        fill_circle(x + Q(34), y + Q(25), imax(2, Q(2)), 0x34C759, 256);
        break;
    case 7:
        fill_rr(x + Q(12), y + Q(8), Q(24), Q(12), Q(2), 0xFFFFFF, 230);
        fill_rr(x + Q(9), y + Q(24), Q(30), Q(16), Q(3), 0xFFFFFF, 245);
        break;
    case 8:
        fill_circle(x + S / 2, y + S / 2, Q(16), 0xFFFFFF, 235);
        fill_circle(x + S / 2, y + S / 2, Q(5), 0x8E8EA0, 256);
        break;
    case 9:
        for (int i = 0; i < 3; i++) {
            rect_a(x + Q(10), y + Q(14 + i * 10), Q(28), imax(1, Q(2)), 0xFFFFFF, 210);
            fill_circle(x + Q(15 + i * 9), y + Q(15 + i * 10), imax(2, Q(4)), 0xFFFFFF, 256);
        }
        break;
    case 10:
        fill_circle(x + Q(17), y + Q(17), Q(7), 0xFFFFFF, 245);
        fill_circle(x + Q(31), y + Q(17), Q(7), 0xFFFFFF, 205);
        fill_circle(x + Q(24), y + Q(31), Q(7), 0xFFFFFF, 165);
        break;
    case 11:
        /* Alto-falante */
        fill_rr(x + Q(10), y + Q(18), Q(9), Q(12), Q(2), 0xFFFFFF, 250);
        for (int i = 0; i < Q(10); i++) {
            int h2 = Q(6) + i * Q(8) / Q(10);
            rect_a(x + Q(19) + i, y + S / 2 - h2, 1, 2 * h2, 0xFFFFFF, 250);
        }
        fill_circle(x + Q(34), y + S / 2, Q(7), 0xFFFFFF, 140);
        fill_circle(x + Q(34), y + S / 2, Q(4), icg[k][1], 256);
        fill_circle(x + Q(40), y + S / 2, Q(12), 0xFFFFFF, 100);
        fill_circle(x + Q(40), y + S / 2, Q(9), icg[k][1], 256);
        break;
    case 12:
        /* Rede: globo estilizado com meridianos */
        fill_circle(x + S / 2, y + S / 2, Q(16), 0xFFFFFF, 230);
        fill_circle(x + S / 2, y + S / 2, Q(10), icg[k][1], 200);
        /* meridianos (linhas verticais curvas simuladas com rects) */
        rect_a(x + S / 2, y + S / 2 - Q(15), imax(1, Q(1)), Q(30), 0xFFFFFF, 220);
        rect_a(x + S / 2 - Q(15), y + S / 2, Q(30), imax(1, Q(1)), 0xFFFFFF, 220);
        /* arco elipse horizontal */
        for (int i = -Q(10); i <= Q(10); i++) {
            int yy = y + S / 2 + Q(8) * i / Q(10) / 5;
            rect_a(x + S / 2 + i - 1, yy, 2, imax(1, Q(1)), 0xFFFFFF, 200);
        }
        break;
    case 13:
        /* Navegador: janela com globo */
        fill_rr(x + Q(7), y + Q(8), Q(34), Q(32), Q(4), 0xFFFFFF, 240);
        rect_a(x + Q(7), y + Q(8), Q(34), Q(7), 0x1D4ED8, 235);
        fill_circle(x + Q(11), y + Q(11), imax(1, Q(2)), 0xFFFFFF, 250);
        fill_circle(x + Q(17), y + Q(11), imax(1, Q(2)), 0xFFFFFF, 220);
        fill_circle(x + Q(23), y + Q(11), imax(1, Q(2)), 0xFFFFFF, 190);
        fill_circle(x + Q(24), y + Q(27), Q(8), 0x3B82F6, 256);
        rect_a(x + Q(16), y + Q(27), Q(16), imax(1, Q(1)), 0xFFFFFF, 210);
        rect_a(x + Q(24), y + Q(19), imax(1, Q(1)), Q(16), 0xFFFFFF, 210);
        for (int i = -Q(5); i <= Q(5); i++) {
            int yy = y + Q(27) + Q(5) * i / Q(5) / 4;
            rect_a(x + Q(24) + i - 1, yy, 2, imax(1, Q(1)), 0xFFFFFF, 170);
        }
        break;
    }
    fill_rr(x + 1, y + 1, S - 2, S / 2, r - 1, 0xFFFFFF, 30);          /* brilho no topo */
    rim(x, y, S, S, r, 110, 30);
#undef Q
}

/* ============================================================
 *  Janelas e aplicativos: cada app e um modulo (draw/click/drag/key)
 * ============================================================ */
typedef struct { int x, y, w, h; const char *title; } Win;
typedef struct {
    const char *title; int icon, x, y, w, h;
    void (*draw)(int id, int x, int y, int w, int h);   /* desenha o conteudo (x,y = canto da janela) */
    int  (*click)(int id, int lx, int ly);              /* devolve 1 para capturar o mouse (arrastar) */
    void (*drag)(int id, int lx, int ly);
    void (*key)(int id, u8 g);                          /* g = indice de glifo, '\b', '\n'... */
} App;
#define NAPPS 7
static Win W[NAPPS];
static int zl[NAPPS], zn;
static u8 minz[NAPPS];
static int dirty = 1, blink, cap = -1;

static int is_open(int id) { for (int i = 0; i < zn; i++) if (zl[i] == id) return 1; return 0; }
static void win_front(int id) {
    int j = 0;
    for (int i = 0; i < zn; i++) if (zl[i] != id) zl[j++] = zl[i];
    zl[j++] = id; zn = j; minz[id] = 0;
}
static void win_close(int id) {
    int j = 0;
    for (int i = 0; i < zn; i++) if (zl[i] != id) zl[j++] = zl[i];
    zn = j; minz[id] = 0;
}
static int top_win(void) {
    for (int i = zn - 1; i >= 0; i--) if (!minz[zl[i]]) return zl[i];
    return -1;
}

/* ---------- app 0: Meu Computador ---------- */
static void computer_draw(int id, int x, int y, int ww, int hh) {
    static const char *nm[4] = { "Disco (C:)", "Disquete A:", "CD-ROM (D:)", "Controle" };
    static const char *sb[4] = { "Elus OS", "3½ pol.", "Vazio", "Painel" };
    static const int ik[4] = { 6, 7, 8, 9 };
    int cx = x + 14, cy = y + 46, tw = (ww - 42) / 2;
    (void)id;
    for (int k = 0; k < 4; k++) {
        int tx = cx + (k & 1) * (tw + 14), ty = cy + (k >> 1) * 94;
        fill_rr(tx, ty, tw, 82, 14, OVC, OVA(24));
        rim(tx, ty, tw, 82, 14, 70, 18);
        draw_icon(ik[k], tx + 14, ty + 17, 48);
        text_b(tx + 76, ty + 22, nm[k], TXT, 256);
        text(tx + 76, ty + 44, sb[k], TXT2);
    }
    text(cx + 4, y + hh - 28, "4 itens", TXT2);
}

/* ---------- app 1: Bloco de Notas ---------- */
static u8 note[1024]; static int nlen;
static void notes_draw(int id, int x, int y, int ww, int hh) {
    int px = x + 14, py = y + 46, pw = ww - 28, ph = hh - 60;
    (void)id;
    fill_rr(px, py, pw, ph, 12, cfg.theme ? 0xFFFFFF : 0x000000, cfg.theme ? 130 : 72);
    rim(px, py, pw, ph, 12, 60, 18);
    int ax = px + 12, ay = py + 10, maxw = pw - 24, maxl = (ph - 20) / 20, cx = 0, cy = 0;
    if (nlen == 0 && !blink) text(ax, ay + 1, "Digite aqui…", TXT2);
    for (int i = 0; i < nlen; i++) {
        u8 c = note[i];
        if (c == '\n') { cx = 0; cy++; continue; }
        int adv = gadv[c];
        if (cx + adv > maxw) { cx = 0; cy++; }
        if (cy >= maxl) break;
        glyph_a(ax + cx, ay + cy * 20 + 1, c, TXT, 256);
        cx += adv;
    }
    if (blink && cy < maxl) rect_a(ax + cx, ay + cy * 20 + 1, 2, 17, ACCENT, 256);
}
static void notes_key(int id, u8 g) {
    (void)id;
    if (g == '\b') { if (nlen) nlen--; else snd_fx_err(); }
    else if (g == '\n') { if (nlen < 1000) note[nlen++] = g; }
    else if (g >= 32) { if (nlen < 1000) note[nlen++] = g; else snd_fx_err(); }
}

/* ---------- app 2: Sobre ---------- */
static void about_draw(int id, int x, int y, int ww, int hh) {
    (void)id;
    draw_icon(0, x + (ww - 64) / 2, y + 52, 64);
    char b[40]; int n;
    text_c(x + ww / 2, y + 128, "Elus OS", TXT, 256);
    text_c(x + ww / 2 + 1, y + 128, "Elus OS", TXT, 256);
    text_c(x + ww / 2, y + 150, "Versão 0.4 · Liquid Glass", TXT, 230);
    text_c(x + ww / 2, y + 168, "Feito do zero em ASM + C", TXT2, 256);
    n = utoa(SW, b); b[n++] = 'x'; n += utoa(SH, b + n); b[n++] = 'x'; n += utoa(32, b + n);
    text_c(x + ww / 2, y + 186, b, TXT2, 256);
    text_c(x + ww / 2, y + 204, "F12 alterna o teclado PT/US", TXT2, 256);
    int bx = x + ww / 2 - 50, by = y + hh - 46;
    fill_rr_v(bx, by, 100, 30, 15, acc_hi(), ACCENT, 256);
    fill_rr(bx + 1, by + 1, 98, 14, 14, 0xFFFFFF, 34);
    text_c(bx + 50, by + 7, "OK", 0xFFFFFF, 256);
}
static int about_click(int id, int lx, int ly) {
    if (inside(W[id].w / 2 - 50, W[id].h - 46, 100, 30, lx, ly)) { snd_fx_ok(); win_close(id); }
    return 0;
}

/* ---------- app 3: Personalizar ---------- */
#define PZ_TX(p) (14 + ((p) % 3) * (THUMB_W + 16))
#define PZ_TY(p) (70 + ((p) / 3) * (THUMB_H + 12))
#define PZ_SWY   270
#define PZ_RY(i) (296 + (i) * 44)
static void seg(int x, int y, int w, int h, const char *a, const char *b, int sel) {
    int hw = w / 2;
    fill_rr(x, y, w, h, h / 2, OVC, OVA(34));
    fill_rr_v(x + (sel ? hw : 0) + 2, y + 2, hw - 4, h - 4, (h - 4) / 2, acc_hi(), ACCENT, 256);
    text_c(x + hw / 2, y + (h - 16) / 2, a, sel == 0 ? 0xFFFFFF : TXT, 256);
    text_c(x + hw + hw / 2, y + (h - 16) / 2, b, sel == 1 ? 0xFFFFFF : TXT, 256);
}
static void toggle(int x, int y, int on) {
    fill_rr(x, y, 52, 30, 15, on ? ACCENT : OVC, on ? 256 : OVA(60));
    fill_circle(x + (on ? 37 : 15), y + 16, 12, 0x000000, 40);
    fill_circle(x + (on ? 37 : 15), y + 15, 12, 0xFFFFFF, 256);
}
static void pers_draw(int id, int x, int y, int ww, int hh) {
    static const char *lb[5] = { "Tema", "Teclado", "Transparência do vidro", "Desfoque do vidro", "Widget de relógio" };
    (void)id; (void)ww; (void)hh;
    text(x + 18, y + 48, "Papel de parede", TXT2);
    for (int p = 0; p < NWP; p++) {
        int tx = x + PZ_TX(p), ty = y + PZ_TY(p);
        if (p == cfg.wall) fill_rr(tx - 3, ty - 3, THUMB_W + 6, THUMB_H + 6, 15, ACCENT, 256);
        blit_rr(tx, ty, THUMB_W, THUMB_H, 12, THUMB(p));
        text_a(tx + 10, ty + THUMB_H - 21, WP[p].name, 0x000000, 150);
        text(tx + 9, ty + THUMB_H - 22, WP[p].name, 0xFFFFFF);
    }
    text(x + 18, y + 232, "Cor de destaque", TXT2);
    for (int i = 0; i < 6; i++) {
        int cx = x + 28 + i * 38, cy = y + PZ_SWY;
        if (i == cfg.accent) fill_circle(cx, cy, 15, TXT, 230);
        fill_circle(cx, cy, 11, ACCENTS[i], 256);
    }
    fill_rr(x + 14, y + PZ_RY(0), 452, 220, 14, OVC, OVA(22));
    rim(x + 14, y + PZ_RY(0), 452, 220, 14, 60, 18);
    for (int i = 1; i < 5; i++) rect_a(x + 30, y + PZ_RY(i), 420, 1, OVC, OVA(34));
    for (int i = 0; i < 5; i++) text(x + 30, y + PZ_RY(i) + 14, lb[i], TXT);
    seg(x + 282, y + PZ_RY(0) + 7, 170, 30, "Escuro", "Claro", cfg.theme);
    seg(x + 282, y + PZ_RY(1) + 7, 170, 30, "PT-BR", "US", lay);
    int ty = y + PZ_RY(2) + 19, tx0 = x + 252;
    fill_rr(tx0, ty, 200, 6, 3, OVC, OVA(60));
    int fw = cfg.opac * 2; if (fw < 6) fw = 6;
    fill_rr(tx0, ty, fw, 6, 3, ACCENT, 256);
    int kx = tx0 + cfg.opac * 2;
    fill_circle(kx, ty + 4, 10, 0x000000, 40);
    fill_circle(kx, ty + 3, 10, 0xFFFFFF, 256);
    toggle(x + 400, y + PZ_RY(3) + 7, cfg.blur);
    toggle(x + 400, y + PZ_RY(4) + 7, cfg.widget);
}
static void set_opac(int lx) { int v = (lx - 252) / 2; cfg.opac = v < 0 ? 0 : (v > 100 ? 100 : v); }
static int pers_click(int id, int lx, int ly) {
    (void)id;
    for (int p = 0; p < NWP; p++)
        if (inside(PZ_TX(p), PZ_TY(p), THUMB_W, THUMB_H, lx, ly)) { if (cfg.wall != p) { cfg.wall = p; wall_init(p); } cfg_save(); return 0; }
    for (int i = 0; i < 6; i++) {
        int dx = lx - (28 + i * 38), dy = ly - PZ_SWY;
        if (dx * dx + dy * dy <= 16 * 16) { cfg.accent = i; cfg_save(); return 0; }
    }
    if (inside(282, PZ_RY(0) + 7, 170, 30, lx, ly)) { cfg.theme = lx >= 282 + 85; cfg_save(); }
    else if (inside(282, PZ_RY(1) + 7, 170, 30, lx, ly)) { lay = lx >= 282 + 85; cfg_save(); }
    else if (inside(240, PZ_RY(2), 226, 44, lx, ly)) { set_opac(lx); cfg_save(); return 1; }
    else if (inside(360, PZ_RY(3), 106, 44, lx, ly)) { cfg.blur ^= 1; cfg_save(); }
    else if (inside(360, PZ_RY(4), 106, 44, lx, ly)) { cfg.widget ^= 1; cfg_save(); }
    return 0;
}
static void pers_drag(int id, int lx, int ly) { (void)id; (void)ly; set_opac(lx); cfg_save(); }

/* ---------- app 4: Som & Audio ---------- */
static int last_piano_key = -1;
static int piano_key_ticks = 0;

static void audio_draw(int id, int x, int y, int ww, int hh) {
    (void)id; (void)ww; (void)hh;
    const struct sound_info *info = snd_get_info();

    /* Card 1: Informacoes de Hardware */
    fill_rr(x + 14, y + 46, 452, 68, 12, OVC, OVA(22));
    rim(x + 14, y + 46, 452, 68, 12, 65, 18);
    draw_icon(11, x + 24, y + 56, 48);
    text_b(x + 82, y + 54, snd_get_device_name(), TXT, 256);

    char dev_sub[64];
    if (info->dev_type == SND_DEV_SB16) {
        char mb[8], nb[8];
        utoa(info->sb_major, mb);
        utoa(info->sb_minor, nb);
        int n = 0;
        n += scat(dev_sub + n, "DSP v");
        n += scat(dev_sub + n, mb);
        dev_sub[n++] = '.';
        n += scat(dev_sub + n, nb);
        scat(dev_sub + n, " · Porta 0x220 · IRQ 5 · DMA 1/5");
    } else {
        scat(dev_sub, "Canal 2 · Temporizador 100 Hz · Sintetizador");
    }
    text(x + 82, y + 74, dev_sub, TXT2);

    /* Badge de status ativo */
    fill_circle(x + 86, y + 97, 4, 0x22C55E, 256);
    text(x + 96, y + 92, info->dev_type == SND_DEV_SB16 ? "Hardware Detectado e Operacional (DMA PCM)" : "Modo Compatibilidade Ativo (PC Speaker)", 0x22C55E);

    /* Card 2: Volume e Mudo */
    fill_rr(x + 14, y + 122, 452, 58, 12, OVC, OVA(22));
    rim(x + 14, y + 122, 452, 58, 12, 65, 18);
    text_b(x + 28, y + 134, "Volume", TXT, 256);

    /* Slider de volume */
    int tx0 = x + 96, ty = y + 147;
    fill_rr(tx0, ty, 200, 6, 3, OVC, OVA(60));
    int fw = (info->volume * 200) / 100;
    if (fw < 4) fw = 4;
    fill_rr(tx0, ty, fw, 6, 3, ACCENT, 256);
    int kx = tx0 + fw;
    fill_circle(kx, ty + 3, 9, 0x000000, 40);
    fill_circle(kx, ty + 3, 9, 0xFFFFFF, 256);

    char vb[16];
    int vn = utoa(info->volume, vb);
    vb[vn++] = '%'; vb[vn] = 0;
    text(x + 306, y + 141, vb, TXT2);

    text(x + 352, y + 141, "Mudo", TXT);
    toggle(x + 398, y + 136, info->mute);

    /* Card 3: Efeitos Sonoros */
    text(x + 18, y + 188, "Efeitos Sonoros do Sistema", TXT2);
    static const char *fx_names[6] = { "Início", "Sucesso", "Erro", "Moeda", "Laser", "Melodia" };
    for (int i = 0; i < 6; i++) {
        int bx = x + 14 + i * 76, by = y + 208;
        fill_rr(bx, by, 72, 30, 8, OVC, OVA(30));
        rim(bx, by, 72, 30, 8, 80, 20);
        text_c(bx + 36, by + 7, fx_names[i], TXT, 256);
    }

    /* Linha extra: Teste PCM e Hino */
    fill_rr(x + 14, y + 246, 222, 30, 8, OVC, OVA(30));
    rim(x + 14, y + 246, 222, 30, 8, 80, 20);
    text_c(x + 125, y + 253, info->dev_type == SND_DEV_SB16 ? "Teste PCM Digital (SB16)" : "Sino de Vidro (Speaker)", TXT, 256);

    fill_rr(x + 244, y + 246, 222, 30, 8, OVC, OVA(30));
    rim(x + 244, y + 246, 222, 30, 8, 80, 20);
    text_c(x + 355, y + 253, "Ode à Alegria (Beethoven)", TXT, 256);

    /* Card 4: Mini Piano / Sintetizador */
    text(x + 18, y + 288, "Mini Sintetizador (toque no piano ou use as teclas 1 a 8)", TXT2);
    static const char *p_notes[8] = { "Dó", "Ré", "Mi", "Fá", "Sol", "Lá", "Si", "Dó" };
    static const char *p_keys[8]  = { "1",  "2",  "3",  "4",  "5",   "6",  "7",  "8"   };

    for (int i = 0; i < 8; i++) {
        int px = x + 16 + i * 56, py = y + 308;
        int active = (last_piano_key == i && piano_key_ticks > 0);
        u32 key_col = active ? ACCENT : (cfg.theme ? 0xFFFFFF : 0x2A2A38);
        fill_rr_v(px, py, 52, 94, 8, active ? acc_hi() : key_col, key_col, 256);
        rim(px, py, 52, 94, 8, active ? 220 : 90, 30);
        text_c(px + 26, py + 48, p_notes[i], active ? 0xFFFFFF : TXT, 256);
        text_c(px + 26, py + 72, p_keys[i], active ? 0xFFFFFF : TXT2, 220);
    }
}

static void set_audio_vol(int lx) {
    int v = (lx - 96) * 100 / 200;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    snd_set_volume(v);
    cfg_save();
}

static int audio_click(int id, int lx, int ly) {
    (void)id;
    /* Slider de volume */
    if (inside(90, 134, 212, 30, lx, ly)) {
        set_audio_vol(lx);
        return 1; /* Captura mouse para arrasto */
    }
    /* Toggle de Mudo */
    if (inside(390, 132, 60, 34, lx, ly)) {
        snd_set_mute(!snd_is_muted());
        cfg_save();
        return 0;
    }
    /* Botoes de efeitos sonoros */
    if (ly >= 208 && ly <= 238) {
        int bi = (lx - 14) / 76;
        if (bi >= 0 && bi < 6 && lx <= 14 + bi * 76 + 72) {
            if (bi == 0) snd_fx_startup();
            else if (bi == 1) snd_fx_ok();
            else if (bi == 2) snd_fx_err();
            else if (bi == 3) snd_fx_coin();
            else if (bi == 4) snd_fx_laser();
            else if (bi == 5) snd_play_melody(0);
            return 0;
        }
    }
    /* Teste PCM e Ode a Alegria */
    if (ly >= 246 && ly <= 276) {
        if (lx >= 14 && lx <= 236) {
            snd_fx_digital_test();
            return 0;
        }
        if (lx >= 244 && lx <= 466) {
            snd_play_melody(1);
            return 0;
        }
    }
    /* Teclas do Piano */
    if (ly >= 308 && ly <= 402) {
        int k = (lx - 16) / 56;
        if (k >= 0 && k < 8 && lx <= 16 + k * 56 + 52) {
            snd_play_piano_key(k);
            last_piano_key = k;
            piano_key_ticks = 15;
            return 0;
        }
    }
    return 0;
}

static void audio_drag(int id, int lx, int ly) {
    (void)id; (void)ly;
    set_audio_vol(lx);
}

static void audio_key(int id, u8 g) {
    (void)id;
    if (g >= '1' && g <= '8') {
        int k = g - '1';
        snd_play_piano_key(k);
        last_piano_key = k;
        piano_key_ticks = 15;
        dirty = 1;
    }
}

/* ============================================================
 *  App 5: Rede (driver RTL8139)
 * ============================================================ */
static u32 net_ping_sent_tick = 0;
static int net_pinging = 0;
static int net_last_ping_ms = -1;
static int net_ping_rx_snap = 0; /* snapshot de pings_received no momento do envio */

/* Converte u32 para string decimal — retorna ponteiro para buf */
static char *u32str(u32 v, char *buf) {
    int i = 11;
    buf[i] = 0;
    if (v == 0) { buf[--i] = '0'; return buf + i; }
    while (v > 0) { buf[--i] = '0' + (int)(v % 10); v /= 10; }
    return buf + i;
}

/* Monta "Texto: valor" em dst, retorna dst */
static char *strcat2(char *dst, const char *a, const char *b) {
    char *p = dst;
    while (*a) *p++ = *a++;
    while (*b) *p++ = *b++;
    *p = 0;
    return dst;
}

static void net_draw(int id, int x, int y, int ww, int hh) {
    (void)id; (void)hh;
    const struct net_info *ni = net_get_info();
    int cx = x + 14, cy = y + 46;

    if (!ni->present) {
        /* Ícone de aviso */
        fill_circle(x + ww / 2, cy + 60, 34, 0xFF5F57, 160);
        rect_a(x + ww / 2 - 9, cy + 40, 18, 28, 0xFFFFFF, 240);
        rect_a(x + ww / 2 - 9, cy + 74, 18, 8,  0xFFFFFF, 240);
        text_c(x + ww / 2, cy + 108, "Adaptador de rede não detectado", TXT, 200);
        text_c(x + ww / 2, cy + 126, "Adicione RTL8139 à VM (-device rtl8139)", TXT, 140);
        return;
    }

    /* LED de status */
    int led_col = ni->link_up ? 0x34D399 : 0xFF5F57;
    fill_circle(cx + 14, cy + 12, 9, led_col, 230);
    fill_circle(cx + 14, cy + 12, 4, 0xFFFFFF, 200);
    const char *status = ni->link_up ? "Conectado — RTL8139" : "Sem sinal — RTL8139";
    text_b(cx + 30, cy + 6, status, TXT, 230);
    rect_a(cx, cy + 28, ww - 28, 1, OVC, OVA(28));

    /* Informações de rede */
    char mac[20], ipbuf[16], gwbuf[16], dnsbuf[16];
    net_format_mac(mac);
    net_format_ip(ni->ip,      ipbuf);
    net_format_ip(ni->gateway, gwbuf);
    net_format_ip(ni->dns,     dnsbuf);

    int row = cy + 38;
    char line[64];
    text_b(cx,      row,      "Endereço MAC:", TXT, 160);  text_b(cx + 120, row,      mac,    TXT, 240); row += 22;
    text_b(cx,      row,      "IP Local:",     TXT, 160);  text_b(cx + 120, row,      ipbuf,  TXT, 240); row += 22;
    text_b(cx,      row,      "Gateway:",      TXT, 160);  text_b(cx + 120, row,      gwbuf,  TXT, 240); row += 22;
    text_b(cx,      row,      "DNS:",          TXT, 160);  text_b(cx + 120, row,      dnsbuf, TXT, 240); row += 22;

    rect_a(cx, row, ww - 28, 1, OVC, OVA(24)); row += 10;

    /* Contadores TX/RX */
    char nb[12];
    strcat2(line, "Pacotes TX: ", u32str(ni->packets_tx, nb));
    text_b(cx, row, line, TXT, 180); row += 20;
    strcat2(line, "Pacotes RX: ", u32str(ni->packets_rx, nb));
    text_b(cx, row, line, TXT, 180); row += 20;

    rect_a(cx, row, ww - 28, 1, OVC, OVA(24)); row += 10;

    /* Botão Ping */
    int btn_col = net_pinging ? 0x6A6A78 : ACCENT;
    fill_rr(cx, row, 160, 32, 10, btn_col, 220);
    rim(cx, row, 160, 32, 10, 80, 20);
    text_c(cx + 80, row + 8, net_pinging ? "Aguardando..." : "Ping Gateway", 0xFFFFFF, 240);

    /* Resultado ping */
    if (net_pinging) {
        text_b(cx + 170, row + 10, "Enviando ping...", TXT, 180);
    } else if (net_last_ping_ms >= 0) {
        strcat2(line, "RTT: ", u32str((u32)net_last_ping_ms, nb));
        char *p = line;
        while (*p) p++;
        *p++ = ' '; *p++ = 'm'; *p++ = 's'; *p = 0;
        text_b(cx + 170, row + 10, line, TXT, 220);
    } else {
        text_b(cx + 170, row + 10, "—", TXT, 120);
    }
    (void)line;
}

static int net_click(int id, int lx, int ly) {
    (void)id;
    const struct net_info *ni = net_get_info();
    if (!ni->present || net_pinging) return 0;
    /* Área do botão Ping: replicar posição calculada em net_draw */
    /* row começa em cy+38 + 4*22 + 10 + 2*20 + 10 = cy+196 = y+46+196 = y+242 */
    /* botão: cx=14, row=y+242, w=160, h=32 */
    if (lx >= 14 && lx <= 174 && ly >= 242 && ly <= 274) {
        net_pinging = 1;
        net_ping_sent_tick = ticks;
        net_ping_rx_snap = net_get_info()->pings_received;
        net_ping(ni->gateway);
        dirty = 1;
    }
    return 0;
}

/* ============================================================
 *  App 6: Navegador Elus (HTTP + renderizador HTML proprio)
 * ============================================================ */
#define BR_BUF      ((u8 *)0x2000000)      /* resposta HTTP crua (256 KB) */
#define BR_BUFCAP   (256 * 1024)
#define BR_TXT      ((u8 *)0x2040000)      /* texto formatado (64 KB) */
#define BR_TXTCAP   (64 * 1024)
#define BR_LINES    ((BrLine *)0x2050000)  /* linhas formatadas */
#define BR_MAXL     512
#define BR_URLS     ((char *)0x2054000)    /* URLs dos links (8 KB) */
#define BR_URLSCAP  8192

typedef struct {
    u16 off, len;        /* texto em BR_TXT (indices de glifo) */
    u16 link_off;        /* offset da URL em BR_URLS (0 = sem link) */
    u16 img_idx;         /* indice da imagem em br_imgs (0xFFFF = nenhuma) */
    u8  style;
    u8  indent;          /* recuo em pixels */
} BrLine;

#define BS_TEXT   0
#define BS_H1     1
#define BS_H2     2
#define BS_H3     3
#define BS_LIST   4
#define BS_PRE    5
#define BS_LINK   6
#define BS_BOLD   7
#define BS_QUOTE  8
#define BS_SPACE  9
#define BS_RULE   10
#define BS_IMAGE  11

#define BRK_UP    0x01
#define BRK_DOWN  0x02
#define BRK_PGUP  0x03
#define BRK_PGDN  0x04
#define BRK_HOME  0x05
#define BRK_END   0x06

static struct {
    char url[384];
    char title[128];
    int editing, caret;
    int scroll;
    int nlines;
    int loading;
    int status, error;
    int built;
    int hist_n, hist_i;
    char hist[6][384];
} br;

/* ---------- armazenamento de imagens (BMP decodificado) ---------- */
#define BR_MAX_IMG 4
#define BR_IMG_BUF ((u32 *)0x2100000)   /* 256 KB para pixels das imagens */
#define BR_IMG_CAP (64 * 1024)          /* 64K u32 pixels */
#define BR_IMG_DL  ((u8 *)0x2080000)    /* buffer temporario de download (256 KB) */
#define BR_IMG_DLCAP (256 * 1024)

struct BrImg { u32 w, h; int loaded; u32 offset; };
static struct BrImg br_imgs[BR_MAX_IMG];
static int br_img_n = 0;
static u32 br_img_buf_used = 0;

/* prototipos antecipados (usados por br_load_image) */
static int br_copy(char *d, int cap, const char *s);
static int br_cat(char *d, int cap, const char *s);

/* Baixa uma imagem (resolvendo URL relativa contra br.url) e tenta decodar como BMP.
 * Retorna o indice em br_imgs ou -1 se falhar. */
static int br_load_image(const char *rel_url) {
    if (br_img_n >= BR_MAX_IMG) return -1;
    char resolved[384];
    if (!http_resolve(br.url, rel_url, resolved, sizeof resolved))
        br_copy(resolved, sizeof resolved, rel_url);

    /* baixa para buffer temporario (espera ate concluir ou falhar) */
    if (!http_get(resolved, BR_IMG_DL, BR_IMG_DLCAP)) return -1;

    for (int t = 0; t < 5000; t++) {
        http_poll();
        net_poll();
        const struct http_info *hi = http_get_info();
        if (hi->state == HTTP_ST_DONE || hi->state == HTTP_ST_ERROR) break;
        for (volatile int k = 0; k < 5000; k++) {}
    }

    const struct http_info *hi = http_get_info();
    if (hi->state != HTTP_ST_DONE) return -1;

    u32 body_len = hi->body_len;
    const u8 *body = BR_IMG_DL + hi->body_off;

    u32 w, h;
    int idx = br_img_n;
    u32 *pix = BR_IMG_BUF + br_img_buf_used;
    if (!video_decode_bmp(body, body_len, &w, &h, pix)) return -1;

    u32 need = w * h;
    if (br_img_buf_used + need > BR_IMG_CAP) return -1;

    br_imgs[idx].w = w;
    br_imgs[idx].h = h;
    br_imgs[idx].loaded = 1;
    br_imgs[idx].offset = br_img_buf_used;
    br_img_buf_used += need;
    br_img_n++;
    return idx;
}

static int br_bn;                    /* linhas construidas */
static u32 br_txtlen;
static int br_urls_len;
static u8 br_cur[400];               /* linha atual (indices de glifo) */
static int br_cl;
static int br_cstyle = BS_TEXT, br_cindent, br_clink;
static int br_maxw = 700;
static u8 br_wordbuf[256];
static int br_wordn;
static int br_in_pre, br_in_title, br_title_n;

static void br_build(const u8 *html, u32 len);

/* ---------- utilidades de texto ---------- */
static int br_cat(char *d, int cap, const char *s);
static int br_copy(char *d, int cap, const char *s) {
    int i = 0;
    if (cap <= 0) return 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
    return i;
}
static int br_cat(char *d, int cap, const char *s) {
    int n = 0; while (d[n]) n++;
    return br_copy(d + n, cap - n, s);
}
static int br_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int br_starts(const char *s, const char *p) {
    while (*p) {
        char a = *s++, b = *p++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}
static int br_tag(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i]) {
        char c = a[i], d = b[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != d) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}
static int br_w(const u8 *s, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) w += gadv[s[i]];
    return w;
}

/* ---------- historico e navegacao ---------- */
static void br_show_home(void);

static void br_load(const char *url, int push) {
    char full[384];
    const char *p = url;
    while (*p == ' ') p++;
    if (!*p) return;
    if (br_starts(p, "http://")) br_copy(full, sizeof full, p);
    else if (br_starts(p, "https://")) { br.error = HTTP_ERR_HTTPS; br.loading = 0; br.built = 0; dirty = 1; return; }
    else if (p[0] == '/' && p[1] == '/') { br_copy(full, sizeof full, "http:"); br_cat(full, sizeof full, p); }
    else { br_copy(full, sizeof full, "http://"); br_cat(full, sizeof full, p); }

    br_copy(br.url, sizeof br.url, full);
    br.caret = br_len(br.url);
    br.scroll = 0;
    br.loading = 1;
    br.built = 0;
    br.status = 0;
    br.error = 0;
    br.title[0] = 0;

    if (push) {
        if (br.hist_i < br.hist_n - 1) br.hist_n = br.hist_i + 1;
        if (br.hist_n >= 6) {
            for (int i = 1; i < 6; i++) br_copy(br.hist[i - 1], sizeof br.hist[0], br.hist[i]);
            br.hist_n = 5;
        }
        br_copy(br.hist[br.hist_n], sizeof br.hist[0], full);
        br.hist_n++;
        br.hist_i = br.hist_n - 1;
    }

    if (!http_get(full, BR_BUF, BR_BUFCAP)) {
        br.loading = 0;
        br.error = HTTP_ERR_CONNECT;
    }
    dirty = 1;
}

static void br_navigate(const char *url) { br_load(url, 1); }

static void br_back(void) {
    if (br.hist_i > 0) { br.hist_i--; br_load(br.hist[br.hist_i], 0); }
}
static void br_reload(void) { if (br.url[0]) br_load(br.url, 0); }

static void br_poll(void) {
    if (!br.loading) return;
    const struct http_info *h = http_get_info();
    if (h->state == HTTP_ST_DONE) {
        br.loading = 0;
        br.status = h->status;
        br.error = 0;
        br_build(BR_BUF + h->body_off, h->body_len);
        br.built = 1;
        br.scroll = 0;
        dirty = 1;
    } else if (h->state == HTTP_ST_ERROR) {
        br.loading = 0;
        br.error = h->error;
        br.built = 0;
        dirty = 1;
    }
}

/* ============================================================
 *  Renderizador HTML -> linhas de glifos
 * ============================================================ */
static int br_add_url(const u8 *u, int n) {
    if (n <= 0 || br_urls_len + n + 1 > BR_URLSCAP) return 0;
    int off = br_urls_len;
    for (int i = 0; i < n; i++) BR_URLS[off + i] = (char)u[i];
    BR_URLS[off + n] = 0;
    br_urls_len += n + 1;
    return off;
}

static void br_emit_line(int style, int indent, int link, const u8 *s, int n) {
    if (br_bn >= BR_MAXL) return;
    if (br_txtlen + (u32)n > BR_TXTCAP) return;
    BrLine *L = &BR_LINES[br_bn++];
    L->off = (u16)br_txtlen;
    L->len = (u16)n;
    L->link_off = (u16)link;
    L->img_idx = 0xFFFF;
    L->style = (u8)style;
    L->indent = (u8)indent;
    for (int i = 0; i < n; i++) BR_TXT[br_txtlen++] = s[i];
}

static void br_space(void) {
    if (br_bn == 0) return;
    if (BR_LINES[br_bn - 1].style == BS_SPACE) return;
    br_emit_line(BS_SPACE, 0, 0, 0, 0);
}

static void br_flush(void) {
    while (br_cl > 0 && br_cur[br_cl - 1] == ' ') br_cl--;
    if (br_cl > 0) br_emit_line(br_cstyle, br_cindent, br_clink, br_cur, br_cl);
    br_cl = 0;
}

static void br_word(const u8 *w, int n) {
    if (!n) return;
    int ww = br_w(w, n);
    int sp = (br_cl > 0) ? gadv[' '] : 0;
    if (br_cl > 0 && br_w(br_cur, br_cl) + sp + ww > br_maxw) br_flush();
    if (br_cl > 0 && br_cl < 399) br_cur[br_cl++] = ' ';
    for (int i = 0; i < n; i++) {
        int a = gadv[w[i]];
        if (br_cl > 0 && br_w(br_cur, br_cl) + a > br_maxw) br_flush();
        if (br_cl < 399) br_cur[br_cl++] = w[i];
    }
}
static void br_word_flush(void) {
    if (br_wordn) { br_word(br_wordbuf, br_wordn); br_wordn = 0; }
}

static int br_named_eq(const u8 *s, int n, const char *t) {
    int i = 0;
    while (i < n && t[i]) { if (s[i] != (u8)t[i]) return 0; i++; }
    return i == n && t[i] == 0;
}

static u32 br_entity(const u8 *s, int len, int *used) {
    *used = 0;
    if (len < 3 || s[0] != '&') return 0;
    int semi = -1;
    for (int i = 1; i < len && i < 12; i++) if (s[i] == ';') { semi = i; break; }
    if (semi < 0) return 0;
    *used = semi + 1;
    if (s[1] == '#') {
        u32 v = 0;
        int i = 2, hex = 0;
        if (i < semi && (s[i] == 'x' || s[i] == 'X')) { hex = 1; i++; }
        if (i >= semi) { *used = 0; return 0; }
        for (; i < semi; i++) {
            int c = s[i], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else { *used = 0; return 0; }
            v = v * (hex ? 16 : 10) + (u32)d;
            if (v > 0x10FFFF) { *used = 0; return 0; }
        }
        return v;
    }
    if (br_named_eq(s + 1, semi - 1, "amp")) return '&';
    if (br_named_eq(s + 1, semi - 1, "lt")) return '<';
    if (br_named_eq(s + 1, semi - 1, "gt")) return '>';
    if (br_named_eq(s + 1, semi - 1, "quot")) return '"';
    if (br_named_eq(s + 1, semi - 1, "apos")) return '\'';
    if (br_named_eq(s + 1, semi - 1, "nbsp")) return 0x20;
    if (br_named_eq(s + 1, semi - 1, "copy")) return 0xA9;
    if (br_named_eq(s + 1, semi - 1, "reg")) return 0xAE;
    if (br_named_eq(s + 1, semi - 1, "deg")) return 0xB0;
    if (br_named_eq(s + 1, semi - 1, "middot")) return 0xB7;
    if (br_named_eq(s + 1, semi - 1, "bull")) return 0x2022;
    if (br_named_eq(s + 1, semi - 1, "mdash")) return 0x2014;
    if (br_named_eq(s + 1, semi - 1, "ndash")) return 0x2013;
    if (br_named_eq(s + 1, semi - 1, "hellip")) return 0x2026;
    if (br_named_eq(s + 1, semi - 1, "laquo")) return 0xAB;
    if (br_named_eq(s + 1, semi - 1, "raquo")) return 0xBB;
    if (br_named_eq(s + 1, semi - 1, "trade")) return 0x2122;
    if (br_named_eq(s + 1, semi - 1, "euro")) return 0x20AC;
    if (br_named_eq(s + 1, semi - 1, "pound")) return 0xA3;
    if (br_named_eq(s + 1, semi - 1, "sect")) return 0xA7;
    if (br_named_eq(s + 1, semi - 1, "para")) return 0xB6;
    if (br_named_eq(s + 1, semi - 1, "times")) return 0xD7;
    if (br_named_eq(s + 1, semi - 1, "divide")) return 0xF7;
    if (br_named_eq(s + 1, semi - 1, "plusmn")) return 0xB1;
    if (br_named_eq(s + 1, semi - 1, "frac12")) return 0xBD;
    if (br_named_eq(s + 1, semi - 1, "ordm")) return 0xBA;
    if (br_named_eq(s + 1, semi - 1, "ordf")) return 0xAA;
    *used = 0;
    return 0;
}

static void br_text(const u8 *s, int len) {
    int i = 0;
    while (i < len) {
        u32 cp;
        if (s[i] == '&') {
            int used = 0;
            u32 e = br_entity(s + i, len - i, &used);
            if (used) { cp = e; i += used; }
            else { cp = '&'; i++; }
        } else {
            const char *p = (const char *)(s + i);
            cp = utf8_next(&p);
            i = (int)((const u8 *)p - s);
        }

        if (br_in_title) {
            if (cp < 0x80 && br_title_n < (int)sizeof br.title - 1) br.title[br_title_n++] = (char)cp;
            continue;
        }

        if (br_in_pre) {
            if (cp == '\n') br_flush();
            else if (cp == ' ' || cp == '\t') {
                int nsp = (cp == '\t') ? 4 : 1;
                for (int k = 0; k < nsp; k++) if (br_cl < 399) br_cur[br_cl++] = ' ';
            } else {
                int g = glyph_of(cp);
                if (g == '?') g = '?';
                if (br_cl > 0 && br_w(br_cur, br_cl) + gadv[g] > br_maxw) br_flush();
                if (br_cl < 399) br_cur[br_cl++] = (u8)g;
            }
            continue;
        }

        if (cp == '\r') continue;
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == 0xA0) { br_word_flush(); continue; }

        int g = glyph_of(cp);
        if (cp == 0x2022 || cp == 0x25CF || cp == 0x25AA) g = '*';
        if (br_wordn < 255) br_wordbuf[br_wordn++] = (u8)g;
        else { br_word_flush(); br_wordbuf[br_wordn++] = (u8)g; }
    }
}

/* Le um atributo dentro da tag html[a..b) */
static int br_attr(const u8 *h, u32 a, u32 b, const char *name, char *out, int outsz) {
    int nl = br_len(name);
    u32 p = a;
    while (p < b) {
        while (p < b && (h[p] == ' ' || h[p] == '\t' || h[p] == '\r' || h[p] == '\n' || h[p] == '/')) p++;
        u32 ns = p;
        while (p < b && ((h[p] >= 'a' && h[p] <= 'z') || (h[p] >= 'A' && h[p] <= 'Z') || h[p] == '-' || h[p] == '_')) p++;
        u32 ne = p;
        if (ne == ns) { p++; continue; }
        int eq = (ne - ns == (u32)nl);
        if (eq) for (int k = 0; k < nl; k++) {
            char c = (char)h[ns + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != name[k]) { eq = 0; break; }
        }
        while (p < b && (h[p] == ' ' || h[p] == '\t')) p++;
        if (p < b && h[p] == '=') {
            p++;
            while (p < b && (h[p] == ' ' || h[p] == '\t')) p++;
            u8 q = 0;
            if (p < b && (h[p] == '"' || h[p] == '\'')) q = h[p++];
            u32 vs = p;
            if (q) { while (p < b && h[p] != q) p++; }
            else { while (p < b && h[p] != ' ' && h[p] != '\t' && h[p] != '>') p++; }
            if (eq) {
                int n = (int)(p - vs);
                if (n > outsz - 1) n = outsz - 1;
                for (int k = 0; k < n; k++) out[k] = (char)h[vs + k];
                out[n] = 0;
                return 1;
            }
            if (q && p < b) p++;
        } else if (eq) {
            out[0] = 0;
            return 1;
        }
    }
    return 0;
}

/* Decodifica entidades em atributos (href) */
static void br_decode_attr(char *s) {
    char out[384];
    int o = 0;
    for (int i = 0; s[i] && o < (int)sizeof out - 1; ) {
        if (s[i] == '&') {
            int used = 0;
            u32 cp = br_entity((const u8 *)s + i, br_len(s + i), &used);
            if (used) {
                if (cp < 0x80) out[o++] = (char)cp;
                i += used;
                continue;
            }
        }
        out[o++] = s[i++];
    }
    out[o] = 0;
    br_copy(s, 384, out);
}

static u32 br_skip_element(const u8 *h, u32 len, u32 i, const char *name) {
    int nl = br_len(name);
    while (i < len) {
        if (h[i] == '<' && i + 1 < len && h[i + 1] == '/') {
            int ok = 1;
            for (int k = 0; k < nl; k++) {
                u32 p = i + 2 + (u32)k;
                if (p >= len) { ok = 0; break; }
                char c = (char)h[p];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                if (c != name[k]) { ok = 0; break; }
            }
            if (ok) {
                u32 j = i + 2 + (u32)nl;
                while (j < len && h[j] != '>') j++;
                return (j < len) ? j + 1 : len;
            }
        }
        i++;
    }
    return len;
}

static int br_is_block(const char *t) {
    return br_tag(t, "p") || br_tag(t, "div") || br_tag(t, "section") ||
           br_tag(t, "article") || br_tag(t, "header") || br_tag(t, "footer") ||
           br_tag(t, "nav") || br_tag(t, "main") || br_tag(t, "table") ||
           br_tag(t, "tr") || br_tag(t, "center") || br_tag(t, "form") ||
           br_tag(t, "address") || br_tag(t, "dl") || br_tag(t, "dt") || br_tag(t, "dd");
}

static void br_build(const u8 *html, u32 len) {
    br_bn = 0;
    br_txtlen = 0;
    br_cl = 0;
    br_wordn = 0;
    br_urls_len = 1;
    BR_URLS[0] = 0;
    br_title_n = 0;
    br.title[0] = 0;
    br_in_pre = 0;
    br_in_title = 0;
    br_cstyle = BS_TEXT;
    br_cindent = 0;
    br_clink = 0;
    br_maxw = 700;

    int style = BS_TEXT, indent = 0, link = 0;
    u32 i = 0;

    while (i < len && br_bn < BR_MAXL) {
        if (html[i] != '<') {
            u32 j = i;
            while (j < len && html[j] != '<') j++;
            br_text(html + i, (int)(j - i));
            i = j;
            continue;
        }

        /* comentario <!-- ... --> */
        if (i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
            u32 j = i + 4;
            while (j + 2 < len && !(html[j] == '-' && html[j + 1] == '-' && html[j + 2] == '>')) j++;
            i = (j + 2 < len) ? j + 3 : len;
            continue;
        }

        u32 j = i + 1;
        int q = 0;
        while (j < len && (html[j] != '>' || q)) {
            if (html[j] == '"') q ^= 1;
            j++;
        }
        if (j >= len) break;

        char tag[16];
        int tn = 0, closing = 0;
        u32 k = i + 1;
        if (k < j && html[k] == '/') { closing = 1; k++; }
        while (k < j && tn < 15) {
            char c = (char)html[k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) break;
            tag[tn++] = c;
            k++;
        }
        tag[tn] = 0;

        if (!closing && (br_tag(tag, "script") || br_tag(tag, "style"))) {
            i = br_skip_element(html, len, j + 1, tag);
            continue;
        }

        if (closing) {
            if (br_tag(tag, "a")) {
                br_word_flush();
                br_flush();
                style = BS_TEXT; link = 0; indent = 0;
                br_cstyle = style; br_clink = link; br_cindent = indent;
            } else if (br_tag(tag, "title")) {
                br_in_title = 0;
            } else if (br_tag(tag, "b") || br_tag(tag, "strong")) {
                if (style == BS_BOLD) { br_word_flush(); br_flush(); style = BS_TEXT; br_cstyle = style; }
            } else if (br_tag(tag, "h1") || br_tag(tag, "h2") || br_tag(tag, "h3") ||
                       br_tag(tag, "h4") || br_tag(tag, "h5") || br_tag(tag, "h6")) {
                br_word_flush();
                br_flush();
                style = BS_TEXT; indent = 0;
                br_cstyle = style; br_cindent = indent;
                br_space();
            } else if (br_tag(tag, "pre")) {
                br_word_flush();
                br_flush();
                br_in_pre = 0; style = BS_TEXT; indent = 0;
                br_cstyle = style; br_cindent = indent;
                br_space();
            } else if (br_tag(tag, "blockquote")) {
                br_word_flush();
                br_flush();
                style = BS_TEXT; indent = 0;
                br_cstyle = style; br_cindent = indent;
                br_space();
            } else if (br_tag(tag, "li")) {
                br_word_flush();
                br_flush();
                indent = 0;
                br_cindent = indent;
            } else if (br_tag(tag, "ul") || br_tag(tag, "ol")) {
                br_word_flush();
                br_flush();
                br_space();
            } else if (br_is_block(tag)) {
                br_word_flush();
                br_flush();
                br_space();
            }
            i = j + 1;
            continue;
        }

        /* tags de abertura */
        if (br_tag(tag, "title")) {
            br_in_title = 1;
            br_title_n = 0;
        } else if (br_tag(tag, "p")) {
            br_word_flush();
            br_flush();
            br_space();
        } else if (br_tag(tag, "br")) {
            br_word_flush();
            if (br_cl == 0) br_space();
            else br_flush();
        } else if (br_tag(tag, "hr")) {
            br_word_flush();
            br_flush();
            br_space();
            br_emit_line(BS_RULE, 0, 0, 0, 0);
            br_space();
        } else if (br_tag(tag, "h1") || br_tag(tag, "h2") || br_tag(tag, "h3") ||
                   br_tag(tag, "h4") || br_tag(tag, "h5") || br_tag(tag, "h6")) {
            br_word_flush();
            br_flush();
            br_space();
            style = (tag[1] == '1') ? BS_H1 : (tag[1] == '2') ? BS_H2 : BS_H3;
            indent = 0;
            br_cstyle = style; br_cindent = indent;
        } else if (br_tag(tag, "li")) {
            br_word_flush();
            br_flush();
            style = BS_LIST;
            indent = 18;
            br_cstyle = style; br_cindent = indent;
        } else if (br_tag(tag, "ul") || br_tag(tag, "ol")) {
            br_word_flush();
            br_flush();
        } else if (br_tag(tag, "pre")) {
            br_word_flush();
            br_flush();
            br_in_pre = 1;
            style = BS_PRE;
            indent = 0;
            br_cstyle = style; br_cindent = indent;
        } else if (br_tag(tag, "blockquote")) {
            br_word_flush();
            br_flush();
            style = BS_QUOTE;
            indent = 18;
            br_cstyle = style; br_cindent = indent;
        } else if (br_tag(tag, "a")) {
            char href[384];
            if (br_attr(html, i + 1, j, "href", href, sizeof href) && href[0]) {
                br_decode_attr(href);
                br_word_flush();
                br_flush();
                link = br_add_url((const u8 *)href, br_len(href));
                style = BS_LINK;
                indent = 0;
                br_cstyle = style; br_cindent = indent; br_clink = link;
            }
        } else if (br_tag(tag, "b") || br_tag(tag, "strong")) {
            if (style != BS_LINK) {
                br_word_flush();
                style = BS_BOLD;
                br_cstyle = style;
            }
        } else if (br_tag(tag, "img")) {
            char src[384];
            char alt[160];
            int img_index = -1;
            u32 img_w = 0, img_h = 0;

            /* tenta baixar e decodar a imagem (BMP) */
            if (br_attr(html, i + 1, j, "src", src, sizeof src) && src[0]) {
                br_decode_attr(src);
                br_word_flush();
                br_flush();
                img_index = br_load_image(src);
                if (img_index >= 0) {
                    img_w = br_imgs[img_index].w;
                    img_h = br_imgs[img_index].h;
                }
            }

            if (img_index >= 0) {
                /* cria uma linha com estilo BS_IMAGE e o indice da imagem */
                if (br_bn < BR_MAXL) {
                    BrLine *L = &BR_LINES[br_bn++];
                    L->off = 0;
                    L->len = 0;
                    L->link_off = 0;
                    L->style = BS_IMAGE;
                    L->indent = 0;
                    L->img_idx = (u16)img_index;
                    /* armazena altura/altura na linha via off/len temporariamente */
                    /* usamos off para a altura e len para a largura (sinalizados por style==BS_IMAGE) */
                    L->off = (u16)(img_h & 0xFFFF);
                    L->len = (u16)(img_w & 0xFFFF);
                }
                br_space();
            } else {
                /* fallback: mostra texto alternativo */
                if (br_attr(html, i + 1, j, "alt", alt, sizeof alt) && alt[0]) {
                    br_decode_attr(alt);
                    u8 tmp[180];
                    int n = 0;
                    const char *pre = "[imagem: ";
                    while (*pre && n < 170) tmp[n++] = (u8)*pre++;
                    for (const char *a = alt; *a && n < 170; a++) tmp[n++] = (u8)glyph_of((u8)*a);
                    if (n < 170) tmp[n++] = ']';
                    br_word_flush();
                    br_word(tmp, n);
                    br_word_flush();
                }
            }
        } else if (br_tag(tag, "td") || br_tag(tag, "th")) {
            br_word_flush();
            if (br_cl > 0 && br_cl < 396) { br_cur[br_cl++] = ' '; br_cur[br_cl++] = ' '; br_cur[br_cl++] = '|'; br_cur[br_cl++] = ' '; }
        } else if (br_is_block(tag)) {
            br_word_flush();
            br_flush();
        }

        i = j + 1;
    }

    br_word_flush();
    br_flush();
    br.nlines = br_bn;
}

/* ---------- navegador: desenho e interacao ---------- */
static const char br_home_html[] =
"<html><head><title>Elus Inicio</title></head><body>"
"<h1>Bem-vindo ao Navegador Elus</h1>"
"<p>Navegador escrito do zero em C, com pilha TCP/IP propria: ARP, IPv4, TCP, UDP, DNS e HTTP/1.0.</p>"
"<h2>Experimente</h2>"
"<ul>"
"<li><a href=\"http://example.com/\">example.com</a></li>"
"<li><a href=\"http://info.cern.ch/\">info.cern.ch - a primeira pagina web</a></li>"
"<li><a href=\"http://10.0.2.2:8000/\">servidor local do host (QEMU)</a></li>"
"</ul>"
"<h2>Dicas</h2>"
"<p>Clique na barra de endereco para digitar uma URL e pressione Enter. Use as setas, Page Up/Page Down ou a barra de rolagem para navegar. Clique em um link para abri-lo.</p>"
"<p>HTTPS ainda nao e suportado; use enderecos http://.</p>"
"</body></html>";

static void br_show_home(void) {
    br.url[0] = 0;
    br.caret = 0;
    br.loading = 0;
    br.status = 0;
    br.error = 0;
    br_build((const u8 *)br_home_html, (u32)sizeof br_home_html - 1);
    br.built = 1;
    br.scroll = 0;
    br.editing = 1;
    br_copy(br.title, sizeof br.title, "Elus Inicio");
    dirty = 1;
}

static void br_init(void) {
    br.hist_n = 0;
    br.hist_i = 0;
    br_copy(br.url, sizeof br.url, "");
    br_show_home();
}

static int br_line_h(BrLine *L) {
    if (L->style == BS_H1) return 32;
    if (L->style == BS_H2) return 26;
    if (L->style == BS_H3) return 22;
    if (L->style == BS_SPACE) return 8;
    if (L->style == BS_RULE) return 10;
    if (L->style == BS_IMAGE) return (int)L->off;   /* off armazena a altura da imagem */
    return 20;
}

static int br_total_h(void) {
    int t = 0;
    for (int i = 0; i < br.nlines; i++) t += br_line_h(&BR_LINES[i]);
    return t;
}

static int br_view_h(int id) { return W[id].h - 120 - 20; }

static void br_scroll_clamp(int id) {
    int maxs = br_total_h() - br_view_h(id);
    if (maxs < 0) maxs = 0;
    if (br.scroll > maxs) br.scroll = maxs;
    if (br.scroll < 0) br.scroll = 0;
}

static void br_btn(int x, int y, int w, const char *label, int enabled) {
    fill_rr(x, y, w, 30, 10, OVC, OVA(enabled ? 46 : 24));
    rim(x, y, w, 30, 10, 60, 18);
    text_c(x + w / 2, y + 7, label, enabled ? TXT : TXT2, enabled ? 240 : 130);
}

static void br_draw_line(BrLine *L, int x, int y, int maxw) {
    if (L->style == BS_SPACE) return;
    if (L->style == BS_RULE) { rect_a(x, y + 4, maxw - 8, 1, OVC, OVA(46)); return; }

    if (L->style == BS_IMAGE) {
        if (L->img_idx < (u16)br_img_n && br_imgs[L->img_idx].loaded) {
            u32 *pix = BR_IMG_BUF + br_imgs[L->img_idx].offset;
            video_draw_bmp(x + L->indent, y, pix,
                           br_imgs[L->img_idx].w, br_imgs[L->img_idx].h);
        } else {
            fill_rr(x + L->indent, y, 40, 20, 4, OVC, OVA(46));
            text_b(x + L->indent + 2, y + 4, "img", TXT2, 180);
        }
        return;
    }

    int tx = x + L->indent;
    u32 col = TXT;
    int al = 256;
    if (L->style == BS_H1) col = ACCENT;
    else if (L->style == BS_H2) { col = TXT; al = 256; }
    else if (L->style == BS_H3) { col = TXT; al = 225; }
    else if (L->style == BS_LINK) col = ACCENT;
    else if (L->style == BS_QUOTE || L->style == BS_PRE) col = TXT2;

    if (L->style == BS_LIST) fill_circle(x + 7, y + 10, 2, ACCENT, 230);

    int xx = tx;
    for (int i = 0; i < L->len; i++) {
        int g = BR_TXT[L->off + i];
        if (L->style == BS_H1 || L->style == BS_BOLD) glyph_a(xx + 1, y + 1, g, col, al);
        else glyph_a(xx, y + 1, g, col, al);
        xx += gadv[g];
    }
    if (L->style == BS_LINK) {
        int w = br_w(BR_TXT + L->off, L->len);
        rect_a(tx, y + 17, w, 1, ACCENT, 170);
    }
}

static void br_status_text(char *out, int cap) {
    if (br.loading) {
        const struct http_info *h = http_get_info();
        if (h->state == HTTP_ST_DNS) br_copy(out, cap, "Resolvendo nome...");
        else if (h->state == HTTP_ST_CONNECT) br_copy(out, cap, "Conectando...");
        else {
            char nb[12];
            br_copy(out, cap, "Recebendo... ");
            br_cat(out, cap, u32str(h->received / 1024, nb));
            br_cat(out, cap, " KB");
        }
        return;
    }
    if (br.error) {
        const char *m = "Sem conexao de rede";
        if (br.error == HTTP_ERR_URL) m = "URL invalida";
        else if (br.error == HTTP_ERR_HTTPS) m = "HTTPS nao e suportado";
        else if (br.error == HTTP_ERR_DNS) m = "Falha ao resolver o nome (DNS)";
        else if (br.error == HTTP_ERR_CONNECT) m = "Falha na conexao";
        else if (br.error == HTTP_ERR_TIMEOUT) m = "Tempo esgotado";
        else if (br.error == HTTP_ERR_PROTOCOL) m = "Resposta HTTP invalida";
        else if (br.error == HTTP_ERR_ABORTED) m = "Cancelado";
        br_copy(out, cap, "Erro: ");
        br_cat(out, cap, m);
        return;
    }
    if (br.built) {
        char nb[12];
        const struct http_info *h = http_get_info();
        br_copy(out, cap, "Concluido");
        if (br.status) {
            br_cat(out, cap, " - HTTP ");
            br_cat(out, cap, u32str((u32)br.status, nb));
        }
        br_cat(out, cap, " - ");
        br_cat(out, cap, u32str(h->body_len / 1024, nb));
        br_cat(out, cap, " KB");
        if (h->truncated) br_cat(out, cap, " (pagina cortada)");
        return;
    }
    br_copy(out, cap, "Digite um endereco e pressione Enter");
}

static void br_draw(int id, int x, int y, int ww, int hh) {
    (void)id;
    int ty = y + 46;
    br_btn(x + 14, ty, 52, "Voltar", br.hist_i > 0);
    br_btn(x + 72, ty, 52, "Inicio", 1);
    br_btn(x + 130, ty, 78, "Recarregar", br.url[0] != 0);

    int ubx = x + 216, ubw = ww - 286;
    fill_rr(ubx, ty, ubw, 32, 10, cfg.theme ? 0xFFFFFF : 0x000000, cfg.theme ? 150 : 90);
    rim(ubx, ty, ubw, 32, 10, br.editing ? 150 : 50, 18);

    const char *shown = br.url[0] ? br.url : "Digite um endereco (ex: example.com)";
    int maxw = ubw - 20, start = 0;
    while (shown[start] && twidth(shown + start) > maxw) start++;
    text_b(ubx + 10, ty + 8, shown + start, br.url[0] ? TXT : TXT2, br.url[0] ? 240 : 150);
    if (br.editing && blink) {
        int cw = 0;
        for (int i = start; i < br.caret && br.url[i]; i++) cw += gadv[glyph_of((u8)br.url[i])];
        rect_a(ubx + 10 + cw, ty + 6, 2, 19, ACCENT, 256);
    }

    br_btn(x + ww - 62, ty, 48, "Ir", 1);

    int px = x + 14, py = y + 86, pw = ww - 28, ph = hh - 120;
    fill_rr(px, py, pw, ph, 12, cfg.theme ? 0xFFFFFF : 0x000000, cfg.theme ? 110 : 55);
    rim(px, py, pw, ph, 12, 45, 14);

    int vx = px + 12, vy = py + 10, vw = pw - 34, vh = ph - 20;
    int total = br_total_h();
    int maxs = total - vh;
    if (maxs < 0) maxs = 0;
    if (br.scroll > maxs) br.scroll = maxs;
    if (br.scroll < 0) br.scroll = 0;

    clip_on = 1; clip_y0 = vy; clip_y1 = vy + vh;
    int yy = vy - br.scroll;
    for (int i = 0; i < br.nlines; i++) {
        BrLine *L = &BR_LINES[i];
        int h = br_line_h(L);
        if (yy + h > vy && yy < vy + vh) br_draw_line(L, vx, yy, vw);
        yy += h;
        if (yy >= vy + vh) break;
    }
    clip_on = 0;

    /* barra de rolagem */
    if (total > vh) {
        int sx = px + pw - 14, sy = py + 10, sh = vh;
        fill_rr(sx, sy, 6, sh, 3, OVC, OVA(24));
        int th = sh * vh / total;
        if (th < 24) th = 24;
        if (th > sh) th = sh;
        int tp = (sh - th) * br.scroll / (total - vh);
        if (tp < 0) tp = 0;
        if (tp > sh - th) tp = sh - th;
        fill_rr(sx, sy + tp, 6, th, 3, TXT, 120);
    }

    char st[160];
    br_status_text(st, sizeof st);
    text_b(x + 16, y + hh - 26, st, TXT2, 210);

    if (total > vh) {
        char pc[8];
        int pct = (total - vh) ? br.scroll * 100 / (total - vh) : 0;
        int n = utoa((u32)pct, pc);
        pc[n++] = '%'; pc[n] = 0;
        text_b(x + ww - 60, y + hh - 26, pc, TXT2, 180);
    }
}

static int br_click(int id, int lx, int ly) {
    int ww = W[id].w, hh = W[id].h;

    if (ly >= 46 && ly < 78) {
        if (lx >= 14 && lx < 66) { br_back(); return 0; }
        if (lx >= 72 && lx < 124) { br_show_home(); return 0; }
        if (lx >= 130 && lx < 208) { br_reload(); return 0; }
        if (lx >= 216 && lx < ww - 62) { br.editing = 1; br.caret = br_len(br.url); dirty = 1; return 0; }
        if (lx >= ww - 62 && lx < ww - 14) { br.editing = 0; br_navigate(br.url); return 0; }
        return 0;
    }

    int px = 14, py = 86, pw = ww - 28, ph = hh - 120;
    if (!inside(px, py, pw, ph, lx, ly)) return 0;

    br.editing = 0;

    /* barra de rolagem */
    if (lx >= px + pw - 16 && lx <= px + pw - 4) {
        int vh = ph - 20, total = br_total_h();
        if (total > vh) {
            int rel = ly - (py + 10);
            if (rel < 0) rel = 0;
            if (rel > vh) rel = vh;
            br.scroll = rel * (total - vh) / vh;
            br_scroll_clamp(id);
            dirty = 1;
            return 1;
        }
        return 0;
    }

    /* clique em link */
    int vy = py + 10, vh = ph - 20;
    int yy = vy - br.scroll;
    for (int i = 0; i < br.nlines; i++) {
        BrLine *L = &BR_LINES[i];
        int h = br_line_h(L);
        if (yy + h > vy && yy < vy + vh && ly >= yy && ly < yy + h) {
            if (L->link_off) {
                char resolved[384];
                if (http_resolve(br.url, BR_URLS + L->link_off, resolved, sizeof resolved))
                    br_navigate(resolved);
                else
                    br_navigate(BR_URLS + L->link_off);
            }
            break;
        }
        yy += h;
    }
    dirty = 1;
    return 0;
}

static void br_drag(int id, int lx, int ly) {
    (void)lx;
    int hh = W[id].h;
    int ph = hh - 120, vh = ph - 20;
    int total = br_total_h();
    if (total <= vh) return;
    int rel = ly - (86 + 10);
    if (rel < 0) rel = 0;
    if (rel > vh) rel = vh;
    br.scroll = rel * (total - vh) / vh;
    br_scroll_clamp(id);
    dirty = 1;
}

static void br_key(int id, u8 g) {
    if (br.editing) {
        if (g == '\n') { br.editing = 0; br_navigate(br.url); return; }
        if (g == '\b') {
            int n = br_len(br.url);
            if (n > 0) { br.url[n - 1] = 0; br.caret = n - 1; dirty = 1; }
            return;
        }
        if (g >= 32 && g < 127) {
            int n = br_len(br.url);
            if (n < (int)sizeof br.url - 1) {
                br.url[n] = (char)g;
                br.url[n + 1] = 0;
                br.caret = n + 1;
            }
            dirty = 1;
        }
        return;
    }

    int vh = br_view_h(id);
    switch (g) {
    case BRK_UP:   br.scroll -= 40; break;
    case BRK_DOWN: br.scroll += 40; break;
    case BRK_PGUP: br.scroll -= vh - 40; break;
    case BRK_PGDN: br.scroll += vh - 40; break;
    case BRK_HOME: br.scroll = 0; break;
    case BRK_END:  br.scroll = br_total_h(); break;
    case 'r': case 'R': br_reload(); return;
    case 'h': case 'H': br_show_home(); return;
    default: return;
    }
    br_scroll_clamp(id);
    dirty = 1;
}

static const App APPS[NAPPS] = {
    { "Meu Computador",  1, 120,  80, 420, 270, computer_draw, 0,           0,          0 },
    { "Bloco de Notas",  2, 250, 120, 440, 300, notes_draw,    0,           0,          notes_key },
    { "Sobre o Elus OS", 3, 430, 190, 340, 272, about_draw,    about_click, 0,          0 },
    { "Personalizar",   10, 520,  52, 480, 530, pers_draw,     pers_click,  pers_drag,  0 },
    { "Som & Áudio",    11, 280,  90, 480, 420, audio_draw,    audio_click, audio_drag, audio_key },
    { "Rede",           12, 100, 100, 440, 340, net_draw,      net_click,   0,          0 },
    { "Navegador",      13, 120,  60, 760, 560, br_draw,       br_click,    br_drag,    br_key },
};

static void draw_window(int id, int active) {
    Win *w = &W[id];
    int x = w->x, y = w->y, ww = w->w, hh = w->h;
    shadow(x, y, ww, hh, R_WIN, active ? 14 : 8, active ? 26 : 16, active ? 120 : 80);
    glass(x, y, ww, hh, R_WIN, GL_TINT, ta_of(active ? 70 : 48), 30, BLUR);
    rim(x, y, ww, hh, R_WIN, active ? 200 : 120, 36);
    fill_circle(x + 18, y + 18, 6, active ? 0xFF5F57 : 0x8A8A96, active ? 256 : 150);
    fill_circle(x + 38, y + 18, 6, active ? 0xFEBC2E : 0x8A8A96, active ? 256 : 150);
    text_c(x + ww / 2, y + 10, w->title, TXT, active ? 256 : 150);
    rect_a(x + 14, y + 37, ww - 28, 1, OVC, OVA(26));
    APPS[id].draw(id, x, y, ww, hh);
}

/* ---------- dock, menu, barra superior, widget ---------- */
#define DOCK_W (28 + (NAPPS + 1) * 48 + NAPPS * 12 + 12)
#define DOCK_H 66
static int dock_dx(void) { return (SW - DOCK_W) / 2; }
static int dock_y(void)  { return SH - DOCK_H - 14; }
static int dock_ix(int i) { return dock_dx() + 14 + i * 60 + (i > 0 ? 12 : 0); }
static int dock_at(int mx, int my) {
    for (int i = 0; i <= NAPPS; i++) if (inside(dock_ix(i), dock_y() + 4, 48, 56, mx, my)) return i;
    return -1;
}
static const char *dock_label(int i) { return i == 0 ? "Iniciar" : APPS[i - 1].title; }
static int menu_open, hover = -1, dock_hover = -1;
#define MENU_W 224
#define MENU_N (NAPPS + 2)
#define MENU_ROW 36
static const char *menu_label(int i) { return i < NAPPS ? APPS[i].title : (i == NAPPS ? "Reiniciar" : "Desligar"); }
static int menu_icon(int i) { return i < NAPPS ? APPS[i].icon : (i == NAPPS ? 4 : 5); }
static int menu_h(void) { return MENU_N * MENU_ROW + 16; }
static int menu_x(void) { return dock_dx(); }
static int menu_y(void) { return dock_y() - 12 - menu_h(); }
static int menu_at(int x, int y) {
    if (!inside(menu_x(), menu_y(), MENU_W, menu_h(), x, y)) return -1;
    int j = y - menu_y() - 8;
    if (j < 0) return -1;
    int i = j / MENU_ROW;
    return i < MENU_N ? i : -1;
}

static void draw_dock(void) {
    int dx = dock_dx(), dy = dock_y();
    shadow(dx, dy, DOCK_W, DOCK_H, 26, 10, 22, 110);
    glass(dx, dy, DOCK_W, DOCK_H, 26, GL_TINT, ta_of(58), 28, BLUR);
    rim(dx, dy, DOCK_W, DOCK_H, 26, 190, 44);
    rect_a(dx + 73, dy + 14, 1, 38, OVC, OVA(46));
    for (int i = 0; i <= NAPPS; i++) {
        int lift = (dock_hover == i) ? 5 : 0;
        draw_icon(i == 0 ? 0 : APPS[i - 1].icon, dock_ix(i), dy + 9 - lift, 48);
        if (i > 0 && is_open(i - 1))
            fill_circle(dock_ix(i) + 24, dy + DOCK_H - 7, 2, TXT, minz[i - 1] ? 110 : 235);
    }
    if (dock_hover >= 0 && !menu_open) {
        const char *nm = dock_label(dock_hover);
        int tw = twidth(nm) + 24;
        int tx = dock_ix(dock_hover) + 24 - tw / 2, ty = dy - 40;
        shadow(tx, ty, tw, 26, 13, 4, 10, 90);
        glass(tx, ty, tw, 26, 13, GL_TINT, ta_of(80), 24, 0);
        rim(tx, ty, tw, 26, 13, 170, 40);
        text_c(tx + tw / 2, ty + 5, nm, TXT, 256);
    }
}

static void draw_menu(void) {
    int mx = menu_x(), my = menu_y(), mh = menu_h();
    shadow(mx, my, MENU_W, mh, 22, 12, 24, 120);
    glass(mx, my, MENU_W, mh, 22, GL_TINT, ta_of(72), 30, BLUR);
    rim(mx, my, MENU_W, mh, 22, 200, 40);
    for (int i = 0; i < MENU_N; i++) {
        int y = my + 8 + i * MENU_ROW;
        if (i == hover) fill_rr(mx + 8, y, MENU_W - 16, MENU_ROW, 12, OVC, OVA(40));
        draw_icon(menu_icon(i), mx + 16, y + 5, 26);
        text(mx + 54, y + 10, menu_label(i), TXT);
    }
}

static void draw_topbar(void) {
    int bx = 10, by = 8, bw = SW - 20, bh = 30;
    shadow(bx, by, bw, bh, 15, 4, 12, 90);
    glass(bx, by, bw, bh, 15, GL_TINT, ta_of(46), 28, BLUR);
    rim(bx, by, bw, bh, 15, 180, 40);
    text_b(bx + 18, by + 7, "Elus OS", TXT, 256);
    int top = top_win();
    if (top >= 0) text_a(bx + 18 + twidth("Elus OS") + 22, by + 7, W[top].title, TXT, 200);
    char t[6]; itoa2(clk_h, t); t[2] = ':'; itoa2(clk_m, t + 3);
    int tw = twidth(t);
    text(bx + bw - 18 - tw, by + 7, t, TXT);
    text(bx + bw - 18 - tw - 14 - twidth(lay ? "US" : "PT"), by + 7, lay ? "US" : "PT", TXT2);
}

static void draw_widget(void) {
    int w = 284, h = 132, x = SW - w - 26, y = 60;
    shadow(x, y, w, h, 26, 12, 24, 100);
    glass(x, y, w, h, 26, GL_TINT, ta_of(20), 40, BLUR);
    rim(x, y, w, h, 26, 210, 50);
    char t[6]; itoa2(clk_h, t); t[2] = ':'; itoa2(clk_m, t + 3);
    int tx = x + (w - 112) / 2;
    for (int i = 0; i < 5; i++) {
        int idx = (t[i] == ':') ? 10 : t[i] - '0';
        const u8 *c = BIGC + idx * 1152;
        int ox = (idx == 10) ? -6 : 0;
        for (int r = 0; r < 48; r++)
            for (int k = 0; k < 24; k++) {
                u32 v = c[r * 24 + k];
                if (v) bpx(tx + ox + k, y + 16 + r, TXT, v);
            }
        tx += (idx == 10) ? 12 : 25;
    }
    char d[48]; int n = 0;
    n += scat(d + n, wd_name[weekday(clk_y, clk_mo ? clk_mo : 1, clk_d ? clk_d : 1)]);
    n += scat(d + n, ", ");
    n += utoa(clk_d, d + n);
    n += scat(d + n, " de ");
    scat(d + n, mo_name[(clk_mo ? clk_mo : 1) - 1]);
    text_c(x + w / 2, y + h - 36, d, TXT, 235);
}

/* ---------- cursor e cena ---------- */
static int MX = 512, MY = 384, drawn_x = 512, drawn_y = 384;
static const char *cur[] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ",
    "X.....X    ", "X......X   ", "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ",
    "X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     X..X  ", "      XX   ",
};
static void cursor_draw(void) {
    for (int r = 0; r < 17; r++)
        for (int c = 0; cur[r][c]; c++) {
            char k = cur[r][c];
            int x = MX + c, y = MY + r;
            if (k != ' ' && x < SW && y < SH) fb[y * pitch32 + x] = (k == 'X') ? 0x101018 : 0xFFFFFF;
        }
    drawn_x = MX; drawn_y = MY;
}

static int dragging = -1, drag_dx, drag_dy, prevL;

static void scene(void) {
    gfast = dragging >= 0 || cap >= 0;
    { u32 *s = WALL, *d = bb; int n = SW * SH; __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) :: "memory"); }
    if (cfg.widget) draw_widget();
    int top = top_win();
    for (int i = 0; i < zn; i++) if (!minz[zl[i]]) draw_window(zl[i], zl[i] == top);
    draw_topbar();
    draw_dock();
    if (menu_open) draw_menu();
    copy_to_fb(0, 0, SW, SH);
    cursor_draw();
    dirty = 0;
}

static void goodbye(void) {
    snd_fx_shutdown();
    u32 start = ticks;
    while (ticks - start < 35) {
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");
    rect(0, 0, SW, SH, 0x000000);
    text_c(SW / 2, SH / 2 - 8, "Agora você pode desligar o computador com segurança.", 0xFF9A3D, 256);
    copy_to_fb(0, 0, SW, SH);
    outw(0x604, 0x2000); outw(0xB004, 0x2000);
    for (;;) __asm__ volatile("hlt");
}
static void run_item(int it) {
    if (it < NAPPS) win_front(it);
    else if (it == NAPPS) reboot();
    else goodbye();
}

/* ---------- mouse ---------- */
static void kbc_in(void)  { for (int t = 0; t < 100000 && (inb(0x64) & 2); t++) {} }
static void kbc_out(void) { for (int t = 0; t < 100000 && !(inb(0x64) & 1); t++) {} }
static void mouse_cmd(u8 v) { kbc_in(); outb(0x64, 0xD4); kbc_in(); outb(0x60, v); kbc_out(); inb(0x60); }
static void mouse_init(void) {
    while (inb(0x64) & 1) inb(0x60);
    kbc_in(); outb(0x64, 0xA8);
    kbc_in(); outb(0x64, 0x20); kbc_out();
    u8 st = (inb(0x60) | 3) & ~0x30;           /* IRQ1 + IRQ12 ligadas, clocks ativos */
    kbc_in(); outb(0x64, 0x60); kbc_in(); outb(0x60, st);
    mouse_cmd(0xF6);
    mouse_cmd(0xF4);
    while (inb(0x64) & 1) inb(0x60);
}


static void do_click(void) {
    if (menu_open) {
        int it = menu_at(MX, MY);
        menu_open = 0; dirty = 1;
        snd_fx_click();
        if (it >= 0) run_item(it);
        return;
    }
    int di = dock_at(MX, MY);
    if (di >= 0) {
        dirty = 1;
        snd_fx_click();
        if (di == 0) { menu_open = 1; hover = -1; return; }
        int id = di - 1;
        if (!is_open(id) || minz[id]) win_front(id);
        else if (top_win() == id) minz[id] = 1;
        else win_front(id);
        return;
    }
    for (int i = zn - 1; i >= 0; i--) {
        int id = zl[i]; Win *w = &W[id];
        if (minz[id] || !inside(w->x, w->y, w->w, w->h, MX, MY)) continue;
        win_front(id); dirty = 1;
        if (inside(w->x + 10, w->y + 10, 16, 16, MX, MY)) { snd_fx_click(); win_close(id); return; }
        if (inside(w->x + 30, w->y + 10, 16, 16, MX, MY)) { snd_fx_click(); minz[id] = 1; return; }
        if (MY < w->y + 36) { snd_fx_click(); dragging = id; drag_dx = MX - w->x; drag_dy = MY - w->y; return; }
        if (APPS[id].click && APPS[id].click(id, MX - w->x, MY - w->y)) { cap = id; return; }
        snd_fx_click();
        return;
    }
}

static u8 pk[3]; static int pki, moved;
static void mouse_byte(u8 d) {
    if (pki == 0 && !(d & 8)) return;
    pk[pki++] = d;
    if (pki < 3) return;
    pki = 0;
    int dx = pk[1] - ((pk[0] << 4) & 0x100);
    int dy = pk[2] - ((pk[0] << 3) & 0x100);
    MX += dx; MY -= dy;
    if (MX < 0) MX = 0;
    if (MX > SW - 1) MX = SW - 1;
    if (MY < 0) MY = 0;
    if (MY > SH - 1) MY = SH - 1;
    int L = pk[0] & 1;
    moved = 1;
    if (!L) {
        if (dragging >= 0 || cap >= 0) dirty = 1;       /* volta ao desfoque completo */
        dragging = -1; cap = -1;
    }
    if (dragging >= 0 && L) {
        W[dragging].x = MX - drag_dx;
        W[dragging].y = MY - drag_dy;
        if (W[dragging].y < 46) W[dragging].y = 46;
        if (W[dragging].y > SH - 100) W[dragging].y = SH - 100;
        dirty = 1;
    }
    if (cap >= 0 && L && APPS[cap].drag) { APPS[cap].drag(cap, MX - W[cap].x, MY - W[cap].y); dirty = 1; }
    int dh = dock_at(MX, MY);
    if (dh != dock_hover) { dock_hover = dh; dirty = 1; }
    if (menu_open) { int h = menu_at(MX, MY); if (h != hover) { hover = h; dirty = 1; } }
    if (L && !prevL) do_click();
    prevL = L;
}

/* ---------- teclado: ABNT2 (com teclas mortas) e US ---------- */
#define DK_ACUTE 0xF1
#define DK_GRAVE 0xF2
#define DK_TILDE 0xF3
#define DK_CIRC  0xF4
#define DK_DIAER 0xF5

static const u8 abnt2[2][58] = {
  { 0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p', DK_ACUTE,'[','\n',0,
    'a','s','d','f','g','h','j','k','l', 0x87, DK_TILDE,'\'',0,']',
    'z','x','c','v','b','n','m',',','.',';',0,'*',0,' ' },
  { 0, 27, '!','@','#','$','%', DK_DIAER,'&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P', DK_GRAVE,'{','\n',0,
    'A','S','D','F','G','H','J','K','L', 0x80, DK_CIRC,'"',0,'}',
    'Z','X','C','V','B','N','M','<','>',':',0,'*',0,' ' },
};
static const u8 us[2][58] = {
  { 0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ' },
  { 0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ' },
};

static const struct { u8 dead, base, g; } comp[] = {
    {DK_ACUTE,'a',0xA0},{DK_ACUTE,'e',0x82},{DK_ACUTE,'i',0xA1},{DK_ACUTE,'o',0xA2},{DK_ACUTE,'u',0xA3},
    {DK_ACUTE,'c',0x87},{DK_ACUTE,'C',0x80},
    {DK_ACUTE,'A',2},{DK_ACUTE,'E',0x90},{DK_ACUTE,'I',6},{DK_ACUTE,'O',7},{DK_ACUTE,'U',18},
    {DK_GRAVE,'a',0x85},{DK_GRAVE,'e',0x8A},{DK_GRAVE,'i',0x8D},{DK_GRAVE,'o',0x95},{DK_GRAVE,'u',0x97},
    {DK_GRAVE,'A',1},
    {DK_TILDE,'a',14},{DK_TILDE,'o',15},{DK_TILDE,'n',0xA4},{DK_TILDE,'A',4},{DK_TILDE,'O',17},{DK_TILDE,'N',0xA5},
    {DK_CIRC,'a',0x83},{DK_CIRC,'e',0x88},{DK_CIRC,'i',0x8C},{DK_CIRC,'o',0x93},{DK_CIRC,'u',0x96},
    {DK_CIRC,'A',3},{DK_CIRC,'E',5},{DK_CIRC,'O',16},
    {DK_DIAER,'a',0x84},{DK_DIAER,'e',0x89},{DK_DIAER,'i',0x8B},{DK_DIAER,'o',0x94},{DK_DIAER,'u',0x81},
    {DK_DIAER,'A',0x8E},{DK_DIAER,'O',0x99},{DK_DIAER,'U',0x9A},
};
static u8 dead_literal(u8 dk) {
    return dk == DK_ACUTE ? '\'' : dk == DK_GRAVE ? '`' : dk == DK_TILDE ? '~' : dk == DK_CIRC ? '^' : '"';
}

static int shift, ext;
static u8 dead;

static void emit(u8 g) { int t = top_win(); if (t >= 0 && APPS[t].key) APPS[t].key(t, g); dirty = 1; }

static void key_byte(u8 d) {
    if (d == 0xE0) { ext = 1; return; }
    if (ext) {
        ext = 0;
        if (d == 0x58) { lay ^= 1; dead = 0; dirty = 1; cfg_save(); }
        else if (d == 0x48) emit(BRK_UP);
        else if (d == 0x50) emit(BRK_DOWN);
        else if (d == 0x49) emit(BRK_PGUP);
        else if (d == 0x51) emit(BRK_PGDN);
        else if (d == 0x47) emit(BRK_HOME);
        else if (d == 0x4F) emit(BRK_END);
        else if (d == 0x53) emit('\b');
        return;
    }
    if (d == 0x2A || d == 0x36) { shift = 1; return; }
    if (d == 0xAA || d == 0xB6) { shift = 0; return; }
    if (d & 0x80) return;
    if (d == 0x58) { lay ^= 1; dead = 0; dirty = 1; cfg_save(); return; }          /* F12 */
    u8 c = 0;
    if (d == 0x56) c = shift ? '|' : '\\';
    else if (d == 0x73) c = lay ? 0 : (shift ? '?' : '/');
    else if (d < 58) c = lay ? us[shift][d] : abnt2[shift][d];
    if (!c) return;
    if (c == 27) { if (menu_open) { menu_open = 0; dirty = 1; } dead = 0; return; }
    { int t = top_win(); if (t < 0 || !APPS[t].key) { dead = 0; return; } }      /* so apps com teclado recebem texto */
    if (c == '\b') { emit('\b'); dead = 0; return; }
    if (c == '\t') return;
    if (c >= DK_ACUTE && c <= DK_DIAER) {
        if (dead) { emit(dead_literal(dead)); }
        dead = c; return;
    }
    if (dead) {
        u8 dk = dead; dead = 0;
        for (unsigned i = 0; i < sizeof comp / sizeof comp[0]; i++)
            if (comp[i].dead == dk && comp[i].base == c) { emit(comp[i].g); return; }
        emit(dead_literal(dk));
        if (c == ' ') return;
    }
    emit(c);
}


/* ============================================================ */
void kmain(void) {
    fb = (u32 *)BI->fb; pitch32 = BI->pitch / 4; SW = BI->w; SH = BI->h;
    bb = (u32 *)0x400000;
    snd_init();
    net_init();
    video_init(fb, bb, SW, SH, 32);
    video_set_corners((const u8 *)CTAB);
    cfg_load();
    font_init();
    thumbs_init();
    wall_init(cfg.wall);
    br_init();
    for (int i = 0; i < NAPPS; i++) { W[i].x = APPS[i].x; W[i].y = APPS[i].y; W[i].w = APPS[i].w; W[i].h = APPS[i].h; W[i].title = APPS[i].title; }
    gdt_init(); idt_init(); pic_init(); pit_init();
    mouse_init();
    read_clock();
    win_front(2);
    win_front(4);
    win_front(5);
    u32 clk_next = 100;
    __asm__ volatile("sti");
    snd_fx_startup();                      /* som de boot assincrono do Elus OS */
#ifdef BR_DEMO
    win_front(6);
    /* descomente para abrir uma pagina automaticamente no boot:
       br_navigate("http://10.0.2.2:8000/teste.html"); */
#endif
    for (;;) {
        while (mqt != mqh) { u8 d = mq[mqt & (QN - 1)]; mqt++; mouse_byte(d); }
        while (kqt != kqh) { u8 d = kq[kqt & (QN - 1)]; kqt++; key_byte(d); }
        net_poll();                                    /* processa pacotes RX pendentes */
        http_poll();                                   /* avanca a maquina de estados HTTP */
        br_poll();                                     /* atualiza a pagina do navegador */
#ifdef NET_SELFTEST
        net_selftest();
#endif
        /* Verifica resposta de ping */
        if (net_pinging) {
            const struct net_info *ni = net_get_info();
            if (ni->pings_received > net_ping_rx_snap) {
                /* RTT real: delta de ticks (100Hz) * 10 = milissegundos */
                u32 dt = (ticks - net_ping_sent_tick) * 10;
                net_last_ping_ms = (int)(dt > 0 ? dt : 1);
                net_pinging = 0;
                dirty = 1;
            } else if (ticks - net_ping_sent_tick > 500) { /* timeout 5s */
                net_last_ping_ms = -1;
                net_pinging = 0;
                dirty = 1;
            }
        }
        if (ticks >= clk_next) { clk_next = ticks + 100; if (read_clock()) dirty = 1; }
        if (piano_key_ticks > 0) {
            piano_key_ticks--;
            if (piano_key_ticks == 0) dirty = 1;
        }
        int b = (top_win() == 1) ? (int)((ticks / 50) & 1) : 0;
        if (b != blink) { blink = b; dirty = 1; }
#ifdef TEST_UD2
        if (ticks > 400) __asm__ volatile("ud2");
#endif
        if (dirty) scene();
        else if (moved) { copy_to_fb(drawn_x, drawn_y, 12, 17); cursor_draw(); }
        moved = 0;
        __asm__ volatile("cli");
        if (mqt == mqh && kqt == kqh) __asm__ volatile("sti; hlt");
        else __asm__ volatile("sti");
    }
}
