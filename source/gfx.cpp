#include "gfx.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include "font_bin.h"

namespace gfx {

static Framebuffer g_fb;
static bool g_ok = false;
static u16 *g_px = nullptr;     // fotograma actual
static u32 g_stride = 0;        // en píxeles
static u16 *g_bg = nullptr;     // fondo precalculado

// ---------------- color ----------------
static inline u16 to565(u32 rgb) {
    return (u16)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}
static inline u16 blend(u16 dst, u32 rgb, u32 a) {
    if (a >= 255) return to565(rgb);
    if (a == 0) return dst;
    u32 dr = (dst >> 11) & 31, dg = (dst >> 5) & 63, db = dst & 31;
    u32 sr = (rgb >> 19) & 31, sg = (rgb >> 10) & 63, sb = (rgb >> 3) & 31;
    u32 r = (sr * a + dr * (255 - a)) / 255;
    u32 g = (sg * a + dg * (255 - a)) / 255;
    u32 b = (sb * a + db * (255 - a)) / 255;
    return (u16)((r << 11) | (g << 5) | b);
}
static u32 lerp_rgb(u32 a, u32 b, float t) {
    auto ch = [&](int s) {
        int x = (a >> s) & 255, y = (b >> s) & 255;
        return (u32)(x + (y - x) * t) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

// ---------------- fuente ----------------
struct Glyph { u32 cp; s16 xo, yo; u16 w, h, adv, pad; u32 off; };
struct Size { u16 px, asc, lh, count; const Glyph *g; };
static Size g_sizes[4];

static bool load_font() {
    const u8 *d = font_bin;
    if (font_bin_size < 8 || memcmp(d, "FNT1", 4)) return false;
    u32 n;
    memcpy(&n, d + 4, 4);
    size_t pos = 8;
    for (u32 i = 0; i < n && i < 4; i++) {
        memcpy(&g_sizes[i], d + pos, 8);
        g_sizes[i].g = (const Glyph *)(d + pos + 8);
        pos += 8 + sizeof(Glyph) * g_sizes[i].count;
    }
    return true;
}

static const Glyph *find(const Size &s, u32 cp) {
    for (u32 i = 0; i < s.count; i++)
        if (s.g[i].cp == cp) return &s.g[i];
    return nullptr;
}

static u32 next_cp(const char *&p) {
    const u8 *u = (const u8 *)p;
    u32 c = u[0];
    int n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { p++; return '?'; }
    p++;
    for (int i = 0; i < n; i++) {
        if ((((const u8 *)p)[0] & 0xC0) != 0x80) return '?';
        c = (c << 6) | (((const u8 *)p)[0] & 0x3F);
        p++;
    }
    return c;
}

static const Glyph *glyph(const Size &s, u32 cp) {
    const Glyph *g = find(s, cp);
    if (!g) g = find(s, '?');
    return g;
}

int line_h(Font f) { return g_sizes[f].lh; }

int text_w(Font f, const char *s) {
    const Size &sz = g_sizes[f];
    int w = 0;
    for (const char *p = s; *p;) {
        const Glyph *g = glyph(sz, next_cp(p));
        if (g) w += g->adv;
    }
    return w;
}

static void draw_glyph(const Glyph *g, int x, int y, u32 rgb) {
    const u8 *a = font_bin + g->off;
    for (int j = 0; j < g->h; j++) {
        int py = y + g->yo + j;
        if (py < 0 || py >= H) continue;
        u16 *row = g_px + py * g_stride;
        for (int i = 0; i < g->w; i++) {
            int px = x + g->xo + i;
            if (px < 0 || px >= W) continue;
            u8 al = a[j * g->w + i];
            if (al) row[px] = blend(row[px], rgb, al);
        }
    }
}

int text(Font f, int x, int y, u32 rgb, const char *s, int max_w) {
    if (!g_px) return 0;
    const Size &sz = g_sizes[f];
    std::string tmp;
    if (max_w > 0 && text_w(f, s) > max_w) {
        // recortar y poner "…"
        const char *ell = "\xE2\x80\xA6";
        int ew = text_w(f, ell);
        const char *p = s, *last = s;
        int w = 0;
        while (*p) {
            const char *q = p;
            const Glyph *g = glyph(sz, next_cp(q));
            int adv = g ? g->adv : 0;
            if (w + adv + ew > max_w) break;
            w += adv;
            p = q;
            last = p;
        }
        tmp.assign(s, last - s);
        tmp += ell;
        s = tmp.c_str();
    }
    int x0 = x;
    for (const char *p = s; *p;) {
        const Glyph *g = glyph(sz, next_cp(p));
        if (!g) continue;
        if (g->w) draw_glyph(g, x, y, rgb);
        x += g->adv;
    }
    return x - x0;
}

// ---------------- formas ----------------
void rect(int x, int y, int w, int h, u32 rgb, u8 alpha) {
    if (!g_px) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    u16 c = to565(rgb);
    for (int j = y; j < y1; j++) {
        u16 *row = g_px + j * g_stride;
        if (alpha == 255) for (int i = x; i < x1; i++) row[i] = c;
        else for (int i = x; i < x1; i++) row[i] = blend(row[i], rgb, alpha);
    }
}

// Cobertura (0..255) de un píxel respecto al rectángulo redondeado (antialias simple).
static inline int cover(int i, int j, int w, int h, int r) {
    int cx, cy;
    if (i < r) cx = r; else if (i >= w - r) cx = w - r - 1; else return 255;
    if (j < r) cy = r; else if (j >= h - r) cy = h - r - 1; else return 255;
    float dx = (float)(i - cx), dy = (float)(j - cy);
    float d = __builtin_sqrtf(dx * dx + dy * dy);
    float rr = (float)r;
    if (d <= rr - 1.0f) return 255;
    if (d >= rr) return 0;
    return (int)((rr - d) * 255);
}

static void rrect_impl(int x, int y, int w, int h, int r, u32 top, u32 bottom, u8 alpha, bool grad) {
    if (!g_px || w <= 0 || h <= 0) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= H) continue;
        u32 c = grad ? lerp_rgb(top, bottom, h > 1 ? (float)j / (h - 1) : 0) : top;
        u16 *row = g_px + py * g_stride;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= W) continue;
            int cv = cover(i, j, w, h, r);
            if (!cv) continue;
            row[px] = blend(row[px], c, cv * alpha / 255);
        }
    }
}

