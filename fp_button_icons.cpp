/*
 * fp_button_icons.cpp -- PC button icons in the game's text.
 *
 * Fat Princess draws its button prompts ("Press X to join") as characters of
 * its fonts: U+2666 is the Cross button, U+25CB Circle, U+25A1 Square, U+2206
 * Triangle, U+2665/U+2660 L1/R1 and so on, each a small picture in the font's
 * texture. This file repaints those pictures, in the four fonts that have them,
 * as Xbox buttons or as the keyboard keys (and mouse buttons) the player has
 * bound, so every prompt in menus and in play shows what to press on a PC.
 *
 * fp_data_patches.cpp calls fp_button_icons_patch() for each font texture
 * entry; the result goes into the patched archive like any other data patch.
 *
 *   FP_BUTTON_ICONS  ps | xbox | keyboard | auto (keyboard, or xbox when an
 *                    XInput controller is plugged in at start-up)
 *   FP_BUTTON_KEYS   "cross=Space;circle=E;..." -- the first key bound to each
 *                    button, from [Controls] in fatprincess.ini
 *
 * The textures are DXT5 (BC3). Only the 4x4 blocks under a repainted glyph are
 * decoded, painted and encoded again; the rest of the texture is untouched.
 * Pictures are drawn at 4x and averaged down; letters come from GDI (Segoe UI
 * Bold), drawn as a white-on-black mask. Without GDI (the Linux test harness)
 * the icons are left as they are.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

enum Btn { CROSS, CIRCLE, SQUARE, TRIANGLE, L1, R1, L2, R2, L3, R3, HOME, DUP, DRIGHT, DDOWN, DLEFT, NBTN };
static const char* const kBtnKey[NBTN] = { "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2",
                                           "l3", "r3", "home", "dpadup", "dpadright", "dpaddown", "dpadleft" };

/* Glyph cells, from the fonts' .txt tables in siege.qrk (game v2.00). The
 * other characters these fonts map in the same range (U+263A.. in
 * FatFingersOutlined, for example) are game icons, not buttons. */
struct Cell { const char* tex; Btn b; int x, y, w, h; };
static const Cell kCells[] = {
    { "fatfingers", CROSS, 2, 195, 56, 56 },   { "fatfingers", CIRCLE, 58, 195, 56, 56 },
    { "fatfingers", SQUARE, 115, 195, 56, 56 }, { "fatfingers", TRIANGLE, 171, 195, 56, 56 },
    { "fatfingers", L1, 232, 195, 62, 56 },     { "fatfingers", R1, 300, 195, 62, 56 },
    { "fatfingers", L2, 368, 195, 63, 58 },     { "fatfingers", R2, 433, 195, 63, 58 },
    { "fatfingers", L3, 497, 195, 56, 56 },     { "fatfingers", R3, 554, 195, 56, 56 },
    { "fatfingers", HOME, 842, 195, 58, 56 },
    { "fatfingers", DUP, 725, 195, 52, 56 },    { "fatfingers", DRIGHT, 670, 195, 47, 56 },
    { "fatfingers", DDOWN, 779, 195, 51, 56 },  { "fatfingers", DLEFT, 622, 195, 46, 56 },

    { "fatfingersoutlined", CROSS, 2, 195, 56, 56 },   { "fatfingersoutlined", CIRCLE, 58, 195, 56, 56 },
    { "fatfingersoutlined", SQUARE, 115, 195, 56, 56 }, { "fatfingersoutlined", TRIANGLE, 171, 195, 56, 56 },
    { "fatfingersoutlined", L1, 232, 195, 62, 56 },     { "fatfingersoutlined", R1, 300, 195, 62, 56 },
    { "fatfingersoutlined", DUP, 725, 195, 52, 56 },    { "fatfingersoutlined", DRIGHT, 670, 195, 47, 56 },
    { "fatfingersoutlined", DDOWN, 779, 195, 51, 56 },  { "fatfingersoutlined", DLEFT, 622, 195, 46, 56 },

    { "boopee", CROSS, 1, 188, 63, 63 },   { "boopee", CIRCLE, 64, 188, 63, 63 },
    { "boopee", SQUARE, 127, 188, 63, 63 }, { "boopee", TRIANGLE, 190, 188, 63, 63 },
    { "boopee", L1, 256, 188, 72, 63 },     { "boopee", R1, 332, 188, 72, 63 },

    { "calibri", CROSS, 0, 214, 42, 42 },   { "calibri", CIRCLE, 42, 214, 42, 42 },
    { "calibri", SQUARE, 84, 214, 42, 42 }, { "calibri", TRIANGLE, 126, 214, 42, 42 },
};

