/*
 * app_demo.c — « Tír na nÓg », une petite intro façon démoscène.
 *
 * Tout est calculé sur la montre, rien n'est stocké : étoiles, titre en
 * particules, tunnel texturé, tore ombré, paysage en voxels (carte générée au
 * lancement) et musique (demo_synth.c). L'image est rendue en 120×120 dans un
 * tampon en RAM interne puis doublée dans un canevas 240×240 en PSRAM.
 *
 *   0–7,7 s    champ d'étoiles, le titre se forme en particules, éclair
 *   7,7–15 s   tunnel infini (motif entrelacé, couleurs qui tournent)
 *   15–31 s    tore ombré qui pulse sur la grosse caisse
 *   31 s →     survol des collines au coucher du soleil, défilement des
 *              salutations, puis « À TOI ! » : on pilote en penchant le poignet
 *
 * Toucher l'écran coupe ou remet le son. Glisser à droite pour quitter.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "motion.h"
#include "power.h"
#include "esp_heap_caps.h"
#include "demo_synth.h"

#define W  120
#define H  120
#define PI 3.14159265f

#define T_TUNNEL  (4 * DEMO_BAR_MS)
#define T_TORUS   (8 * DEMO_BAR_MS)
#define T_VOXEL   (16 * DEMO_BAR_MS)
#define T_SCROLL  (T_VOXEL + 1500)
#define T_PILOT   (36 * DEMO_BAR_MS)

static uint16_t *fb;                 /* 120×120, RAM interne */
static uint16_t *cv;                 /* 240×240, PSRAM (canevas) */
static uint8_t *hmap;                /* 256×256 hauteurs */
static uint16_t *cmap;               /* 256×256 couleurs */
static uint8_t *t_ang, *t_dep, *t_shd;   /* tunnel : angle, profondeur, ombre */
static uint8_t *tex;                 /* 128×128 */
static lv_obj_t *v_cv;
static lv_timer_t *v_tmr;
static bool s_sound = true, s_tilt;
static uint32_t s_sound_ms;          /* affichage de l'icône son */
static uint32_t s_last_ms;

static inline uint16_t rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t blend(uint16_t a, uint16_t b, int t /* 0..256 */)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + ((br - ar) * t >> 8)) << 11) | ((ag + ((bg - ag) * t >> 8)) << 5) | (ab + ((bb - ab) * t >> 8)));
}

static inline uint16_t add565(uint16_t a, int k /* 0..255 vers le blanc */)
{
    return blend(a, 0xFFFF, k);
}

static uint32_t rng = 0x1234567;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float frnd(void) { return (rnd() & 0xFFFF) / 65535.0f; }

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
    { '/', { "    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    " } },
};
static uint8_t s_font[128][7];       /* bits 4..0 = colonnes */

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

/* UTF-8 → lettre de base + accent (1 aigu, 2 grave, 3 point médian, 4 circonflexe) */
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
        case 0xCD: case 0xED: *base = 'I'; *acc = 1; break;
        case 0xD3: case 0xF3: *base = 'O'; *acc = 1; break;
        case 0xDA: case 0xFA: *base = 'U'; *acc = 1; break;
        case 0xC0: case 0xE0: *base = 'A'; *acc = 2; break;
        case 0xCA: case 0xEA: *base = 'E'; *acc = 4; break;
        case 0xD4: case 0xF4: *base = 'O'; *acc = 4; break;
        case 0xC7: case 0xE7: *base = 'C'; break;
        case 0xB7: *base = '*'; *acc = 3; break;
        default: *base = '?';
        }
        return s + 2;
    }
    *base = '?';
    while (*s && (*s & 0xC0) == 0x80) s++;
    return s + 1;
}

typedef void (*plot_fn)(int x, int y, void *ctx);