void round_rect(int x, int y, int w, int h, int r, u32 rgb, u8 alpha) { rrect_impl(x, y, w, h, r, rgb, rgb, alpha, false); }
void round_rect_v(int x, int y, int w, int h, int r, u32 top, u32 bottom) { rrect_impl(x, y, w, h, r, top, bottom, 255, true); }

// ---------------- ciclo ----------------
bool init() {
    if (!load_font()) return false;
    NWindow *win = nwindowGetDefault();
    if (R_FAILED(nwindowSetDimensions(win, W, H))) return false;
    if (R_FAILED(framebufferCreate(&g_fb, win, W, H, PIXEL_FORMAT_RGB_565, 2))) return false;
    if (R_FAILED(framebufferMakeLinear(&g_fb))) { framebufferClose(&g_fb); return false; }
    // Fondo: degradado azul con un brillo suave arriba a la izquierda.
    g_bg = (u16 *)malloc(W * H * sizeof(u16));
    if (!g_bg) { framebufferClose(&g_fb); return false; }
    for (int j = 0; j < H; j++) {
        u32 base = lerp_rgb(0x0D3B8C, 0x061A45, (float)j / (H - 1));
        for (int i = 0; i < W; i++) {
            float dx = (i - 200) / 900.0f, dy = (j - 60) / 600.0f;
            float glow = 1.0f - (dx * dx + dy * dy);
            u32 c = glow > 0 ? lerp_rgb(base, 0x1A5BC4, glow * 0.35f) : base;
            g_bg[j * W + i] = to565(c);
        }
    }
    g_ok = true;
    return true;
}

void exit() {
    if (!g_ok) return;
    framebufferClose(&g_fb);
    free(g_bg);
    g_bg = nullptr;
    g_ok = false;
}

void begin() {
    if (!g_ok) return;
    g_px = (u16 *)framebufferBegin(&g_fb, &g_stride);
    g_stride /= sizeof(u16);
    for (int j = 0; j < H; j++) memcpy(g_px + j * g_stride, g_bg + j * W, W * sizeof(u16));
}

void end() {
    if (!g_ok || !g_px) return;
    framebufferEnd(&g_fb);
    g_px = nullptr;
}

} // namespace gfx