/* ---- DXT5 (BC3) blocks ------------------------------------------------------------ */
static void c565(uint16_t c, int o[3])
{
    o[0] = ((c >> 11) & 31) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31;
}
static void bc3_decode(const uint8_t* b, uint8_t out[16][4])
{
    const int a0 = b[0], a1 = b[1];
    int al[8] = { a0, a1 };
    if (a0 > a1) for (int i = 0; i < 6; i++) al[2 + i] = (a0 * (6 - i) + a1 * (i + 1)) / 7;
    else { for (int i = 0; i < 4; i++) al[2 + i] = (a0 * (4 - i) + a1 * (i + 1)) / 5; al[6] = 0; al[7] = 255; }
    uint64_t ab = 0;
    for (int i = 0; i < 6; i++) ab |= (uint64_t)b[2 + i] << (8 * i);
    int c[4][3];
    c565((uint16_t)(b[8] | (b[9] << 8)), c[0]); c565((uint16_t)(b[10] | (b[11] << 8)), c[1]);
    for (int k = 0; k < 3; k++) { c[2][k] = (2 * c[0][k] + c[1][k]) / 3; c[3][k] = (c[0][k] + 2 * c[1][k]) / 3; }
    const uint32_t bits = b[12] | (b[13] << 8) | (b[14] << 16) | ((uint32_t)b[15] << 24);
    for (int i = 0; i < 16; i++) {
        const int ci = (bits >> (2 * i)) & 3;
        out[i][0] = (uint8_t)c[ci][0]; out[i][1] = (uint8_t)c[ci][1]; out[i][2] = (uint8_t)c[ci][2];
        out[i][3] = (uint8_t)al[(ab >> (3 * i)) & 7];
    }
}
static uint16_t to565(const float c[3])
{
    auto q = [](float v, int m) { int x = (int)lrintf(v * m / 255.0f); return x < 0 ? 0 : x > m ? m : x; };
    return (uint16_t)((q(c[0], 31) << 11) | (q(c[1], 63) << 5) | q(c[2], 31));
}
static void bc3_encode(const uint8_t px[16][4], uint8_t* b)
{
    /* Alpha: the block's range in eight steps. */
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; i++) { if (px[i][3] < amin) amin = px[i][3]; if (px[i][3] > amax) amax = px[i][3]; }
    b[0] = (uint8_t)amax; b[1] = (uint8_t)amin;
    int al[8] = { amax, amin };
    if (amax > amin) for (int i = 0; i < 6; i++) al[2 + i] = (amax * (6 - i) + amin * (i + 1)) / 7;
    else for (int i = 2; i < 8; i++) al[i] = amax;
    uint64_t ab = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; k++) { const int d = abs(al[k] - px[i][3]); if (d < bd) { bd = d; best = k; } }
        ab |= (uint64_t)best << (3 * i);
    }
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(ab >> (8 * i));

    /* Colour: the end points along the main axis of the visible pixels. */
    float mean[3] = { 0, 0, 0 }, wsum = 0;
    for (int i = 0; i < 16; i++) {
        const float w = px[i][3] > 8 ? 1.0f : 0.02f;
        for (int k = 0; k < 3; k++) mean[k] += w * px[i][k];
        wsum += w;
    }
    for (int k = 0; k < 3; k++) mean[k] /= wsum;
    float cov[6] = { 0 };
    for (int i = 0; i < 16; i++) {
        const float w = px[i][3] > 8 ? 1.0f : 0.02f;
        const float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
        cov[0] += w * d[0] * d[0]; cov[1] += w * d[0] * d[1]; cov[2] += w * d[0] * d[2];
        cov[3] += w * d[1] * d[1]; cov[4] += w * d[1] * d[2]; cov[5] += w * d[2] * d[2];
    }
    float ax[3] = { 0.577f, 0.577f, 0.577f };
    for (int it = 0; it < 8; it++) {
        const float n[3] = { cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2],
                             cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2],
                             cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2] };
        const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-6f) break;
        for (int k = 0; k < 3; k++) ax[k] = n[k] / l;
    }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 16; i++) {
        if (px[i][3] <= 8 && amax > 8) continue;
        const float t = (px[i][0] - mean[0]) * ax[0] + (px[i][1] - mean[1]) * ax[1] + (px[i][2] - mean[2]) * ax[2];
        if (t < lo) lo = t;
        if (t > hi) hi = t;
    }
    if (lo > hi) lo = hi = 0;
    float e0[3], e1[3];
    for (int k = 0; k < 3; k++) { e0[k] = mean[k] + ax[k] * hi; e1[k] = mean[k] + ax[k] * lo; }
    uint16_t c0 = to565(e0), c1 = to565(e1);
    if (c0 < c1) { const uint16_t t = c0; c0 = c1; c1 = t; }
    int pal[4][3];
    c565(c0, pal[0]); c565(c1, pal[1]);
    for (int k = 0; k < 3; k++) { pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3; pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3; }
    uint32_t bits = 0;
    if (c0 != c1)
        for (int i = 0; i < 16; i++) {
            int best = 0, bd = 1 << 30;
            for (int k = 0; k < 4; k++) {
                const int d0 = pal[k][0] - px[i][0], d1 = pal[k][1] - px[i][1], d2 = pal[k][2] - px[i][2];
                const int d = d0 * d0 * 3 + d1 * d1 * 4 + d2 * d2 * 2;
                if (d < bd) { bd = d; best = k; }
            }
            bits |= (uint32_t)best << (2 * i);
        }
    b[8] = (uint8_t)c0; b[9] = (uint8_t)(c0 >> 8); b[10] = (uint8_t)c1; b[11] = (uint8_t)(c1 >> 8);
    b[12] = (uint8_t)bits; b[13] = (uint8_t)(bits >> 8); b[14] = (uint8_t)(bits >> 16); b[15] = (uint8_t)(bits >> 24);
}