/* dessine un texte ; pour chaque pixel allumé, plot(x, y) ; renvoie la largeur */
static int text_walk(const char *s, int x, int y, int sc, plot_fn plot, void *ctx)
{
    int x0 = x;
    while (*s) {
        char b; int acc;
        s = next_char(s, &b, &acc);
        if (acc == 3) {                                   /* point médian */
            for (int j = 0; j < sc; j++) for (int i = 0; i < sc; i++) plot(x + 2 * sc + i, y + 3 * sc + j, ctx);
            x += 6 * sc;
            continue;
        }
        const uint8_t *g = s_font[(unsigned char)b & 127];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (1 << (4 - c)))
                    for (int j = 0; j < sc; j++) for (int i = 0; i < sc; i++) plot(x + c * sc + i, y + r * sc + j, ctx);
        if (acc == 4) {                                   /* circonflexe */
            for (int j = 0; j < sc; j++) for (int i = 0; i < sc; i++) {
                plot(x + 1 * sc + i, y - 1 * sc + j, ctx);
                plot(x + 2 * sc + i, y - 2 * sc + j, ctx);
                plot(x + 3 * sc + i, y - 1 * sc + j, ctx);
            }
        } else if (acc) {                                 /* accent au-dessus */
            int ax = acc == 1 ? 3 : 1;
            for (int j = 0; j < sc; j++) for (int i = 0; i < sc; i++) {
                plot(x + ax * sc + i, y - 2 * sc + j, ctx);
                plot(x + 2 * sc + i, y - 1 * sc + j, ctx);
            }
        }
        x += 6 * sc;
    }
    return x - x0;
}

static int text_width(const char *s, int sc)
{
    int n = 0;
    while (*s) { char b; int a; s = next_char(s, &b, &a); n++; }
    return n * 6 * sc - sc;
}

static int fit(const char *s, int sc)            /* plus grande échelle ≤ sc qui tient */
{
    while (sc > 1 && text_width(s, sc) > W - 2) sc--;
    return sc;
}

typedef struct { uint16_t col; } pctx_t;
static void plot_fb(int x, int y, void *ctx)
{
    if ((unsigned)x < W && (unsigned)y < H) fb[y * W + x] = ((pctx_t *)ctx)->col;
}

static void text(const char *s, int x, int y, int sc, uint16_t col, bool shadow)
{
    pctx_t k = { 0 };
    if (shadow) text_walk(s, x + 1, y + 1, sc, plot_fb, &k);
    k.col = col;
    text_walk(s, x, y, sc, plot_fb, &k);
}

static void text_c(const char *s, int y, int sc, uint16_t col, bool shadow)
{
    text(s, (W - text_width(s, sc)) / 2, y, sc, col, shadow);
}

/* ------------------------------------------------------------------ 1. étoiles et titre */
#define NSTAR 140
#define NPART 900
static float st_x[NSTAR], st_y[NSTAR], st_z[NSTAR];
static int16_t *p_tx, *p_ty;          /* cibles du titre */
static float *p_sx, *p_sy;            /* départs */
static int s_np;

static const char *const TITLE_1 = "TÍR NA", *const TITLE_2 = "NÓG";

typedef struct { int n; } collect_t;
static void plot_collect(int x, int y, void *ctx)
{
    collect_t *c = ctx;
    if (c->n < NPART) { p_tx[c->n] = (int16_t)x; p_ty[c->n] = (int16_t)y; c->n++; }
}

static void stars_init(void)
{
    for (int i = 0; i < NSTAR; i++) { st_x[i] = frnd() * 2 - 1; st_y[i] = frnd() * 2 - 1; st_z[i] = 0.05f + frnd(); }
    collect_t c = { 0 };
    text_walk(TITLE_1, (W - text_width(TITLE_1, 2)) / 2, 40, 2, plot_collect, &c);
    text_walk(TITLE_2, (W - text_width(TITLE_2, 3)) / 2, 62, 3, plot_collect, &c);
    s_np = c.n;
    for (int i = 0; i < s_np; i++) {
        float a = frnd() * 2 * PI, r = 70 + frnd() * 60;
        p_sx[i] = 60 + cosf(a) * r;
        p_sy[i] = 60 + sinf(a) * r;
    }
}

static void put(int x, int y, uint16_t c) { if ((unsigned)x < W && (unsigned)y < H) fb[y * W + x] = c; }

