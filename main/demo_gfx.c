/* demo_gfx.c — voir demo_gfx.h */
#include "demo_gfx.h"
#include <string.h>

static uint32_t s_rng = 0x1234567;
void g_seed(uint32_t s) { s_rng = s ? s : 0x1234567; }
uint32_t g_rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
float g_frnd(void) { return (g_rnd() & 0xFFFF) / 65535.0f; }

/* ------------------------------------------------------------------ police 5×7 */
static const struct { char c; const char *r[7]; } FONT[] = {
    { 'A', { " ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" } },
    { 'B', { "#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### " } },
    { 'C', { " ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### " } },
    { 'D', { "#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### " } },
    { 'E', { "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####" } },
    { 'F', { "#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    " } },
    { 'G', { " ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ####" } },
    { 'H', { "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" } },
    { 'I', { " ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " } },
    { 'J', { "  ###", "   # ", "   # ", "   # ", "   # ", "#  # ", " ##  " } },
    { 'K', { "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #" } },
    { 'L', { "#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####" } },
    { 'M', { "#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #" } },
    { 'N', { "#   #", "##  #", "# # #", "#  ##", "#   #", "#   #", "#   #" } },
    { 'O', { " ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " } },
    { 'P', { "#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    " } },
    { 'Q', { " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #" } },
    { 'R', { "#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #" } },
    { 'S', { " ####", "#    ", "#    ", " ### ", "    #", "    #", "#### " } },
    { 'T', { "#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  " } },
    { 'U', { "#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### " } },
    { 'V', { "#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  " } },
    { 'W', { "#   #", "#   #", "#   #", "# # #", "# # #", "# # #", " # # " } },
    { 'X', { "#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #" } },
    { 'Y', { "#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  " } },
    { 'Z', { "#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####" } },
    { '0', { " ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### " } },
    { '1', { "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " } },
    { '2', { " ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####" } },
    { '3', { "#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### " } },
    { '4', { "   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # " } },
    { '5', { "#####", "#    ", "#### ", "    #", "    #", "#   #", " ### " } },
    { '6', { "  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### " } },
    { '7', { "#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   " } },
    { '8', { " ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### " } },
    { '9', { " ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  " } },
    { '!', { "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  " } },
    { '.', { "     ", "     ", "     ", "     ", "     ", " ##  ", " ##  " } },
    { ',', { "     ", "     ", "     ", "     ", " ##  ", "  #  ", " #   " } },
    { '-', { "     ", "     ", "     ", "#####", "     ", "     ", "     " } },
    { '\'', { "  #  ", "  #  ", " #   ", "     ", "     ", "     ", "     " } },
    { ':', { "     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     " } },
    { '&', { " ##  ", "#  # ", "# #  ", " #   ", "# # #", "#  # ", " ## #" } },
    { '%', { "##   ", "##  #", "   # ", "  #  ", " #   ", "#  ##", "   ##" } },
    { '*', { "     ", "  #  ", "# # #", " ### ", "# # #", "  #  ", "     " } },
    { '?', { " ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  " } },
    { '\x01', { " ####", "# #  ", "# #  ", "# ###", "# #  ", "# #  ", " ####" } },
    { '/', { "    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    " } },
};
static uint8_t s_font[128][7];             /* bits 4..0 = colonnes */

static void font_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (size_t i = 0; i < sizeof FONT / sizeof FONT[0]; i++)
        for (int y = 0; y < 7; y++) {
            uint8_t b = 0;
            for (int x = 0; x < 5; x++) if (FONT[i].r[y][x] == '#') b |= 1 << (4 - x);
            s_font[(int)FONT[i].c][y] = b;
        }
}

/* UTF-8 → lettre de base + accent (1 aigu, 2 grave, 3 point médian, 4 circonflexe, 5 tréma, 6 cédille) */
static const char *next_char(const char *s, char *base, int *acc)
{
    unsigned char c = (unsigned char)*s;
    *acc = 0;
    if (c < 0x80) { *base = (char)(c >= 'a' && c <= 'z' ? c - 32 : c); return s + 1; }
    if ((c & 0xE0) == 0xC0 && s[1]) {
        unsigned cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        switch (cp) {
        case 0xC9: case 0xE9: *base = 'E'; *acc = 1; break;
        case 0xC8: case 0xE8: *base = 'E'; *acc = 2; break;
        case 0xCA: case 0xEA: *base = 'E'; *acc = 4; break;
        case 0xCB: case 0xEB: *base = 'E'; *acc = 5; break;
        case 0xCD: case 0xED: *base = 'I'; *acc = 1; break;
        case 0xCE: case 0xEE: *base = 'I'; *acc = 4; break;
        case 0xCF: case 0xEF: *base = 'I'; *acc = 5; break;
        case 0xD3: case 0xF3: *base = 'O'; *acc = 1; break;
        case 0xD4: case 0xF4: *base = 'O'; *acc = 4; break;
        case 0xDA: case 0xFA: *base = 'U'; *acc = 1; break;
        case 0xD9: case 0xF9: *base = 'U'; *acc = 2; break;
        case 0xDB: case 0xFB: *base = 'U'; *acc = 4; break;
        case 0xC0: case 0xE0: *base = 'A'; *acc = 2; break;
        case 0xC2: case 0xE2: *base = 'A'; *acc = 4; break;
        case 0xC7: case 0xE7: *base = 'C'; *acc = 6; break;
        case 0xB7: *base = '*'; *acc = 3; break;
        case 0x152: case 0x153: *base = '\x01'; break;
        case 0xD7: *base = 'X'; break;
        default: *base = '?';
        }
        return s + 2;
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) { *base = '-'; return s + 3; }   /* tirets typographiques */
    *base = '?';
    s++;
    while (*s && (*s & 0xC0) == 0x80) s++;
    return s;
}