/* ---- drawing ----------------------------------------------------------------------- */
static constexpr int SS = 4;   /* supersampling per axis */

struct Col { float r, g, b, a; };
static Col rgb(uint32_t c, float a = 1.0f) { return { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a }; }
static Col mix(Col a, Col b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t }; }

/* A picture of w x h output pixels, drawn at SS x SS per pixel; premultiplied. */
struct Canvas {
    int w, h, W, H;
    std::vector<float> p;
    Canvas(int w_, int h_) : w(w_), h(h_), W(w_ * SS), H(h_ * SS), p((size_t)W * H * 4, 0.0f) {}
    void over(int x, int y, Col c, float cov)
    {
        if (x < 0 || y < 0 || x >= W || y >= H || cov <= 0) return;
        const float a = c.a * (cov > 1 ? 1 : cov);
        float* d = &p[((size_t)y * W + x) * 4];
        d[0] = c.r * a + d[0] * (1 - a); d[1] = c.g * a + d[1] * (1 - a);
        d[2] = c.b * a + d[2] * (1 - a); d[3] = a + d[3] * (1 - a);
    }
    /* Rounded rectangle in output-pixel units, filled top-to-bottom c0 -> c1. */
    void rrect(float x0, float y0, float x1, float y1, float rad, Col c0, Col c1)
    {
        const float r = rad < 0 ? 0 : rad;
        for (int Y = (int)floorf(y0 * SS); Y < (int)ceilf(y1 * SS); Y++)
            for (int X = (int)floorf(x0 * SS); X < (int)ceilf(x1 * SS); X++) {
                const float x = (X + 0.5f) / SS, y = (Y + 0.5f) / SS;
                if (x < x0 || x > x1 || y < y0 || y > y1) continue;
                const float qx = fmaxf(fmaxf(x0 + r - x, x - (x1 - r)), 0.0f), qy = fmaxf(fmaxf(y0 + r - y, y - (y1 - r)), 0.0f);
                if (qx * qx + qy * qy > r * r) continue;
                over(X, Y, mix(c0, c1, (y - y0) / fmaxf(y1 - y0, 1e-3f)), 1.0f);
            }
    }
    void disc(float cx, float cy, float rad, Col c0, Col c1)
    {
        rrect(cx - rad, cy - rad, cx + rad, cy + rad, rad, c0, c1);
    }
    void ring(float cx, float cy, float r0, float r1, Col c)
    {
        for (int Y = (int)((cy - r1) * SS); Y <= (int)((cy + r1) * SS); Y++)
            for (int X = (int)((cx - r1) * SS); X <= (int)((cx + r1) * SS); X++) {
                const float x = (X + 0.5f) / SS - cx, y = (Y + 0.5f) / SS - cy, d = sqrtf(x * x + y * y);
                if (d >= r0 && d <= r1) over(X, Y, c, 1.0f);
            }
    }
    /* A line of the given width with round ends. */
    void line(float ax, float ay, float bx, float by, float wd, Col c)
    {
        const float r = wd / 2;
        for (int Y = (int)((fminf(ay, by) - r) * SS); Y <= (int)((fmaxf(ay, by) + r) * SS); Y++)
            for (int X = (int)((fminf(ax, bx) - r) * SS); X <= (int)((fmaxf(ax, bx) + r) * SS); X++) {
                const float x = (X + 0.5f) / SS, y = (Y + 0.5f) / SS;
                const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
                float t = l2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0;
                t = fminf(fmaxf(t, 0), 1);
                const float ex = x - (ax + t * dx), ey = y - (ay + t * dy);
                if (ex * ex + ey * ey <= r * r) over(X, Y, c, 1.0f);
            }
    }
    void tri(float ax, float ay, float bx, float by, float cx, float cy, Col c)
    {
        for (int Y = (int)(fminf(ay, fminf(by, cy)) * SS); Y <= (int)(fmaxf(ay, fmaxf(by, cy)) * SS); Y++)
            for (int X = (int)(fminf(ax, fminf(bx, cx)) * SS); X <= (int)(fmaxf(ax, fmaxf(bx, cx)) * SS); X++) {
                const float x = (X + 0.5f) / SS, y = (Y + 0.5f) / SS;
                const float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
                const float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
                const float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
                const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
                if (!(neg && pos)) over(X, Y, c, 1.0f);
            }
    }
    /* A coverage mask (W x H, 0..1) in one colour. */
    void mask(const std::vector<float>& m, Col c)
    {
        for (int i = 0; i < W * H; i++) if (m[i] > 0) over(i % W, i / W, c, m[i]);
    }
    /* Average down to straight-alpha RGBA bytes. */
    void resolve(std::vector<uint8_t>& out) const
    {
        out.assign((size_t)w * h * 4, 0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float s[4] = { 0, 0, 0, 0 };
                for (int j = 0; j < SS; j++)
                    for (int i = 0; i < SS; i++) {
                        const float* d = &p[((size_t)(y * SS + j) * W + x * SS + i) * 4];
                        for (int k = 0; k < 4; k++) s[k] += d[k];
                    }
                for (int k = 0; k < 4; k++) s[k] /= SS * SS;
                uint8_t* o = &out[((size_t)y * w + x) * 4];
                const float a = s[3];
                for (int k = 0; k < 3; k++) { const float v = a > 1e-4f ? s[k] / a : 0; o[k] = (uint8_t)lrintf(fminf(fmaxf(v, 0), 1) * 255); }
                o[3] = (uint8_t)lrintf(fminf(fmaxf(a, 0), 1) * 255);
            }
    }
};