static void scene_stars(uint32_t ms, float dt)
{
    /* fond : léger dégradé nuit */
    for (int y = 0; y < H; y++) {
        uint16_t c = rgb(4, 4 + y / 12, 16 + y / 5);
        for (int x = 0; x < W; x++) fb[y * W + x] = c;
    }
    float speed = 0.25f + 1.6f * (ms / (float)T_TUNNEL) * (ms / (float)T_TUNNEL);
    for (int i = 0; i < NSTAR; i++) {
        float pz = st_z[i];
        st_z[i] -= speed * dt;
        if (st_z[i] < 0.03f) { st_x[i] = frnd() * 2 - 1; st_y[i] = frnd() * 2 - 1; st_z[i] = 1.0f; pz = 1.0f; }
        float sx = 60 + st_x[i] / st_z[i] * 50, sy = 60 + st_y[i] / st_z[i] * 50;
        float px = 60 + st_x[i] / pz * 50, py = 60 + st_y[i] / pz * 50;
        int b = (int)(255 * (1.0f - st_z[i]));
        uint16_t c = rgb(b, b, b + 40);
        /* traînée */
        int n = (int)(fabsf(sx - px) + fabsf(sy - py)) + 1;
        if (n > 12) n = 12;
        for (int k = 0; k <= n; k++) {
            float u = k / (float)n;
            put((int)(px + (sx - px) * u), (int)(py + (sy - py) * u), blend(rgb(0, 0, 40), c, (int)(u * 256)));
        }
    }
    /* sous-titre */
    if (ms > 600 && ms < 3800) {
        int a = ms < 1200 ? (int)((ms - 600) * 256 / 600) : ms > 3200 ? (int)((3800 - ms) * 256 / 600) : 256;
        if (a > 256) a = 256;
        if (a < 0) a = 0;
        text_c(_("SEB & CLAUDE"), 46, 1, blend(0, rgb(170, 190, 255), a), false);
        text_c(_("PRÉSENTENT"), 60, 1, blend(0, rgb(170, 190, 255), a), false);
    }
    /* le titre se forme en particules */
    if (ms > 3200) {
        float t = (ms - 3200) / 3000.0f;
        if (t > 1) t = 1;
        float e = t * t * (3 - 2 * t);
        float boom = ms > T_TUNNEL - 450 ? (ms - (T_TUNNEL - 450)) / 450.0f : 0;
        float sw = (1 - e) * 2.4f, cs = cosf(sw), sn = sinf(sw);
        float sweep = fmodf(ms / 900.0f, 2.0f) * 160 - 20;
        for (int i = 0; i < s_np; i++) {
            float cx = p_sx[i] - 60, cy = p_sy[i] - 60;
            float rx = 60 + cx * cs - cy * sn, ry = 60 + cx * sn + cy * cs;
            float x = rx + (p_tx[i] - rx) * e, y = ry + (p_ty[i] - ry) * e;
            if (boom > 0) { x += (x - 60) * boom * 2.5f; y += (y - 60) * boom * 2.5f; }
            /* reflet doré qui balaie le titre */
            int hl = (int)(220 - fabsf(x - sweep) * 9);
            if (hl < 0) hl = 0;
            uint16_t c = add565(rgb(90 + (int)(120 * e), 200, 110), hl);
            put((int)x, (int)y, c);
        }
        if (e >= 1 && boom == 0) text_c("T-WATCH 2020", 92, 1, rgb(120, 160, 120), false);
    }
}

/* ------------------------------------------------------------------ 2. tunnel */
static uint16_t s_pal[256];

static void tunnel_init(void)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float dx = x - 59.5f, dy = y - 59.5f;
            float r = sqrtf(dx * dx + dy * dy) + 0.5f;
            int i = y * W + x;
            t_ang[i] = (uint8_t)((int)(atan2f(dy, dx) / (2 * PI) * 256) & 255);
            t_dep[i] = (uint8_t)((int)(2600.0f / r) & 255);
            float s = r < 8 ? 0 : (r - 8) / 52.0f;
            t_shd[i] = (uint8_t)(s > 1 ? 255 : s * 255);
        }
    /* texture entrelacée (vaguement celtique) */
    for (int v = 0; v < 128; v++)
        for (int u = 0; u < 128; u++) {
            float a = sinf(u * 2 * PI / 32) * sinf(v * 2 * PI / 32);
            float b = sinf((u + v) * 2 * PI / 64);
            int band = ((u / 16 + v / 16) & 1) ? (u & 15) : (v & 15);
            float k = 0.5f + 0.35f * a + 0.15f * b + (band < 3 ? -0.35f : 0);
            tex[v * 128 + u] = (uint8_t)(k < 0 ? 0 : k > 1 ? 255 : k * 255);
        }
}

static void make_pal(float shift)
{
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        float r = 0.5f + 0.5f * cosf(2 * PI * (t * 0.9f + shift + 0.00f));
        float g = 0.5f + 0.5f * cosf(2 * PI * (t * 0.9f + shift + 0.33f));
        float b = 0.5f + 0.5f * cosf(2 * PI * (t * 0.9f + shift + 0.67f));
        float l = t;                                     /* sombre → clair */
        s_pal[i] = rgb((int)(r * 255 * l + 40 * l), (int)(g * 255 * l + 60 * l), (int)(b * 255 * l));
    }
}