int g_char_len(const char *s)
{
    char b; int a;
    return (int)(next_char(s, &b, &a) - s);
}

static void block(int x, int y, int sc, g_plot_fn plot, void *ctx)
{
    for (int j = 0; j < sc; j++) for (int i = 0; i < sc; i++) plot(x + i, y + j, ctx);
}

int g_text_walk(const char *s, int x, int y, int sc, g_plot_fn plot, void *ctx)
{
    font_init();
    int x0 = x;
    while (*s) {
        char b; int acc;
        s = next_char(s, &b, &acc);
        if (acc == 3) { block(x + 2 * sc, y + 3 * sc, sc, plot, ctx); x += 6 * sc; continue; }
        const uint8_t *g = s_font[(unsigned char)b & 127];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (1 << (4 - c))) block(x + c * sc, y + r * sc, sc, plot, ctx);
        switch (acc) {
        case 1: block(x + 3 * sc, y - 2 * sc, sc, plot, ctx); block(x + 2 * sc, y - sc, sc, plot, ctx); break;
        case 2: block(x + 1 * sc, y - 2 * sc, sc, plot, ctx); block(x + 2 * sc, y - sc, sc, plot, ctx); break;
        case 4: block(x + 1 * sc, y - sc, sc, plot, ctx); block(x + 2 * sc, y - 2 * sc, sc, plot, ctx); block(x + 3 * sc, y - sc, sc, plot, ctx); break;
        case 5: block(x + 1 * sc, y - 2 * sc, sc, plot, ctx); block(x + 3 * sc, y - 2 * sc, sc, plot, ctx); break;
        case 6: block(x + 2 * sc, y + 7 * sc, sc, plot, ctx); block(x + 1 * sc, y + 8 * sc, sc, plot, ctx); break;
        }
        x += 6 * sc;
    }
    return x - x0;
}

int g_text_width(const char *s, int sc)
{
    int n = 0;
    while (*s) { s += g_char_len(s); n++; }
    return n ? n * 6 * sc - sc : 0;
}

int g_text_fit(const char *s, int sc, int max_w)
{
    while (sc > 1 && g_text_width(s, sc) > max_w) sc--;
    return sc;
}

typedef struct { gsurf_t *s; uint16_t col; } pctx_t;
static void plot_s(int x, int y, void *ctx)
{
    pctx_t *k = ctx;
    g_put(k->s, x, y, k->col);
}

void g_text(gsurf_t *s, const char *t, int x, int y, int sc, uint16_t col, bool shadow)
{
    pctx_t k = { s, 0 };
    if (shadow) g_text_walk(t, x + (sc + 1) / 2, y + (sc + 1) / 2, sc, plot_s, &k);
    k.col = col;
    g_text_walk(t, x, y, sc, plot_s, &k);
}

void g_text_c(gsurf_t *s, const char *t, int y, int sc, uint16_t col, bool shadow)
{
    g_text(s, t, (s->w - g_text_width(t, sc)) / 2, y, sc, col, shadow);
}

/* ------------------------------------------------------------------ surfaces */
void g_tri(gsurf_t *s, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{
    int t;
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y2 < y0) { t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y2 == y0) return;
    for (int y = y0 < 0 ? 0 : y0; y <= y2 && y < s->h; y++) {
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        int xb;
        if (y < y1) xb = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        else xb = y2 == y1 ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        if (xa > xb) { t = xa; xa = xb; xb = t; }
        if (xa < 0) xa = 0;
        if (xb >= s->w) xb = s->w - 1;
        uint16_t *row = s->px + y * s->w;
        for (int x = xa; x <= xb; x++) row[x] = c;
    }
}

void g_rect(gsurf_t *s, int x0, int y0, int x1, int y1, uint16_t c)
{
    y0 -= s->oy; y1 -= s->oy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) s->px[y * s->w + x] = c;
}

void g_fill(gsurf_t *s, uint16_t c)
{
    for (int i = 0; i < s->w * s->h; i++) s->px[i] = c;
}

void g_flash(gsurf_t *s, int k)
{
    if (k <= 0) return;
    if (k > 256) k = 256;
    for (int i = 0; i < s->w * s->h; i++) s->px[i] = g_white(s->px[i], k);
}

void g_fade(gsurf_t *s, int k)
{
    if (k <= 0) return;
    if (k > 256) k = 256;
    for (int i = 0; i < s->w * s->h; i++) s->px[i] = g_blend(s->px[i], 0, k);
}

void g_scale(const gsurf_t *s, uint16_t *dst, int dst_w, int sx, int sy)
{
    for (int y = 0; y < s->h; y++) {
        const uint16_t *src = s->px + y * s->w;
        uint16_t *d = dst + (size_t)y * sy * dst_w;
        if (sx == 2) {
            uint32_t *d32 = (uint32_t *)d;
            for (int x = 0; x < s->w; x++) d32[x] = src[x] | ((uint32_t)src[x] << 16);
        } else {
            for (int x = 0; x < s->w; x++) for (int i = 0; i < sx; i++) d[x * sx + i] = src[x];
        }
        for (int j = 1; j < sy; j++) memcpy(d + j * dst_w, d, (size_t)s->w * sx * 2);
    }
}