/* Text as a coverage mask over the whole canvas: capital letters maxh tall
 * (smaller if the text would be wider than maxw), centred on (cx, cy) with
 * the capital height as the vertical box, so "-" or "," stay small. */
static bool text_mask(const Canvas& cv, const std::wstring& s, float cx, float cy, float maxw, float maxh,
                      std::vector<float>& m)
{
    m.assign((size_t)cv.W * cv.H, 0.0f);
#ifdef _WIN32
    const int TW = cv.W * 3, TH = cv.H * 3;   /* room for a first, too-big try */
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = TW; bi.bmiHeader.biHeight = -TH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dc || !bm || !bits) { if (bm) DeleteObject(bm); if (dc) DeleteDC(dc); return false; }
    HGDIOBJ oldb = SelectObject(dc, bm);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    const uint32_t* px = (const uint32_t*)bits;
    struct Box { int x0, y0, x1, y1; };
    auto draw = [&](const std::wstring& t, int fh) {
        memset(bits, 0, (size_t)TW * TH * 4);
        HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldf = SelectObject(dc, f);
        TextOutW(dc, 4, 4, t.c_str(), (int)t.size());
        GdiFlush();
        SelectObject(dc, oldf); DeleteObject(f);
        Box b = { TW, TH, -1, -1 };
        for (int y = 0; y < TH; y++)
            for (int x = 0; x < TW; x++)
                if (px[(size_t)y * TW + x] & 0xFF00) {
                    b.x0 = x < b.x0 ? x : b.x0; b.x1 = x > b.x1 ? x : b.x1;
                    b.y0 = y < b.y0 ? y : b.y0; b.y1 = y > b.y1 ? y : b.y1;
                }
        return b;
    };
    /* Font size from the height of "H", then narrower if needed. */
    int fh = (int)(maxh * SS * 1.5f);
    Box cap = draw(L"H", fh);
    bool ok = cap.x1 >= 0;
    if (ok) {
        fh = (int)(fh * (maxh * SS) / (float)(cap.y1 - cap.y0 + 1));
        Box ink = draw(s, fh);
        if (ink.x1 >= 0 && ink.x1 - ink.x0 + 1 > maxw * SS) fh = (int)(fh * (maxw * SS) / (float)(ink.x1 - ink.x0 + 1));
        if (fh < 4) fh = 4;
        cap = draw(L"H", fh);
        ink = draw(s, fh);
        ok = ink.x1 >= 0 && cap.x1 >= 0;
        if (ok) {
            const int ox = (int)lrintf(cx * SS - (ink.x0 + ink.x1 + 1) / 2.0f);
            const int oy = (int)lrintf(cy * SS - (cap.y0 + cap.y1 + 1) / 2.0f);
            for (int y = ink.y0; y <= ink.y1; y++)
                for (int x = ink.x0; x <= ink.x1; x++) {
                    const int X = ox + x, Y = oy + y;
                    if (X >= 0 && Y >= 0 && X < cv.W && Y < cv.H)
                        m[(size_t)Y * cv.W + X] = ((px[(size_t)y * TW + x] >> 8) & 255) / 255.0f;
                }
        }
    }
    SelectObject(dc, oldb); DeleteObject(bm); DeleteDC(dc);
    return ok;
#else
    (void)s; (void)cx; (void)cy; (void)maxw; (void)maxh;
    return false;
#endif
}

/* ---- the icons ---------------------------------------------------------------------- */
struct Style { int kind; std::string keys[NBTN]; };   /* kind: 1 xbox, 2 keyboard */

static std::wstring widen(const std::string& s)
{
    std::wstring w;
    for (unsigned char c : s) w += (wchar_t)c;
    return w;
}