static void scene_tunnel(uint32_t ms)
{
    float t = (ms - T_TUNNEL) / 1000.0f;
    make_pal(t * 0.08f);
    int du = (int)(t * 90), dv = (int)(t * 26);
    float kick = demo_synth_kick() + demo_synth_snare() * 0.6f;
    int bright = (int)(kick * 70);
    int8_t twist[256];                                   /* torsion selon la profondeur */
    for (int d = 0; d < 256; d++) twist[d] = (int8_t)(sinf(d * 0.05f + t * 1.7f) * 18);
    for (int i = 0; i < W * H; i++) {
        int d = t_dep[i];
        int a = t_ang[i] + twist[d];
        int c = tex[((d + du) & 127) * 128 + ((a / 2 + dv) & 127)];
        int s = t_shd[i];
        int idx = (c * s >> 8) + bright;
        fb[i] = s_pal[idx > 255 ? 255 : idx];
    }
    if (t < 0.25f) {                                     /* éclair d'entrée */
        int k = (int)((0.25f - t) * 4 * 255);
        for (int i = 0; i < W * H; i++) fb[i] = add565(fb[i], k);
    }
    static const char *const WORDS[] = { N_("100 %"), N_("CALCULÉ"), N_("SUR LA"), N_("MONTRE") };
    int w = (int)(t / (DEMO_BAR_MS / 1000.0f));
    if (w >= 0 && w < 4 && fmodf(t, DEMO_BAR_MS / 1000.0f) < 1.4f) text_c(_(WORDS[w]), 54, 2, 0xFFFF, true);
}

/* ------------------------------------------------------------------ 3. tore */
#define TU 24
#define TV 12
typedef struct { int16_t x, y; float z; } pv_t;
static pv_t s_pv[TU * TV];
static float s_vx[TU * TV], s_vy[TU * TV], s_vz[TU * TV];
typedef struct { uint16_t i; float z; uint16_t col; } face_t;
static face_t s_face[TU * TV];

static void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{
    /* tri des sommets en y, remplissage par lignes (virgule fixe 16.16) */
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y2 < y0) { int t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
    if (y2 < y1) { int t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y2 == y0) return;
    for (int y = y0 < 0 ? 0 : y0; y <= y2 && y < H; y++) {
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        int xb;
        if (y < y1) xb = y1 == y0 ? x1 : x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        else xb = y2 == y1 ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        if (xa > xb) { int t = xa; xa = xb; xb = t; }
        if (xa < 0) xa = 0;
        if (xb >= W) xb = W - 1;
        uint16_t *row = fb + y * W;
        for (int x = xa; x <= xb; x++) row[x] = c;
    }
}

