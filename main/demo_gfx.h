/*
 * demo_gfx.h — petits outils de dessin logiciel pour la Démo et le jeu de vol :
 * couleurs RGB565, police 5×7 avec accents, triangles pleins, aléatoire,
 * agrandissement vers le canevas LVGL.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t *px;
    int w, h;
    int oy;          /* première ligne contenue (bande d'écran) ; 0 en général */
} gsurf_t;

static inline uint16_t g_rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* mélange a → b, t de 0 à 256 */
static inline uint16_t g_blend(uint16_t a, uint16_t b, int t)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + ((br - ar) * t >> 8)) << 11) | ((ag + ((bg - ag) * t >> 8)) << 5) | (ab + ((bb - ab) * t >> 8)));
}

static inline uint16_t g_white(uint16_t a, int k) { return g_blend(a, 0xFFFF, k); }

/* addition saturée (lueurs) */
static inline uint16_t g_add(uint16_t a, uint16_t b)
{
    int r = (a >> 11) + (b >> 11), g = ((a >> 5) & 63) + ((b >> 5) & 63), bl = (a & 31) + (b & 31);
    return (uint16_t)(((r > 31 ? 31 : r) << 11) | ((g > 63 ? 63 : g) << 5) | (bl > 31 ? 31 : bl));
}

static inline void g_put(gsurf_t *s, int x, int y, uint16_t c)
{
    y -= s->oy;
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->px[y * s->w + x] = c;
}

static inline void g_glow(gsurf_t *s, int x, int y, uint16_t c)
{
    y -= s->oy;
    if ((unsigned)x < (unsigned)s->w && (unsigned)y < (unsigned)s->h) s->px[y * s->w + x] = g_add(s->px[y * s->w + x], c);
}

/* rectangle plein [x0, x1[ × [y0, y1[, découpé à la surface (bandes comprises) */
void g_rect(gsurf_t *s, int x0, int y0, int x1, int y1, uint16_t c);

/* aléatoire rapide (xorshift) */
void     g_seed(uint32_t s);
uint32_t g_rnd(void);
float    g_frnd(void);                              /* 0..1 */

/* police 5×7 : majuscules, chiffres, ponctuation, accents dessinés ;
 * sc = échelle ; renvoie la largeur dessinée */
typedef void (*g_plot_fn)(int x, int y, void *ctx);
int  g_text_walk(const char *s, int x, int y, int sc, g_plot_fn plot, void *ctx);
int  g_text_width(const char *s, int sc);
int  g_text_fit(const char *s, int sc, int max_w);   /* plus grande échelle ≤ sc qui tient */
void g_text(gsurf_t *s, const char *t, int x, int y, int sc, uint16_t col, bool shadow);
void g_text_c(gsurf_t *s, const char *t, int y, int sc, uint16_t col, bool shadow);
/* un caractère UTF-8 → octets consommés (pour les défilements lettre à lettre) */
int  g_char_len(const char *s);

void g_tri(gsurf_t *s, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);
void g_fill(gsurf_t *s, uint16_t c);
void g_flash(gsurf_t *s, int k);                    /* vers le blanc, k 0..256 */
void g_fade(gsurf_t *s, int k);                     /* vers le noir, k 0..256 */

/* agrandit s dans dst (stride dst_w) : ×sx horizontalement, ×sy verticalement */
void g_scale(const gsurf_t *s, uint16_t *dst, int dst_w, int sx, int sy);