/* A short label for a key name from fatprincess.ini. */
static std::wstring key_label(const std::string& name)
{
    std::string n;
    for (char c : name) n += (char)tolower((unsigned char)c);
    static const struct { const char* k; const wchar_t* l; } t[] = {
        { "space", L"Space" }, { "enter", L"Enter" }, { "return", L"Enter" }, { "tab", L"Tab" },
        { "escape", L"Esc" }, { "esc", L"Esc" }, { "backspace", L"Bksp" }, { "shift", L"Shift" },
        { "lshift", L"Shift" }, { "rshift", L"Shift" }, { "ctrl", L"Ctrl" }, { "control", L"Ctrl" },
        { "lctrl", L"Ctrl" }, { "rctrl", L"Ctrl" }, { "alt", L"Alt" }, { "lalt", L"Alt" }, { "ralt", L"Alt" },
        { "up", L"\x2191" }, { "down", L"\x2193" }, { "left", L"\x2190" }, { "right", L"\x2192" },
        { "insert", L"Ins" }, { "delete", L"Del" }, { "home", L"Home" }, { "end", L"End" },
        { "pageup", L"PgUp" }, { "pagedown", L"PgDn" }, { "capslock", L"Caps" },
        { "comma", L"," }, { "period", L"." }, { "slash", L"/" }, { "semicolon", L";" }, { "quote", L"'" },
        { "lbracket", L"[" }, { "rbracket", L"]" }, { "minus", L"-" }, { "equals", L"=" },
        { "backslash", L"\\" }, { "grave", L"`" }, { "mouse4", L"M4" }, { "mouse5", L"M5" },
        { "none", L"\x2013" }, { "", L"\x2013" },
    };
    for (const auto& e : t) if (n == e.k) return e.l;
    if (n.size() > 6 && n.compare(0, 6, "numpad") == 0) return L"Num" + widen(n.substr(6));
    std::string u = name;
    for (char& c : u) c = (char)toupper((unsigned char)c);
    if (u.size() == 1) return widen(u);
    if (u.size() >= 2 && u[0] == 'F' && isdigit((unsigned char)u[1])) return widen(u);
    return widen(name);
}

/* The PS picture's own extent inside its cell, so a new icon has its size. */
static void ink_box(const std::vector<uint8_t>& rgba, int w, int h, float& x0, float& y0, float& x1, float& y1)
{
    int a = w, b = h, c = -1, d = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (rgba[((size_t)y * w + x) * 4 + 3] > 48) { if (x < a) a = x; if (x > c) c = x; if (y < b) b = y; if (y > d) d = y; }
    if (c < 0) { a = 1; b = 1; c = w - 2; d = h - 2; }
    x0 = (float)a; y0 = (float)b; x1 = (float)c + 1; y1 = (float)d + 1;
}

static const Col kDarkTop = rgb(0x4A4A4E), kDarkBot = rgb(0x1C1C1F), kEdge = rgb(0x0B0B0C);

static void draw_xbox(Canvas& cv, Btn b, float x0, float y0, float x1, float y1)
{
    const float w = x1 - x0, h = y1 - y0, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    std::vector<float> m;
    if (b <= TRIANGLE || b == L3 || b == R3 || b == HOME) {
        const float r = fminf(w, h) / 2;
        cv.disc(cx, cy, r, kEdge, kEdge);
        cv.disc(cx, cy, r - 2.2f, kDarkTop, kDarkBot);
        cv.rrect(cx - r * 0.62f, cy - r * 0.86f, cx + r * 0.62f, cy - r * 0.18f, r * 0.34f, rgb(0xFFFFFF, 0.16f), rgb(0xFFFFFF, 0.0f));
        if (b == HOME) { cv.ring(cx, cy, r * 0.42f, r * 0.58f, rgb(0xF2F2F2)); return; }
        static const uint32_t col[4] = { 0x6CC72C, 0xE8403F, 0x3A86EA, 0xF4C522 };   /* A B X Y */
        static const wchar_t* let[4] = { L"A", L"B", L"X", L"Y" };
        const bool stick = b == L3 || b == R3;
        if (text_mask(cv, stick ? (b == L3 ? L"LS" : L"RS") : let[b], cx, cy + r * 0.02f,
                      r * (stick ? 1.25f : 1.1f), r * (stick ? 0.78f : 0.98f), m))
            cv.mask(m, stick ? rgb(0xF2F2F2) : rgb(col[b]));
        return;
    }
    /* Bumpers and triggers: a dark pill with LB / RB / LT / RT. */
    const float ph = fminf(h, w * 0.78f), py0 = cy - ph / 2, py1 = cy + ph / 2;
    const bool trig = b == L2 || b == R2;
    cv.rrect(x0, py0, x1, py1, trig ? ph * 0.30f : ph * 0.48f, kEdge, kEdge);
    cv.rrect(x0 + 2.2f, py0 + 2.2f, x1 - 2.2f, py1 - 2.2f, trig ? ph * 0.26f : ph * 0.44f, kDarkTop, kDarkBot);
    const wchar_t* t = b == L1 ? L"LB" : b == R1 ? L"RB" : b == L2 ? L"LT" : L"RT";
    if (text_mask(cv, t, cx, cy, w * 0.66f, ph * 0.50f, m)) cv.mask(m, rgb(0xF2F2F2));
}