static int face_cmp(const void *a, const void *b)
{
    float d = ((const face_t *)b)->z - ((const face_t *)a)->z;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static void scene_torus(uint32_t ms)
{
    float t = (ms - T_TORUS) / 1000.0f;
    float kick = demo_synth_kick();
    /* fond : halo violet qui respire + plasma discret */
    int8_t px[W], py[H];
    for (int i = 0; i < W; i++) px[i] = (int8_t)(sinf(i * 0.09f + t) * 8);
    for (int i = 0; i < H; i++) py[i] = (int8_t)(sinf(i * 0.07f - t * 1.3f) * 8);
    int kk = (int)(kick * 25);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int dx = x - 60, dy = y - 60;
            int r = (dx * dx + dy * dy) >> 6;
            int k = 70 - r + px[x] + py[y] + kk;
            if (k < 0) k = 0;
            fb[y * W + x] = rgb(k / 2 + 6, k / 6 + 2, k + 14);
        }
    float R = 1.0f, r = 0.42f * (1 + 0.18f * kick);
    float ax = t * 0.9f, ay = t * 0.63f, az = t * 0.31f;
    float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay), cz = cosf(az), sz = sinf(az);
    for (int i = 0; i < TU; i++)
        for (int j = 0; j < TV; j++) {
            float u = i * 2 * PI / TU, v = j * 2 * PI / TV;
            float x = (R + r * cosf(v)) * cosf(u), y = (R + r * cosf(v)) * sinf(u), z = r * sinf(v);
            float y1 = y * cx - z * sx, z1 = y * sx + z * cx;           /* X */
            float x2 = x * cy + z1 * sy, z2 = -x * sy + z1 * cy;        /* Y */
            float x3 = x2 * cz - y1 * sz, y3 = x2 * sz + y1 * cz;       /* Z */
            int k = i * TV + j;
            s_vx[k] = x3; s_vy[k] = y3; s_vz[k] = z2;
            float d = 3.2f + z2;
            s_pv[k].x = (int16_t)(60 + x3 / d * 82);
            s_pv[k].y = (int16_t)(60 + y3 / d * 82);
            s_pv[k].z = z2;
        }
    const float lx = -0.4f, ly = -0.55f, lz = -0.73f;
    int nf = 0;
    for (int i = 0; i < TU; i++)
        for (int j = 0; j < TV; j++) {
            int a = i * TV + j, b = ((i + 1) % TU) * TV + j, c = ((i + 1) % TU) * TV + (j + 1) % TV, d = i * TV + (j + 1) % TV;
            /* face visible ? (sens à l'écran) */
            int cr = (s_pv[b].x - s_pv[a].x) * (s_pv[d].y - s_pv[a].y) - (s_pv[b].y - s_pv[a].y) * (s_pv[d].x - s_pv[a].x);
            if (cr >= 0) continue;
            float e1x = s_vx[b] - s_vx[a], e1y = s_vy[b] - s_vy[a], e1z = s_vz[b] - s_vz[a];
            float e2x = s_vx[d] - s_vx[a], e2y = s_vy[d] - s_vy[a], e2z = s_vz[d] - s_vz[a];
            float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
            float nl = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-6f;
            float l = (nx * lx + ny * ly + nz * lz) / nl;
            if (l < 0) l = -l * 0.25f;
            float spec = l > 0.9f ? (l - 0.9f) * 10 : 0;
            /* anneaux de couleur le long du tore */
            float hue = i / (float)TU + t * 0.05f;
            int rr = (int)((140 + 110 * sinf(hue * 2 * PI)) * (0.15f + 0.85f * l));
            int gg = (int)((60 + 50 * sinf(hue * 2 * PI + 2.1f)) * (0.15f + 0.85f * l));
            int bb = (int)((170 + 80 * sinf(hue * 2 * PI + 4.2f)) * (0.15f + 0.85f * l));
            uint16_t col = add565(rgb(rr, gg, bb), (int)(spec * 220));
            s_face[nf].i = (uint16_t)(i * TV + j);
            s_face[nf].z = (s_pv[a].z + s_pv[b].z + s_pv[c].z + s_pv[d].z) * 0.25f;
            s_face[nf].col = col;
            nf++;
        }
    qsort(s_face, nf, sizeof s_face[0], face_cmp);
    for (int f = 0; f < nf; f++) {
        int i = s_face[f].i / TV, j = s_face[f].i % TV;
        int a = i * TV + j, b = ((i + 1) % TU) * TV + j, c = ((i + 1) % TU) * TV + (j + 1) % TV, d = i * TV + (j + 1) % TV;
        tri(s_pv[a].x, s_pv[a].y, s_pv[b].x, s_pv[b].y, s_pv[c].x, s_pv[c].y, s_face[f].col);
        tri(s_pv[a].x, s_pv[a].y, s_pv[c].x, s_pv[c].y, s_pv[d].x, s_pv[d].y, s_face[f].col);
    }
    if (t < 0.2f) { int k = (int)((0.2f - t) * 5 * 255); for (int i = 0; i < W * H; i++) fb[i] = add565(fb[i], k); }
    if (t > 2 && t < 13) text_c(_("RENDU LOGICIEL"), 110, 1, rgb(200, 170, 255), true);
}

/* ------------------------------------------------------------------ 4. voxels */
static float cam_x = 128, cam_y = 128, cam_a = 0, cam_h = 120, cam_roll, cam_pitch;
static float tilt0_x, tilt0_y, s_turn;
#define SUN_A 0.3f

static float vnoise_lat[64 * 64];
static float vnoise(float x, float y, int per)     /* bruit de valeur périodique */
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    int x0 = xi % per, y0 = yi % per, x1 = (xi + 1) % per, y1 = (yi + 1) % per;
    float a = vnoise_lat[y0 * 64 + x0], b = vnoise_lat[y0 * 64 + x1];
    float c = vnoise_lat[y1 * 64 + x0], d = vnoise_lat[y1 * 64 + x1];
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static void voxel_init(void)
{
    for (int i = 0; i < 64 * 64; i++) vnoise_lat[i] = frnd();
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            /* la carte se répète tous les 256 : chaque octave a une période entière */
            float h = 0, amp = 1, tot = 0;
            for (int per = 6; per <= 48; per *= 2) {
                h += vnoise(x * per / 256.0f, y * per / 256.0f, per) * amp;
                tot += amp; amp *= 0.45f;
            }
            h /= tot;
            float v = (h - 0.36f) / 0.4f;
            if (v < 0) v = 0;
            v = powf(v, 1.5f) * 115;
            hmap[y * 256 + x] = (uint8_t)(v > 255 ? 255 : v);
        }
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            int h = hmap[y * 256 + x];
            int sl = h - hmap[((y + 1) & 255) * 256 + ((x + 1) & 255)];   /* pente : lumière du soir */
            int r, g, b;
            if (h < 18)       { r = 20; g = 60 + h * 2; b = 110 + h * 3; }        /* mer */
            else if (h < 26)  { r = 200; g = 180; b = 120; }                     /* sable */
            else if (h < 70)  { r = 60 + h / 3; g = 125 + h / 3; b = 45; }       /* herbe */
            else if (h < 95)  { r = 115; g = 105; b = 90; }                      /* roche */
            else              { r = 235; g = 232; b = 240; }                     /* neige */
            int k = sl * 10;
            r += k; g += k; b += k / 2;
            if (h >= 18) { int n = (int)(rnd() & 15) - 8; r += n; g += n; b += n; }   /* grain */
            cmap[y * 256 + x] = rgb(r, g, b);
        }
}

static uint8_t H_AT(float x, float y) { return hmap[(((int)y) & 255) * 256 + (((int)x) & 255)]; }