static void draw_mouse(Canvas& cv, int which, float x0, float y0, float x1, float y1)
{
    /* which: 0 left, 1 right, 2 middle */
    const float h = (y1 - y0), w = fminf(x1 - x0, h * 0.70f), cx = (x0 + x1) / 2;
    const float mx0 = cx - w / 2, mx1 = cx + w / 2, my0 = y0, my1 = y1, split = my0 + h * 0.44f;
    cv.rrect(mx0, my0, mx1, my1, w * 0.48f, kEdge, kEdge);
    cv.rrect(mx0 + 2, my0 + 2, mx1 - 2, my1 - 2, w * 0.44f, rgb(0xF6F6F6), rgb(0xCFCFD2));
    const Col hot = rgb(0xF5A623);
    if (which == 0) cv.rrect(mx0 + 2, my0 + 2, cx - 0.8f, split, w * 0.40f, hot, rgb(0xE08A10));
    if (which == 1) cv.rrect(cx + 0.8f, my0 + 2, mx1 - 2, split, w * 0.40f, hot, rgb(0xE08A10));
    cv.rrect(mx0 + 2, split - 0.8f, mx1 - 2, split + 0.8f, 0, kEdge, kEdge);                   /* across */
    cv.rrect(cx - 0.8f, my0 + 2, cx + 0.8f, split, 0, kEdge, kEdge);                          /* between */
    const float ww = w * 0.16f;                                                               /* wheel */
    cv.rrect(cx - ww, my0 + h * 0.12f, cx + ww, my0 + h * 0.34f, ww, kEdge, kEdge);
    cv.rrect(cx - ww + 1, my0 + h * 0.12f + 1, cx + ww - 1, my0 + h * 0.34f - 1, ww - 1,
             which == 2 ? hot : rgb(0x8A8A90), which == 2 ? rgb(0xE08A10) : rgb(0x5A5A60));
}

/* Keys shown as a symbol rather than a word, drawn in the key top's box. */
static bool key_symbol(Canvas& cv, const std::string& n, float x0, float y0, float x1, float y1)
{
    const Col ink = rgb(0x1E1E22);
    const float w = x1 - x0, h = y1 - y0, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const float s = fminf(w, h), lw = fmaxf(s * 0.11f, 2.2f);
    if (n == "space") {                                  /* an open box: the space bar's mark */
        const float l = cx - w * 0.30f, r = cx + w * 0.30f, t = cy - h * 0.06f, b = cy + h * 0.20f;
        cv.line(l, t, l, b, lw, ink); cv.line(l, b, r, b, lw, ink); cv.line(r, b, r, t, lw, ink);
        return true;
    }
    if (n == "enter" || n == "return") {                 /* a return arrow */
        const float r = cx + s * 0.26f, l = cx - s * 0.24f, t = cy - s * 0.22f, m = cy + s * 0.10f;
        cv.line(r, t, r, m, lw, ink); cv.line(r, m, l + s * 0.10f, m, lw, ink);
        cv.tri(l - s * 0.04f, m, l + s * 0.16f, m - s * 0.15f, l + s * 0.16f, m + s * 0.15f, ink);
        return true;
    }
    if (n == "shift" || n == "lshift" || n == "rshift") { /* an outlined up arrow */
        const float t = cy - s * 0.28f, mid = cy, b = cy + s * 0.26f, hw = s * 0.27f, sw = s * 0.11f;
        cv.line(cx, t, cx - hw, mid, lw * 0.8f, ink); cv.line(cx, t, cx + hw, mid, lw * 0.8f, ink);
        cv.line(cx - hw, mid, cx - sw, mid, lw * 0.8f, ink); cv.line(cx + hw, mid, cx + sw, mid, lw * 0.8f, ink);
        cv.line(cx - sw, mid, cx - sw, b, lw * 0.8f, ink); cv.line(cx + sw, mid, cx + sw, b, lw * 0.8f, ink);
        cv.line(cx - sw, b, cx + sw, b, lw * 0.8f, ink);
        return true;
    }
    if (n == "tab") {                                    /* arrow to a bar */
        const float l = cx - s * 0.28f, r = cx + s * 0.22f;
        cv.line(l, cy, r - s * 0.12f, cy, lw, ink);
        cv.tri(r + s * 0.02f, cy, r - s * 0.16f, cy - s * 0.15f, r - s * 0.16f, cy + s * 0.15f, ink);
        cv.line(r + s * 0.08f, cy - s * 0.20f, r + s * 0.08f, cy + s * 0.20f, lw, ink);
        return true;
    }
    if (n == "backspace") {                              /* arrow pointing left */
        const float l = cx - s * 0.28f, r = cx + s * 0.28f;
        cv.line(l + s * 0.14f, cy, r, cy, lw, ink);
        cv.tri(l - s * 0.02f, cy, l + s * 0.18f, cy - s * 0.16f, l + s * 0.18f, cy + s * 0.16f, ink);
        return true;
    }
    if (n == "up" || n == "down" || n == "left" || n == "right") {
        const float a = s * 0.24f;
        if (n == "up")    cv.tri(cx, cy - a, cx - a, cy + a * 0.75f, cx + a, cy + a * 0.75f, ink);
        if (n == "down")  cv.tri(cx, cy + a, cx - a, cy - a * 0.75f, cx + a, cy - a * 0.75f, ink);
        if (n == "left")  cv.tri(cx - a, cy, cx + a * 0.75f, cy - a, cx + a * 0.75f, cy + a, ink);
        if (n == "right") cv.tri(cx + a, cy, cx - a * 0.75f, cy - a, cx - a * 0.75f, cy + a, ink);
        return true;
    }
    return false;
}