static void scene_voxel(uint32_t ms, float dt)
{
    float t = (ms - T_VOXEL) / 1000.0f;
    bool pilot = ms >= T_PILOT;
    /* ciel du soir, indexé par la hauteur au-dessus de l'horizon */
    const uint16_t sky_top = rgb(30, 24, 80), sky_bot = rgb(255, 150, 90);
    uint16_t sky[96];
    for (int i = 0; i < 96; i++) { int k = 256 - i * 256 / 70; sky[i] = blend(sky_top, sky_bot, k < 0 ? 0 : k); }
    const uint16_t fog = rgb(240, 150, 110);

    /* pilote : automatique, puis le poignet */
    float turn = 0, climb = 0;
    if (pilot && s_tilt) {
        int16_t ax, ay, az;
        if (motion_accel_read(&ax, &ay, &az)) {
            float fx = ax / 1024.0f, fy = ay / 1024.0f;
            tilt0_x += (fx - tilt0_x) * 0.004f;          /* le neutre suit lentement la pose */
            tilt0_y += (fy - tilt0_y) * 0.004f;
            turn = (fx - tilt0_x) * 2.2f;
            climb = (fy - tilt0_y) * 140;
        }
    } else {
        /* pilote automatique : suit les vallées, avec un peu de fantaisie */
        float hl = 0, hr = 0;
        for (int k = 1; k <= 3; k++) {
            float d = 25.0f * k;
            hl += H_AT(cam_x + cosf(cam_a - 0.45f) * d, cam_y + sinf(cam_a - 0.45f) * d);
            hr += H_AT(cam_x + cosf(cam_a + 0.45f) * d, cam_y + sinf(cam_a + 0.45f) * d);
        }
        turn = (hl - hr) * 0.004f + sinf(t * 0.31f) * 0.3f;
    }
    s_turn += (turn - s_turn) * 0.06f;
    turn = s_turn;
    if (turn > 0.9f) turn = 0.9f;
    if (turn < -0.9f) turn = -0.9f;
    cam_a += turn * dt * 0.9f;
    cam_roll += (turn * 0.6f - cam_roll) * 0.1f;
    float v = 38;
    cam_x += cosf(cam_a) * v * dt;
    cam_y += sinf(cam_a) * v * dt;
    float ground = 0;
    for (int k = 0; k < 8; k++) { float g = H_AT(cam_x + cosf(cam_a) * k * 9, cam_y + sinf(cam_a) * k * 9) - k * 2.0f; if (g > ground) ground = g; }
    float target = ground + 50 + climb;
    if (target < 40) target = 40;
    cam_h += (target - cam_h) * 0.06f;
    if (cam_h < ground + 12) cam_h = ground + 12;
    cam_pitch += ((pilot ? -climb * 0.25f : 0) - cam_pitch) * 0.1f;

    float hz = 40 + cam_pitch;
    float ca = cosf(cam_a), sa = sinf(cam_a);
    float sd = remainderf(SUN_A - cam_a, 2 * PI);          /* soleil : direction fixe */
    int sun_col = fabsf(sd) < 1.2f ? 60 + (int)(tanf(sd) * 60) : -1000;
    for (int col = 0; col < W; col++) {
        float horizon = hz + (col - 60) * cam_roll;
        float rx = (col - 60) / 60.0f;                    /* champ ±45° */
        float dx = ca - sa * rx, dy = sa + ca * rx;
        int ybuf = H;
        float z = 1, dz = 1;
        while (z < 240 && ybuf > 0) {
            float px = cam_x + dx * z, py = cam_y + dy * z;
            int mi = (((int)py) & 255) * 256 + (((int)px) & 255);
            int h = hmap[mi];
            if (h < 18) h = 18;                            /* niveau de la mer */
            int sy = (int)((cam_h - h) / z * 70 + horizon);
            if (sy < ybuf) {
                uint16_t c = cmap[mi];
                if (hmap[mi] < 18) c = blend(c, rgb(255, 190, 140), (int)(70 + 30 * sinf((px + py) * 0.2f + t * 2)));   /* reflets */
                int f = (int)(z * 256 / 240);
                c = blend(c, fog, f * f >> 8);
                int y0 = sy < 0 ? 0 : sy;
                for (int y = y0; y < ybuf; y++) fb[y * W + col] = c;
                ybuf = y0;
            }
            z += dz;
            dz *= 1.025f;
        }
        int dxs = col - sun_col;
        for (int y = 0; y < ybuf; y++) {
            int up = (int)horizon - y;                      /* pixels au-dessus de l'horizon */
            uint16_t c = sky[up < 0 ? 0 : up > 95 ? 95 : up];
            if (sun_col > -40 && sun_col < W + 40) {        /* soleil et halo */
                int dy = up - 12, d2 = dxs * dxs + dy * dy;
                if (d2 <= 30) c = rgb(255, 244, 200);
                else if (d2 < 900) c = add565(c, (900 - d2) * 120 / 900);
            }
            fb[y * W + col] = c;
        }
    }

    if (t < 0.25f) { int k = (int)((0.25f - t) * 4 * 255); for (int i = 0; i < W * H; i++) fb[i] = add565(fb[i], k); }

    /* défilement sinusoïdal des salutations */
    if (ms > T_SCROLL && !pilot) {
        const char *MSG = _("TÍR NA NÓG · UNE INTRO POUR T-WATCH 2020 · TOUT EST CALCULÉ ICI, MÊME LA MUSIQUE · "
                            "SALUTATIONS À FARBRAUSCH · LILYGO · ESPRESSIF · LA DÉMOSCÈNE · "
                            "ET AUX AMIS DES QSO DES GASCONS · BONS 73 ! ·         ");
        /* vitesse calée pour que tout le texte passe avant « À TOI ! » */
        float span = W + text_width(MSG, 2);
        float off = (ms - T_SCROLL) * span / (T_PILOT - T_SCROLL);
        int x = W - (int)off;
        const char *s = MSG;
        while (*s && x < W) {
            char b; int acc;
            const char *n = next_char(s, &b, &acc);
            if (x > -12) {
                char one[4] = { 0 };
                memcpy(one, s, (size_t)(n - s));
                int y = 92 + (int)(sinf(x * 0.08f + t * 4) * 6);
                text(one, x, y, 2, rgb(255, 255, 220), true);
            }
            x += 12;
            s = n;
        }
    }
    if (pilot) {
        float k = (ms - T_PILOT) / 1000.0f;
        if (k < 4 && fmodf(k, 0.5f) < 0.35f) text_c(_("À TOI !"), 50, fit(_("À TOI !"), 3), 0xFFFF, true);
        if (k > 1 && k < 7) text_c(_("PENCHE LE POIGNET"), 100, 1, 0xFFFF, true);
    }
}

/* ------------------------------------------------------------------ boucle */
static void frame(void)
{
    if (!fb || !cv) return;
    uint32_t ms = demo_synth_ms();
    float dt = (ms - s_last_ms) / 1000.0f;
    if (dt < 0 || dt > 0.1f) dt = 0.033f;          /* saut d'horloge : pas normal */
    s_last_ms = ms;
    if (ms < T_TUNNEL) scene_stars(ms, dt);
    else if (ms < T_TORUS) scene_tunnel(ms);
    else if (ms < T_VOXEL) scene_torus(ms);
    else scene_voxel(ms, dt);
    if (ms < s_sound_ms) text(s_sound ? _("SON") : _("MUET"), 92, 3, 1, 0xFFFF, true);

    /* ×2 vers le canevas */
    for (int y = 0; y < H; y++) {
        const uint16_t *src = fb + y * W;
        uint32_t *d0 = (uint32_t *)(cv + (2 * y) * 240), *d1 = (uint32_t *)(cv + (2 * y + 1) * 240);
        for (int x = 0; x < W; x++) {
            uint32_t p = src[x] | ((uint32_t)src[x] << 16);
            d0[x] = p;
            d1[x] = p;
        }
    }
    lv_obj_invalidate(v_cv);
}

static void tick_cb(lv_timer_t *t) { frame(); }

static void tap_cb(lv_event_t *e)
{
    s_sound = !s_sound;
    demo_synth_sound(s_sound);
    s_sound_ms = demo_synth_ms() + 1200;
}

/* ------------------------------------------------------------------ cycle de vie */
static void *alloc_int(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void *alloc_ps(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static void free_all(void)
{
    heap_caps_free(fb); heap_caps_free(cv); heap_caps_free(hmap); heap_caps_free(cmap);
    heap_caps_free(t_ang); heap_caps_free(t_dep); heap_caps_free(t_shd); heap_caps_free(tex);
    heap_caps_free(p_tx); heap_caps_free(p_ty); heap_caps_free(p_sx); heap_caps_free(p_sy);
    fb = cv = cmap = NULL; hmap = t_ang = t_dep = t_shd = tex = NULL;
    p_tx = p_ty = NULL; p_sx = p_sy = NULL;
}

static void create(lv_obj_t *scr)
{
    font_init();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    fb = alloc_int(W * H * 2);
    t_ang = alloc_ps(W * H); t_dep = alloc_ps(W * H); t_shd = alloc_ps(W * H);   /* tables : la PSRAM suffit */
    tex = alloc_ps(128 * 128);
    cv = alloc_ps(240 * 240 * 2);
    hmap = alloc_ps(256 * 256);
    cmap = alloc_ps(256 * 256 * 2);
    p_tx = alloc_ps(NPART * 2); p_ty = alloc_ps(NPART * 2);
    p_sx = alloc_ps(NPART * 4); p_sy = alloc_ps(NPART * 4);
    v_cv = lv_canvas_create(scr);
    if (!fb || !cv || !hmap || !cmap || !t_ang || !t_dep || !t_shd || !tex || !p_tx || !p_ty || !p_sx || !p_sy) {
        free_all();
        lv_obj_t *l = ui_label(scr, &font_text_16, C_FG, _("Mémoire insuffisante"));
        lv_obj_center(l);
        return;
    }
    memset(cv, 0, 240 * 240 * 2);
    lv_canvas_set_buffer(v_cv, cv, 240, 240, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(v_cv, 0, 0);
    lv_obj_add_flag(v_cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_cv, tap_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(v_cv);
    rng = 0x1234567;
    stars_init();
    tunnel_init();
    voxel_init();
}

static void destroy(void)
{
    if (v_tmr) { lv_timer_delete(v_tmr); v_tmr = NULL; }
    demo_synth_stop();
    free_all();
    v_cv = NULL;
}

static void on_show(void)
{
    if (!fb) return;
    power_hold_active(true);                      /* 240 MHz pendant l'intro */
    s_tilt = motion_accel_begin();
    int16_t ax, ay, az;
    if (s_tilt && motion_accel_read(&ax, &ay, &az)) { tilt0_x = ax / 1024.0f; tilt0_y = ay / 1024.0f; }
    cam_x = cam_y = 128; cam_a = 0; cam_h = 120; cam_roll = cam_pitch = s_turn = 0;
    s_last_ms = 0;
    demo_synth_start(s_sound);
    if (!v_tmr) v_tmr = lv_timer_create(tick_cb, 20, NULL);
    else lv_timer_resume(v_tmr);
}

static void on_hide(void)
{
    if (v_tmr) lv_timer_pause(v_tmr);
    demo_synth_stop();
    if (fb) power_hold_active(false);
    if (s_tilt) { motion_accel_end(); s_tilt = false; }
}

static bool keep_on(void) { return true; }

const app_t app_demo = {
    .name = N_("Démo"),
    .icon = SYM_ROCKET,
    .color = 0xF97316,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .keep_screen_on = keep_on,
};