static void draw_key(Canvas& cv, const std::string& key, float x0, float y0, float x1, float y1)
{
    std::string n;
    for (char c : key) n += (char)tolower((unsigned char)c);
    if (n == "mouseleft" || n == "mouseright" || n == "mousemiddle") {
        /* The mouse gets the cell's full height: a bumper-shaped picture is low. */
        const float my0 = fminf(y0, 1.0f), my1 = fmaxf(y1, cv.h - 1.0f);
        draw_mouse(cv, n == "mouseleft" ? 0 : n == "mouseright" ? 1 : 2, x0, my0, x1, my1);
        return;
    }
    /* Square-ish key, at most as wide as it is tall plus a little. */
    float w = x1 - x0, h = y1 - y0;
    if (w > h * 1.15f) { const float c = (x0 + x1) / 2; w = h * 1.15f; x0 = c - w / 2; x1 = c + w / 2; }
    const float rad = fminf(w, h) * 0.20f;
    cv.rrect(x0, y0, x1, y1, rad, kEdge, kEdge);
    cv.rrect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, rad * 0.8f, rgb(0xA9A9AE), rgb(0x7D7D84));          /* the key's sides */
    const float tx0 = x0 + 3.5f, ty0 = y0 + 2.5f, tx1 = x1 - 3.5f, ty1 = y1 - h * 0.16f;
    cv.rrect(tx0, ty0, tx1, ty1, rad * 0.7f, rgb(0xFBFBFB), rgb(0xDADADE));                     /* its top */
    if (key_symbol(cv, n, tx0, ty0, tx1, ty1)) return;
    const std::wstring l = key_label(key);
    std::vector<float> m;
    const float th = (ty1 - ty0) * (l.size() <= 1 ? 0.52f : l.size() <= 3 ? 0.36f : 0.30f);
    if (text_mask(cv, l, (tx0 + tx1) / 2, (ty0 + ty1) / 2, (tx1 - tx0) - 4, th, m)) cv.mask(m, rgb(0x1E1E22));
}

static bool style_from_env(Style& st)
{
    const char* e = getenv("FP_BUTTON_ICONS");
    std::string s = e ? e : "ps";
    for (char& c : s) c = (char)tolower((unsigned char)c);
    if (s == "auto") {
        s = "keyboard";
#ifdef _WIN32
        typedef DWORD (WINAPI *XGetState)(DWORD, void*);
        for (const char* dll : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" }) {
            HMODULE h = LoadLibraryA(dll);
            if (!h) continue;
            XGetState get = (XGetState)(void*)GetProcAddress(h, "XInputGetState");
            uint8_t state[16];
            for (DWORD i = 0; get && i < 4; i++) if (get(i, state) == ERROR_SUCCESS) { s = "xbox"; break; }
            FreeLibrary(h);
            break;
        }
#endif
        fprintf(stderr, "[icons] auto: %s\n", s.c_str());
    }
    st.kind = s == "xbox" ? 1 : s == "keyboard" || s == "keys" ? 2 : 0;
    if (const char* k = getenv("FP_BUTTON_KEYS")) {
        std::string t = k;
        size_t p = 0;
        while (p < t.size()) {
            size_t q = t.find(';', p);
            if (q == std::string::npos) q = t.size();
            const std::string kv = t.substr(p, q - p);
            const size_t eq = kv.find('=');
            if (eq != std::string::npos)
                for (int b = 0; b < NBTN; b++) if (kv.compare(0, eq, kBtnKey[b]) == 0) st.keys[b] = kv.substr(eq + 1);
            p = q + 1;
        }
    }
    return st.kind != 0;
}

} // namespace

/* What fp_data_patches puts in its cache key: the style and the key labels, so
 * the patched archive is rebuilt when either changes. Empty = no icon patch. */
std::string fp_button_icons_id()
{
    Style st;
    if (!style_from_env(st)) return std::string();
#ifndef _WIN32
    return std::string();   /* no GDI for the letters: keep the PS pictures */
#endif
    std::string id = st.kind == 1 ? "icons-xbox-1" : "icons-keys-1";
    if (st.kind == 2) {
        /* The keys as a short hash: the id goes into the cache key and the log. */
        uint64_t h = 1469598103934665603ull;
        for (const char* k = getenv("FP_BUTTON_KEYS"); k && *k; k++) h = (h ^ (uint8_t)*k) * 1099511628211ull;
        char hx[24];
        snprintf(hx, sizeof hx, "-%016llx", (unsigned long long)h);
        id += hx;
    }
    return id;
}

/* Repaint the button glyphs of one font texture (its archive entry name, e.g.
 * data/textures/fonts/fatfingers.gtf). False when there is nothing to do. */
bool fp_button_icons_patch(const std::string& entry, std::string& gtf)
{
    Style st;
    if (!style_from_env(st)) return false;
    std::string tex = entry.substr(entry.find_last_of('/') + 1);
    tex = tex.substr(0, tex.find('.'));
    if (gtf.size() < 0x80 || (uint8_t)gtf[0x18] != 0x88) return false;   /* DXT5 only */
    const uint8_t* hd = (const uint8_t*)gtf.data();
    const int W = (hd[0x20] << 8) | hd[0x21], H = (hd[0x22] << 8) | hd[0x23];
    if ((size_t)W * H + 0x80 > gtf.size() || (W & 3) || (H & 3)) return false;
    const int bw = W / 4;
    uint8_t* blocks = (uint8_t*)&gtf[0x80];
    int done = 0;
    for (const Cell& c : kCells) {
        if (tex != c.tex) continue;
        if (st.kind == 1 && c.b >= DUP) continue;                 /* d-pad arrows read fine as they are */
        if (st.kind == 2 && c.b == HOME) continue;
        if (c.x + c.w > W || c.y + c.h > H) continue;
        /* Decode the blocks under the cell. */
        const int bx0 = c.x / 4, by0 = c.y / 4, bx1 = (c.x + c.w + 3) / 4, by1 = (c.y + c.h + 3) / 4;
        const int rw = (bx1 - bx0) * 4, rh = (by1 - by0) * 4;
        std::vector<uint8_t> reg((size_t)rw * rh * 4);
        for (int by = by0; by < by1; by++)
            for (int bx = bx0; bx < bx1; bx++) {
                uint8_t px[16][4];
                bc3_decode(blocks + ((size_t)by * bw + bx) * 16, px);
                for (int i = 0; i < 16; i++)
                    memcpy(&reg[((size_t)((by - by0) * 4 + i / 4) * rw + (bx - bx0) * 4 + i % 4) * 4], px[i], 4);
            }
        /* The old picture: its extent, then cleared. */
        const int ox = c.x - bx0 * 4, oy = c.y - by0 * 4;
        std::vector<uint8_t> cell((size_t)c.w * c.h * 4);
        for (int y = 0; y < c.h; y++) memcpy(&cell[(size_t)y * c.w * 4], &reg[((size_t)(oy + y) * rw + ox) * 4], (size_t)c.w * 4);
        float x0, y0, x1, y1;
        ink_box(cell, c.w, c.h, x0, y0, x1, y1);
        Canvas cv(c.w, c.h);
        if (st.kind == 1) draw_xbox(cv, c.b, x0, y0, x1, y1);
        else {
            /* The arrows are used for menu directions: show the left-stick
             * keys (what moves through menus) when they are bound. */
            static const char* const ls[4] = { "lsup", "lsright", "lsdown", "lsleft" };
            std::string k = st.keys[c.b];
            if (c.b >= DUP) {
                const std::string want = ls[c.b - DUP];
                const std::string t = getenv("FP_BUTTON_KEYS") ? getenv("FP_BUTTON_KEYS") : "";
                const size_t at = (";" + t).find(";" + want + "=");
                if (at != std::string::npos) {
                    const size_t v = at + want.size() + 1, e = t.find(';', v);
                    const std::string key = t.substr(v, e == std::string::npos ? std::string::npos : e - v);
                    std::string lk; for (char ch : key) lk += (char)tolower((unsigned char)ch);
                    if (!key.empty() && lk != "none") k = key;
                }
            }
            draw_key(cv, k.empty() ? std::string("None") : k, x0, y0, x1, y1);
        }
        std::vector<uint8_t> out;
        cv.resolve(out);
        for (int y = 0; y < c.h; y++) memcpy(&reg[((size_t)(oy + y) * rw + ox) * 4], &out[(size_t)y * c.w * 4], (size_t)c.w * 4);
        /* Encode the blocks again. */
        for (int by = by0; by < by1; by++)
            for (int bx = bx0; bx < bx1; bx++) {
                uint8_t px[16][4];
                for (int i = 0; i < 16; i++)
                    memcpy(px[i], &reg[((size_t)((by - by0) * 4 + i / 4) * rw + (bx - bx0) * 4 + i % 4) * 4], 4);
                bc3_encode(px, blocks + ((size_t)by * bw + bx) * 16);
            }
        done++;
    }
    if (done) fprintf(stderr, "[icons] %s: %d button picture(s) repainted (%s)\n", tex.c_str(), done,
                      st.kind == 1 ? "Xbox" : "keyboard");
    return done > 0;
}

/* The font textures that have button pictures. */
const char* const* fp_button_icons_entries()
{
    static const char* const e[] = { "data/textures/fonts/fatfingers.gtf", "data/textures/fonts/fatfingersoutlined.gtf",
                                     "data/textures/fonts/boopee.gtf", "data/textures/fonts/calibri.gtf", nullptr };
    return e;
}

#ifdef FP_BUTTON_ICONS_TEST
/* Preview: fp_icons_test <in.gtf> <entry name> <out.gtf> -- under Wine. */
int main(int argc, char** argv)
{
    if (argc < 4) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::string d; char b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) d.append(b, n);
    fclose(f);
    const bool ok = fp_button_icons_patch(argv[2], d);
    FILE* o = fopen(argv[3], "wb");
    fwrite(d.data(), 1, d.size(), o); fclose(o);
    printf("patched=%d\n", ok);
    return 0;
}
#endif
