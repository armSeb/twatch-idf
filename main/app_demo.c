/*
 * app_demo.c — « Tír na nÓg », une intro façon démoscène (~2 min).
 *
 * Tout est calculé sur la montre, rien n'est stocké. L'image est rendue en
 * 120×120 dans un tampon en RAM interne puis doublée dans un canevas 240×240
 * en PSRAM. L'horloge est celle de la musique (demo_synth.c) : les scènes et
 * les effets tombent sur les mesures et les coups de grosse caisse.
 *
 *   mesures  0–4   étoiles, « présentent », le titre se forme en particules
 *            4–8   tunnel texturé
 *            8–12  rotozoom d'un entrelacs sur plasma
 *           12–16  tore ombré
 *           16–24  nuée de 700 points qui change de forme, barres « copper »
 *           24–28  métaballes (le pont), générique
 *           28–32  le titre brûle, roulement
 *           32–48  survol du paysage (voxel.c), défilement des salutations
 *           48–56  nœud celtique en 3D
 *           56–64  retour aux étoiles, « MERCI ! », fondu
 *
 * Toucher l'écran coupe ou remet le son ; à la fin, toucher relance la démo.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "power.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "demo_gfx.h"
#include "demo_synth.h"
#include "voxel.h"

#define W  120
#define H  120
#define PI 3.14159265f
#define BAR(n) ((uint32_t)(n) * DEMO_BAR_MS)

static gsurf_t S;                    /* l'image (120×120, RAM interne) */
static uint16_t *cv;                 /* 240×240, PSRAM (canevas) */
static uint8_t *t_ang, *t_dep, *t_shd;   /* tunnel : angle, profondeur, ombre */
static uint8_t *tex;                 /* entrelacs 128×128 */
static uint8_t *fire;                /* 120×122 */
static lv_obj_t *v_cv;
static lv_timer_t *v_tmr;
static bool s_sound = true;
static uint32_t s_sound_ms, s_last_ms;
static bool s_vox;                   /* paysage demandé */

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float ease(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

/* ------------------------------------------------------------------ particules de texte */
#define NPART 900
static int16_t *p_tx, *p_ty;
static float *p_sx, *p_sy;
static int s_np;

typedef struct { int n; } collect_t;
static void plot_collect(int x, int y, void *ctx)
{
    collect_t *c = ctx;
    if (c->n < NPART) { p_tx[c->n] = (int16_t)x; p_ty[c->n] = (int16_t)y; c->n++; }
}

/* cibles = pixels de une ou deux lignes de texte ; départs sur un cercle */
static void particles_text(const char *l1, int y1, int sc1, const char *l2, int y2, int sc2)
{
    collect_t c = { 0 };
    g_text_walk(l1, (W - g_text_width(l1, sc1)) / 2, y1, sc1, plot_collect, &c);
    if (l2) g_text_walk(l2, (W - g_text_width(l2, sc2)) / 2, y2, sc2, plot_collect, &c);
    s_np = c.n;
    for (int i = 0; i < s_np; i++) {
        float a = g_frnd() * 2 * PI, r = 70 + g_frnd() * 60;
        p_sx[i] = 60 + cosf(a) * r;
        p_sy[i] = 60 + sinf(a) * r;
    }
}

/* e : 0 → 1 rassemblement, boom : 0 → 1 explosion */
static void particles_draw(float e, float boom, uint32_t ms, uint16_t base)
{
    float sw = (1 - e) * 2.4f, cs = cosf(sw), sn = sinf(sw);
    float sweep = fmodf(ms / 900.0f, 2.0f) * 160 - 20;
    for (int i = 0; i < s_np; i++) {
        float cx = p_sx[i] - 60, cy = p_sy[i] - 60;
        float rx = 60 + cx * cs - cy * sn, ry = 60 + cx * sn + cy * cs;
        float x = rx + (p_tx[i] - rx) * e, y = ry + (p_ty[i] - ry) * e;
        if (boom > 0) { x += (x - 60) * boom * 2.5f; y += (y - 60) * boom * 2.5f; }
        int hl = (int)(220 - fabsf(x - sweep) * 9);
        g_put(&S, (int)x, (int)y, g_white(base, hl < 0 ? 0 : hl));
    }
}

/* ------------------------------------------------------------------ étoiles */
#define NSTAR 140
static float st_x[NSTAR], st_y[NSTAR], st_z[NSTAR];

static void stars_init(void)
{
    for (int i = 0; i < NSTAR; i++) { st_x[i] = g_frnd() * 2 - 1; st_y[i] = g_frnd() * 2 - 1; st_z[i] = 0.05f + g_frnd(); }
}

static void stars_draw(float speed, float dt)
{
    for (int y = 0; y < H; y++) {
        uint16_t c = g_rgb(4, 4 + y / 12, 16 + y / 5);
        for (int x = 0; x < W; x++) S.px[y * W + x] = c;
    }
    for (int i = 0; i < NSTAR; i++) {
        float pz = st_z[i];
        st_z[i] -= speed * dt;
        if (st_z[i] < 0.03f) { st_x[i] = g_frnd() * 2 - 1; st_y[i] = g_frnd() * 2 - 1; st_z[i] = 1.0f; pz = 1.0f; }
        float sx = 60 + st_x[i] / st_z[i] * 50, sy = 60 + st_y[i] / st_z[i] * 50;
        float px = 60 + st_x[i] / pz * 50, py = 60 + st_y[i] / pz * 50;
        int b = (int)(255 * (1.0f - st_z[i]));
        uint16_t c = g_rgb(b, b, b + 40);
        int n = (int)(fabsf(sx - px) + fabsf(sy - py)) + 1;
        if (n > 12) n = 12;
        for (int k = 0; k <= n; k++) {
            float u = k / (float)n;
            g_put(&S, (int)(px + (sx - px) * u), (int)(py + (sy - py) * u), g_blend(g_rgb(0, 0, 40), c, (int)(u * 256)));
        }
    }
}

static int fade_in_out(uint32_t ms, uint32_t a, uint32_t b, uint32_t f)
{
    if (ms < a || ms > b) return 0;
    if (ms < a + f) return (int)((ms - a) * 256 / f);
    if (ms > b - f) return (int)((b - ms) * 256 / f);
    return 256;
}

static void scene_intro(uint32_t ms, float dt)
{
    float speed = 0.25f + 1.6f * (ms / (float)BAR(4)) * (ms / (float)BAR(4));
    stars_draw(speed, dt);
    int a = fade_in_out(ms, 600, 3800, 600);
    if (a) {
        uint16_t c = g_blend(0, g_rgb(170, 190, 255), a);
        g_text_c(&S, _("SEB & CLAUDE"), 46, 1, c, false);
        g_text_c(&S, _("PRÉSENTENT"), 60, 1, c, false);
    }
    if (ms > 3200) {
        float e = ease((ms - 3200) / 3000.0f);
        float boom = ms > BAR(4) - 450 ? (ms - (BAR(4) - 450)) / 450.0f : 0;
        particles_draw(e, boom, ms, g_rgb(90 + (int)(120 * e), 200, 110));
        if (e >= 1 && boom == 0) g_text_c(&S, "T-WATCH 2020", 92, 1, g_rgb(120, 160, 120), false);
    }
}

/* ------------------------------------------------------------------ tunnel */
static uint16_t s_pal[256];

static void textures_init(void)
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
    /* entrelacs : rubans croisés sur une grille de 32, bords sombres */
    for (int v = 0; v < 128; v++)
        for (int u = 0; u < 128; u++) {
            int cu = u & 31, cv2 = v & 31;
            float d1 = fabsf(((cu + cv2) & 31) - 16.0f), d2 = fabsf(((cu - cv2 + 64) & 31) - 16.0f);
            bool over = ((u >> 5) + (v >> 5)) & 1;
            float a = d1 < 6 ? 1 - d1 / 6 : 0, b = d2 < 6 ? 1 - d2 / 6 : 0;
            float k = over ? (a > 0.1f ? a : b * (d1 > 7 ? 1 : 0.3f)) : (b > 0.1f ? b : a * (d2 > 7 ? 1 : 0.3f));
            float base = 0.15f + 0.1f * sinf(u * 0.2f) * sinf(v * 0.2f);
            float val = k > 0.05f ? 0.35f + 0.65f * k : base;
            tex[v * 128 + u] = (uint8_t)(clampf(val, 0, 1) * 255);
        }
}

static void make_pal(float shift, float sat)
{
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        float r = 0.5f + 0.5f * cosf(2 * PI * (t * 0.9f + shift));
        float g = 0.5f + 0.5f * cosf(2 * PI * (t * 0.9f + shift + 0.33f));
        float b = 0.5f + 0.5f * cosf(2 * PI * (t * 0.9f + shift + 0.67f));
        float l = t;
        s_pal[i] = g_rgb((int)((r * sat + (1 - sat)) * 255 * l + 40 * l), (int)((g * sat + (1 - sat)) * 255 * l + 60 * l), (int)((b * sat + (1 - sat)) * 255 * l));
    }
}

static void big_word(const char *s, int y, uint16_t col)
{
    g_text_c(&S, s, y, g_text_fit(s, 2, W - 2), col, true);
}

static void scene_tunnel(uint32_t ms)
{
    float t = (ms - BAR(4)) / 1000.0f;
    make_pal(t * 0.08f, 1);
    int du = (int)(t * 90), dv = (int)(t * 26);
    int bright = (int)((demo_synth_kick() + demo_synth_snare() * 0.6f) * 70);
    int8_t twist[256];
    for (int d = 0; d < 256; d++) twist[d] = (int8_t)(sinf(d * 0.05f + t * 1.7f) * 18);
    for (int i = 0; i < W * H; i++) {
        int d = t_dep[i];
        int a = t_ang[i] + twist[d];
        int c = tex[((d + du) & 127) * 128 + ((a / 2 + dv) & 127)];
        int idx = (c * t_shd[i] >> 8) + bright;
        S.px[i] = s_pal[idx > 255 ? 255 : idx];
    }
    if (t < 0.25f) g_flash(&S, (int)((0.25f - t) * 4 * 256));
    static const char *const WORDS[] = { N_("100 %"), N_("CALCULÉ"), N_("SUR LA"), N_("MONTRE") };
    int w = (int)(t / (DEMO_BAR_MS / 1000.0f));
    if (w >= 0 && w < 4 && fmodf(t, DEMO_BAR_MS / 1000.0f) < 1.4f) big_word(_(WORDS[w]), 54, 0xFFFF);
}

/* ------------------------------------------------------------------ rotozoom sur plasma */
static void scene_roto(uint32_t ms)
{
    float t = (ms - BAR(8)) / 1000.0f;
    float kick = demo_synth_kick();
    float ang = t * 0.6f + sinf(t * 0.4f) * 0.8f;
    float zoom = 1.0f + 0.5f * sinf(t * 0.7f) - kick * 0.1f;
    float cu = cosf(ang) * zoom, su = sinf(ang) * zoom;
    float ox = 64 + t * 22, oy = 64 + sinf(t * 0.5f) * 40;
    uint8_t px[W], py[H];
    for (int i = 0; i < W; i++) px[i] = (uint8_t)(64 + 63 * sinf(i * 0.06f + t * 1.3f));
    for (int i = 0; i < H; i++) py[i] = (uint8_t)(64 + 63 * sinf(i * 0.045f - t * 0.9f));
    /* palette or / vert / sarcelle */
    for (int i = 0; i < 256; i++) {
        float k = i / 255.0f, h = k * 2 * PI + t * 0.5f;
        s_pal[i] = g_rgb((int)((120 + 120 * sinf(h)) * k + 30), (int)((150 + 100 * sinf(h + 1.6f)) * k + 20), (int)((90 + 90 * sinf(h + 3.4f)) * k + 25));
    }
    for (int y = 0; y < H; y++) {
        float u = ox + (-60) * cu - (y - 60) * su, v = oy + (-60) * su + (y - 60) * cu;
        int32_t U = (int32_t)(u * 256), V = (int32_t)(v * 256), dU = (int32_t)(cu * 256), dV = (int32_t)(su * 256);
        uint16_t *row = S.px + y * W;
        for (int x = 0; x < W; x++) {
            int c = tex[((V >> 8) & 127) * 128 + ((U >> 8) & 127)];
            int p = (px[x] + py[y]) >> 1;
            int idx = (c + p * 3) >> 2;
            idx += (int)(kick * 40);
            row[x] = s_pal[idx > 255 ? 255 : idx];
            U += dU; V += dV;
        }
    }
    if (t < 0.2f) g_flash(&S, (int)((0.2f - t) * 5 * 256));
    if (t > 1.0f && t < 6.5f) {
        int a = fade_in_out(ms, BAR(8) + 1000, BAR(8) + 6500, 400);
        g_text_c(&S, _("ROTOZOOM"), 6, 2, g_blend(0, 0xFFFF, a), true);
        g_text_c(&S, _("SUR PLASMA"), 104, 1, g_blend(0, 0xFFFF, a), true);
    }
}

/* ------------------------------------------------------------------ objets 3D pleins */
typedef struct { int16_t x, y; } p2_t;
typedef struct { uint16_t a, b, c, d; float z; uint16_t col; } face_t;
#define MAXV 600
#define MAXF 560
static p2_t *s_pv;
static float *s_vx, *s_vy, *s_vz;
static face_t *s_face;

static int face_cmp(const void *a, const void *b)
{
    float d = ((const face_t *)b)->z - ((const face_t *)a)->z;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

typedef struct { float cx, sx, cy, sy, cz, sz; } rot_t;
static rot_t rot_make(float ax, float ay, float az)
{
    return (rot_t){ cosf(ax), sinf(ax), cosf(ay), sinf(ay), cosf(az), sinf(az) };
}
static void rot_apply(const rot_t *r, float *x, float *y, float *z)
{
    float y1 = *y * r->cx - *z * r->sx, z1 = *y * r->sx + *z * r->cx;
    float x2 = *x * r->cy + z1 * r->sy, z2 = -*x * r->sy + z1 * r->cy;
    float x3 = x2 * r->cz - y1 * r->sz, y3 = x2 * r->sz + y1 * r->cz;
    *x = x3; *y = y3; *z = z2;
}

/* maillage en grille nu×nv refermé sur lui-même (tore, tube du nœud) */
static void draw_grid_mesh(int nu, int nv, float cam_d, float scale, float hue_t, int style)
{
    for (int k = 0; k < nu * nv; k++) {
        float d = cam_d + s_vz[k];
        s_pv[k].x = (int16_t)(60 + s_vx[k] / d * scale);
        s_pv[k].y = (int16_t)(60 + s_vy[k] / d * scale);
    }
    const float lx = -0.4f, ly = -0.55f, lz = -0.73f;
    int nf = 0;
    for (int i = 0; i < nu; i++)
        for (int j = 0; j < nv; j++) {
            int a = i * nv + j, b = ((i + 1) % nu) * nv + j, c = ((i + 1) % nu) * nv + (j + 1) % nv, d = i * nv + (j + 1) % nv;
            int cr = (s_pv[b].x - s_pv[a].x) * (s_pv[d].y - s_pv[a].y) - (s_pv[b].y - s_pv[a].y) * (s_pv[d].x - s_pv[a].x);
            int cr2 = (s_pv[c].x - s_pv[b].x) * (s_pv[d].y - s_pv[b].y) - (s_pv[c].y - s_pv[b].y) * (s_pv[d].x - s_pv[b].x);
            if (cr >= 0 && cr2 >= 0) continue;
            float e1x = s_vx[b] - s_vx[a], e1y = s_vy[b] - s_vy[a], e1z = s_vz[b] - s_vz[a];
            float e2x = s_vx[d] - s_vx[a], e2y = s_vy[d] - s_vy[a], e2z = s_vz[d] - s_vz[a];
            float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
            float nl = sqrtf(nx * nx + ny * ny + nz * nz) + 1e-6f;
            float l = (nx * lx + ny * ly + nz * lz) / nl;
            if (l < 0) l = -l * 0.2f;
            float spec = l > 0.88f ? (l - 0.88f) * 8 : 0;
            int rr, gg, bb;
            if (style == 0) {                                    /* tore : anneaux colorés */
                float h = i / (float)nu + hue_t;
                rr = (int)(140 + 110 * sinf(h * 2 * PI)); gg = (int)(60 + 50 * sinf(h * 2 * PI + 2.1f)); bb = (int)(170 + 80 * sinf(h * 2 * PI + 4.2f));
            } else {                                             /* nœud : or et émeraude */
                float h = sinf(i * 2 * PI / nu * 3 + hue_t * 6) * 0.5f + 0.5f;
                rr = (int)(60 + 170 * h); gg = (int)(150 + 50 * h); bb = (int)(70 - 30 * h);
                if ((j & 1) && style == 1) { rr = rr * 7 / 8; gg = gg * 7 / 8; bb = bb * 7 / 8; }
            }
            float k = 0.12f + 0.88f * l;
            uint16_t col = g_white(g_rgb((int)(rr * k), (int)(gg * k), (int)(bb * k)), (int)(spec * 230 > 230 ? 230 : spec * 230));
            if (nf < MAXF) {
                s_face[nf] = (face_t){ (uint16_t)a, (uint16_t)b, (uint16_t)c, (uint16_t)d,
                                       s_vz[a] + s_vz[b] + s_vz[c] + s_vz[d], col };
                nf++;
            }
        }
    qsort(s_face, nf, sizeof s_face[0], face_cmp);
    for (int f = 0; f < nf; f++) {
        face_t *q = &s_face[f];
        g_tri(&S, s_pv[q->a].x, s_pv[q->a].y, s_pv[q->b].x, s_pv[q->b].y, s_pv[q->c].x, s_pv[q->c].y, q->col);
        g_tri(&S, s_pv[q->a].x, s_pv[q->a].y, s_pv[q->c].x, s_pv[q->c].y, s_pv[q->d].x, s_pv[q->d].y, q->col);
    }
}

static void bg_halo(float t, float kick, int r0, int g0, int b0)
{
    int8_t px[W], py[H];
    for (int i = 0; i < W; i++) px[i] = (int8_t)(sinf(i * 0.09f + t) * 8);
    for (int i = 0; i < H; i++) py[i] = (int8_t)(sinf(i * 0.07f - t * 1.3f) * 8);
    int kk = (int)(kick * 25);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int dx = x - 60, dy = y - 60;
            int k = 70 - ((dx * dx + dy * dy) >> 6) + px[x] + py[y] + kk;
            if (k < 0) k = 0;
            S.px[y * W + x] = g_rgb(k * r0 / 8 + 6, k * g0 / 8 + 2, k * b0 / 8 + 14);
        }
}

#define TU 24
#define TV 12
static void scene_torus(uint32_t ms)
{
    float t = (ms - BAR(12)) / 1000.0f;
    float kick = demo_synth_kick();
    bg_halo(t, kick, 4, 1, 8);
    float R = 1.0f, r = 0.42f * (1 + 0.18f * kick);
    rot_t ro = rot_make(t * 0.9f, t * 0.63f, t * 0.31f);
    for (int i = 0; i < TU; i++)
        for (int j = 0; j < TV; j++) {
            float u = i * 2 * PI / TU, v = j * 2 * PI / TV;
            float x = (R + r * cosf(v)) * cosf(u), y = (R + r * cosf(v)) * sinf(u), z = r * sinf(v);
            rot_apply(&ro, &x, &y, &z);
            int k = i * TV + j;
            s_vx[k] = x; s_vy[k] = y; s_vz[k] = z;
        }
    draw_grid_mesh(TU, TV, 3.2f, 82, t * 0.05f, 0);
    if (t < 0.2f) g_flash(&S, (int)((0.2f - t) * 5 * 256));
    if (t > 1.5f && t < 6.5f) g_text_c(&S, _("RENDU LOGICIEL"), 110, 1, g_rgb(200, 170, 255), true);
}

/* nœud de trèfle : un tube de 48 × 8 facettes */
#define KU 48
#define KV 8
static void knot_point(float t, float *x, float *y, float *z)
{
    *x = sinf(t) + 2 * sinf(2 * t);
    *y = cosf(t) - 2 * cosf(2 * t);
    *z = -sinf(3 * t);
}

static void scene_knot(uint32_t ms)
{
    float t = (ms - BAR(48)) / 1000.0f;
    float kick = demo_synth_kick();
    /* fond : rayons qui tournent */
    float ra = t * 0.4f;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            int a = (t_ang[i] + (int)(ra * 40)) & 31;
            int k = (a < 16 ? 30 : 12) + (int)(kick * 20) - (t_shd[i] >> 4);
            S.px[i] = g_rgb(k / 2, k, k / 2 + 4);
        }
    rot_t ro = rot_make(t * 0.5f, t * 0.37f, t * 0.23f);
    float rt = 0.3f + 0.05f * kick;
    for (int i = 0; i < KU; i++) {
        float u = i * 2 * PI / KU;
        float px, py, pz, qx, qy, qz;
        knot_point(u, &px, &py, &pz);
        knot_point(u + 0.01f, &qx, &qy, &qz);
        float tx = qx - px, ty = qy - py, tz = qz - pz, tl = sqrtf(tx * tx + ty * ty + tz * tz);
        tx /= tl; ty /= tl; tz /= tl;
        /* normale : T × axe z, puis binormale */
        float nx = ty, ny = -tx, nz = 0, nl = sqrtf(nx * nx + ny * ny) + 1e-6f;
        nx /= nl; ny /= nl;
        float bx = ty * nz - tz * ny, by = tz * nx - tx * nz, bz = tx * ny - ty * nx;
        for (int j = 0; j < KV; j++) {
            float v = j * 2 * PI / KV, cv2 = cosf(v) * rt, sv = sinf(v) * rt;
            float x = (px + nx * cv2 + bx * sv) * 0.42f, y = (py + ny * cv2 + by * sv) * 0.42f, z = (pz + nz * cv2 + bz * sv) * 0.42f;
            rot_apply(&ro, &x, &y, &z);
            int k = i * KV + j;
            s_vx[k] = x; s_vy[k] = y; s_vz[k] = z;
        }
    }
    draw_grid_mesh(KU, KV, 3.2f, 100, t * 0.1f, 1);
    if (t < 0.25f) g_flash(&S, (int)((0.25f - t) * 4 * 256));
    int a = fade_in_out(ms, BAR(48) + 1500, BAR(48) + 7000, 500);
    if (a) g_text_c(&S, _("LE NŒUD CELTE"), 108, 1, g_blend(0, g_rgb(255, 225, 140), a), true);
}

/* ------------------------------------------------------------------ nuée de points */
#define NDOT 700
static void dot_shape(int s, int i, float *x, float *y, float *z)
{
    float u = i / (float)NDOT;
    switch (s) {
    case 0: {                                                  /* sphère */
        float yy = 1 - 2 * u, r = sqrtf(1 - yy * yy), ph = i * 2.39996f;
        *x = cosf(ph) * r; *y = yy; *z = sinf(ph) * r;
        break;
    }
    case 1: {                                                  /* tore */
        float a = (i % 35) * 2 * PI / 35, b = (i / 35) * 2 * PI / 20;
        *x = (0.75f + 0.32f * cosf(b)) * cosf(a); *y = (0.75f + 0.32f * cosf(b)) * sinf(a); *z = 0.32f * sinf(b);
        break;
    }
    case 2: {                                                  /* cube */
        int f = i % 6, k = i / 6;
        float a = (k % 11) / 5.0f - 1, b = ((k / 11) % 11) / 5.0f - 1;
        float c = (f & 1) ? 1 : -1;
        float v[3];
        v[f / 2] = c; v[(f / 2 + 1) % 3] = a; v[(f / 2 + 2) % 3] = b;
        *x = v[0] * 0.7f; *y = v[1] * 0.7f; *z = v[2] * 0.7f;
        break;
    }
    default: {                                                 /* galaxie spirale */
        float r = sqrtf(u) * 1.15f;
        float a = (i & 1 ? PI : 0) + r * 5.5f + ((i * 7919) % 100) / 100.0f * 0.5f;
        *x = cosf(a) * r; *z = sinf(a) * r; *y = (((i * 104729) % 100) / 100.0f - 0.5f) * 0.12f * (1.2f - r);
        break;
    }
    }
}

static void copper(float t)
{
    for (int y = 0; y < H; y++) {
        int r = 3, g = 2, b = 10;
        for (int k = 0; k < 3; k++) {
            float cy = 60 + sinf(t * (0.9f + k * 0.37f) + k * 2.1f) * 46;
            float d = fabsf(y - cy);
            if (d < 9) {
                int e = (int)((9 - d) * 22);
                static const uint8_t C[3][3] = { { 255, 80, 60 }, { 60, 200, 120 }, { 80, 120, 255 } };
                r += C[k][0] * e >> 8; g += C[k][1] * e >> 8; b += C[k][2] * e >> 8;
            }
        }
        uint16_t c = g_rgb(r, g, b);
        for (int x = 0; x < W; x++) S.px[y * W + x] = c;
    }
}

static void scene_dots(uint32_t ms)
{
    float t = (ms - BAR(16)) / 1000.0f;
    float kick = demo_synth_kick();
    copper(t);
    int phase = (int)((ms - BAR(16)) / BAR(2));
    if (phase > 3) phase = 3;
    float m = ease(((ms - BAR(16)) - phase * BAR(2)) / 1200.0f);
    int from = phase == 0 ? 3 : phase - 1, to = phase;
    if (phase == 0 && ms - BAR(16) < 1200) m = ease((ms - BAR(16)) / 1200.0f);
    rot_t ro = rot_make(t * 0.7f + 0.4f, t * 0.9f, t * 0.2f);
    float sc = 1 + kick * 0.12f;
    for (int i = 0; i < NDOT; i++) {
        float ax, ay, az, bx, by, bz;
        dot_shape(from, i, &ax, &ay, &az);
        dot_shape(to, i, &bx, &by, &bz);
        float x = (ax + (bx - ax) * m) * sc, y = (ay + (by - ay) * m) * sc, z = (az + (bz - az) * m) * sc;
        if (phase == 0 && ms - BAR(16) < 1200) { x = bx * m * sc; y = by * m * sc; z = bz * m * sc; }
        rot_apply(&ro, &x, &y, &z);
        float d = 3.0f + z;
        int sx = (int)(60 + x / d * 120), sy = (int)(60 + y / d * 120);
        int b = (int)(200 - z * 80);
        b = b < 40 ? 40 : b > 255 ? 255 : b;
        float h = i / (float)NDOT * 2 * PI + t;
        uint16_t c = g_rgb((int)((160 + 95 * sinf(h)) * b / 255), (int)((160 + 95 * sinf(h + 2.1f)) * b / 255), (int)((160 + 95 * sinf(h + 4.2f)) * b / 255));
        g_glow(&S, sx, sy, c);
        if (z < 0) { g_glow(&S, sx + 1, sy, c); g_glow(&S, sx, sy + 1, c); g_glow(&S, sx + 1, sy + 1, c); }
    }
    if (ms - BAR(16) < 250) g_flash(&S, (int)((250 - (ms - BAR(16))) * 256 / 250));
    static const char *const NAMES[] = { N_("SPHÈRE"), N_("TORE"), N_("CUBE"), N_("GALAXIE") };
    int a = fade_in_out(ms - BAR(16) - phase * BAR(2), 1300, BAR(2) - 200, 300);
    if (a) g_text_c(&S, _(NAMES[phase]), 108, 1, g_blend(0, 0xFFFF, a), true);
    if (phase == 0 && a) g_text_c(&S, _("700 POINTS"), 6, 1, g_blend(0, 0xFFFF, a), true);
}

/* ------------------------------------------------------------------ métaballes */
static void scene_meta(uint32_t ms)
{
    float t = (ms - BAR(24)) / 1000.0f;
    float bx[5], by[5], br[5];
    for (int k = 0; k < 5; k++) {
        bx[k] = 30 + 22 * sinf(t * (0.5f + k * 0.13f) + k * 1.7f) + k * 0.2f;
        by[k] = 30 + 20 * cosf(t * (0.43f + k * 0.11f) + k * 2.3f);
        br[k] = 26 + k * 7;
    }
    for (int y = 0; y < 60; y++)
        for (int x = 0; x < 60; x++) {
            float f = 0;
            for (int k = 0; k < 5; k++) {
                float dx = x - bx[k], dy = y - by[k];
                f += br[k] / (dx * dx + dy * dy + 1);
            }
            int v = (int)(f * 120);
            uint16_t c;
            if (v < 100) c = g_rgb(v / 6, v / 4 + 6, v / 3 + 14);                 /* nuit */
            else if (v < 120) c = g_rgb(255, 220, 140);                           /* liseré doré */
            else { int e = v > 400 ? 400 : v; c = g_rgb(30 + e / 8, 110 + e / 4, 80 + e / 6); }
            uint16_t *p = S.px + (2 * y) * W + 2 * x;
            p[0] = p[1] = p[W] = p[W + 1] = c;
        }
    static const char *const CRED[][2] = {
        { N_("MUSIQUE"), N_("SYNTHÉ 4 VOIES") }, { N_("IMAGES"), N_("120 × 120, ×2") },
        { N_("CODE"), N_("SEB & CLAUDE") }, { N_("POIDS"), N_("0 OCTET D'IMAGE") },
    };
    int k = (int)((ms - BAR(24)) / BAR(1));
    if (k > 3) k = 3;
    int a = fade_in_out(ms - BAR(24) - k * BAR(1), 150, BAR(1) - 150, 300);
    if (a) {
        g_text_c(&S, _(CRED[k][0]), 44, 2, g_blend(0, 0xFFFF, a), true);
        g_text_c(&S, _(CRED[k][1]), 66, 1, g_blend(0, g_rgb(255, 230, 160), a), true);
    }
}

/* ------------------------------------------------------------------ feu */
static uint16_t s_fpal[256];
static void plot_heat(int x, int y, void *ctx)
{
    if ((unsigned)x < W && (unsigned)y < H) fire[y * W + x] = 255;
}

static void scene_fire(uint32_t ms)
{
    float t = (ms - BAR(28)) / 1000.0f;
    static bool pal;
    if (!pal) {
        pal = true;
        for (int i = 0; i < 256; i++) {
            int r = i * 3, g = (i - 85) * 3, b = (i - 170) * 3;
            s_fpal[i] = g_rgb(r, g < 0 ? 0 : g, b < 0 ? 0 : b);
        }
    }
    if (ms - BAR(28) < 60) memset(fire, 0, W * (H + 2));
    /* foyer en bas, plus fort avec le roulement */
    int heat = 160 + (int)(t / 7.68f * 95);
    for (int x = 0; x < W; x++) {
        fire[(H + 1) * W + x] = (uint8_t)((g_rnd() & 1) ? heat : heat / 3);
        fire[H * W + x] = (uint8_t)((g_rnd() & 3) ? heat : 0);
    }
    /* le titre brûle */
    float rise = clampf((t - 1.0f) / 5.0f, 0, 1);
    int ty = 70 - (int)(rise * 22);
    g_text_walk("TÍR NA", (W - g_text_width("TÍR NA", 2)) / 2, ty, 2, plot_heat, NULL);
    g_text_walk("NÓG", (W - g_text_width("NÓG", 3)) / 2, ty + 22, 3, plot_heat, NULL);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int s = fire[(y + 1) * W + (x ? x - 1 : x)] + fire[(y + 1) * W + x] + fire[(y + 1) * W + (x < W - 1 ? x + 1 : x)] + fire[(y + 2) * W + x];
            int v = (s >> 2) - 2;
            fire[y * W + x] = (uint8_t)(v < 0 ? 0 : v);
        }
    for (int i = 0; i < W * H; i++) S.px[i] = s_fpal[fire[i]];
    g_text_c(&S, "TÍR NA", ty, 2, g_rgb(255, 250, 220), false);
    g_text_c(&S, "NÓG", ty + 22, 3, g_rgb(255, 250, 220), false);
    if (ms > BAR(32) - 300) g_flash(&S, (int)((ms - (BAR(32) - 300)) * 256 / 300));
}

/* ------------------------------------------------------------------ paysage */
static vox_cam_t s_cam;
static bool s_cam_set;
static float s_turn;

static void scene_voxel(uint32_t ms, float dt)
{
    float t = (ms - BAR(32)) / 1000.0f;
    if (vox_progress() < 100) { scene_fire(BAR(32) - 1000); return; }
    if (!s_cam_set) {
        s_cam_set = true;
        vox_find_valley(&s_cam.x, &s_cam.y, &s_cam.a);
        s_cam.h = vox_ground(s_cam.x, s_cam.y) + 40;
    }
    /* pilote automatique : suit les vallées */
    float hl = 0, hr = 0;
    for (int k = 1; k <= 3; k++) {
        float d = 25.0f * k;
        hl += vox_ground(s_cam.x + cosf(s_cam.a - 0.45f) * d, s_cam.y + sinf(s_cam.a - 0.45f) * d);
        hr += vox_ground(s_cam.x + cosf(s_cam.a + 0.45f) * d, s_cam.y + sinf(s_cam.a + 0.45f) * d);
    }
    float turn = clampf((hl - hr) * 0.004f + sinf(t * 0.31f) * 0.3f, -0.9f, 0.9f);
    s_turn += (turn - s_turn) * 0.06f;
    s_cam.a += s_turn * dt * 0.9f;
    s_cam.roll += (s_turn * 0.6f - s_cam.roll) * 0.1f;
    s_cam.x += cosf(s_cam.a) * 40 * dt;
    s_cam.y += sinf(s_cam.a) * 40 * dt;
    float ground = 0;
    for (int k = 0; k < 8; k++) {
        float g = vox_ground(s_cam.x + cosf(s_cam.a) * k * 9, s_cam.y + sinf(s_cam.a) * k * 9) - k * 2.0f;
        if (g > ground) ground = g;
    }
    s_cam.h += (ground + 40 - s_cam.h) * 0.06f;
    if (s_cam.h < ground + 12) s_cam.h = ground + 12;
    s_cam.pitch = 0;
    vox_render(&S, NULL, &s_cam, 70, t);
    if (t < 0.3f) g_flash(&S, (int)((0.3f - t) * 3.3f * 256));
    /* défilement sinusoïdal, vitesse calée sur la durée de la scène */
    const char *msg = _("TÍR NA NÓG · UNE INTRO POUR T-WATCH 2020 · TOUT EST CALCULÉ ICI, MÊME LA MUSIQUE · "
                        "LE PAYSAGE EST INVENTÉ À CHAQUE LANCEMENT : PRÉS, HAIES, FERMES, RIVIÈRES · "
                        "SALUTATIONS À LILYGO · ESPRESSIF · LA DÉMOSCÈNE · "
                        "ET AUX AMIS DES QSO DES GASCONS · BONS 73 ! ·         ");
    if (t > 1.5f) {
        float span = W + g_text_width(msg, 2);
        float off = (t - 1.5f) * span / (BAR(16) / 1000.0f - 2.0f);
        int x = W - (int)off;
        const char *s = msg;
        while (*s && x < W) {
            int n = g_char_len(s);
            if (x > -12) {
                char one[5] = { 0 };
                memcpy(one, s, (size_t)(n < 4 ? n : 4));
                int y = 92 + (int)(sinf(x * 0.08f + t * 4) * 6);
                g_text(&S, one, x, y, 2, g_rgb(255, 255, 220), true);
            }
            x += 12;
            s += n;
        }
    }
}

/* ------------------------------------------------------------------ fin */
static bool s_outro_init;
static void scene_outro(uint32_t ms, float dt)
{
    if (!s_outro_init) { s_outro_init = true; particles_text(_("MERCI !"), 40, g_text_fit(_("MERCI !"), 3, W - 4), NULL, 0, 0); }
    float t = (ms - BAR(56)) / 1000.0f;
    stars_draw(0.15f, dt);
    if (t < 0.3f) g_flash(&S, (int)((0.3f - t) * 3.3f * 256));
    float e = ease((t - 0.5f) / 3.5f);
    if (t > 0.5f) particles_draw(e, 0, ms, g_rgb(255, 200, 90));
    int a = fade_in_out(ms, BAR(56) + 5000, BAR(64), 800);
    if (a) {
        uint16_t c = g_blend(0, g_rgb(170, 190, 255), a);
        g_text_c(&S, _("TÍR NA NÓG"), 78, 1, c, false);
        g_text_c(&S, _("SEB & CLAUDE · 2026"), 92, 1, c, false);
        g_text_c(&S, "T-WATCH 2020", 104, 1, c, false);
    }
    if (ms > BAR(62)) g_fade(&S, (int)((ms - BAR(62)) * 256 / BAR(2)));
}

static void scene_end(uint32_t ms, float dt)
{
    stars_draw(0.1f, dt);
    int a = (int)((ms - DEMO_END_MS) * 256 / 1500);
    if (a > 256) a = 256;
    g_text_c(&S, "TÍR NA NÓG", 48, 1, g_blend(0, g_rgb(120, 200, 120), a), false);
    if ((ms / 600) & 1) g_text_c(&S, _("TOUCHE POUR REVOIR"), 66, 1, g_blend(0, g_rgb(150, 150, 170), a), false);
}

/* ------------------------------------------------------------------ boucle */
static void frame(void)
{
    if (!S.px || !cv) return;
    uint32_t ms = demo_synth_ms();
    float dt = (ms - s_last_ms) / 1000.0f;
    if (dt < 0 || dt > 0.1f) dt = 0.033f;
    s_last_ms = ms;
    if (ms < BAR(4)) scene_intro(ms, dt);
    else if (ms < BAR(8)) scene_tunnel(ms);
    else if (ms < BAR(12)) scene_roto(ms);
    else if (ms < BAR(16)) scene_torus(ms);
    else if (ms < BAR(24)) scene_dots(ms);
    else if (ms < BAR(28)) scene_meta(ms);
    else if (ms < BAR(32)) scene_fire(ms);
    else if (ms < BAR(48)) scene_voxel(ms, dt);
    else if (ms < BAR(56)) scene_knot(ms);
    else if (ms < BAR(64)) scene_outro(ms, dt);
    else scene_end(ms, dt);
    /* le paysage n'est plus utile après sa scène : on rend la mémoire */
    if (ms >= BAR(48) && s_vox) { vox_free(); s_vox = false; }
    if (ms < s_sound_ms) g_text(&S, s_sound ? _("SON") : _("MUET"), 92, 3, 1, 0xFFFF, true);
    g_scale(&S, cv, 240, 2, 2);
    lv_obj_invalidate(v_cv);
}

static void tick_cb(lv_timer_t *t) { frame(); }

static void restart(void)
{
    g_seed(0x7172A);
    stars_init();
    particles_text("TÍR NA", 40, 2, "NÓG", 62, 3);
    s_outro_init = false;
    s_cam = (vox_cam_t){ .x = 256, .y = 256, .h = 80 };
    s_cam_set = false;
    s_turn = 0;
    s_last_ms = 0;
    if (!s_vox) s_vox = vox_gen_start(esp_random());
    demo_synth_start(SONG_DEMO, s_sound);
}

static void tap_cb(lv_event_t *e)
{
    if (demo_synth_ms() >= DEMO_END_MS) { restart(); return; }
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
    if (s_vox) { vox_free(); s_vox = false; }
    heap_caps_free(S.px); heap_caps_free(cv);
    heap_caps_free(t_ang); heap_caps_free(t_dep); heap_caps_free(t_shd); heap_caps_free(tex); heap_caps_free(fire);
    heap_caps_free(p_tx); heap_caps_free(p_ty); heap_caps_free(p_sx); heap_caps_free(p_sy);
    heap_caps_free(s_pv); heap_caps_free(s_vx); heap_caps_free(s_vy); heap_caps_free(s_vz); heap_caps_free(s_face);
    S.px = NULL; cv = NULL;
    t_ang = t_dep = t_shd = tex = fire = NULL;
    p_tx = p_ty = NULL; p_sx = p_sy = NULL;
    s_pv = NULL; s_vx = s_vy = s_vz = NULL; s_face = NULL;
}

static void create(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    S = (gsurf_t){ alloc_int(W * H * 2), W, H };
    cv = alloc_ps(240 * 240 * 2);
    t_ang = alloc_ps(W * H); t_dep = alloc_ps(W * H); t_shd = alloc_ps(W * H);
    tex = alloc_ps(128 * 128);
    fire = alloc_ps(W * (H + 2));
    p_tx = alloc_ps(NPART * 2); p_ty = alloc_ps(NPART * 2);
    p_sx = alloc_ps(NPART * 4); p_sy = alloc_ps(NPART * 4);
    s_pv = alloc_ps(MAXV * sizeof(p2_t));
    s_vx = alloc_ps(MAXV * 4); s_vy = alloc_ps(MAXV * 4); s_vz = alloc_ps(MAXV * 4);
    s_face = alloc_ps(MAXF * sizeof(face_t));
    v_cv = lv_canvas_create(scr);
    if (!S.px || !cv || !t_ang || !t_dep || !t_shd || !tex || !fire || !p_tx || !p_ty || !p_sx || !p_sy
        || !s_pv || !s_vx || !s_vy || !s_vz || !s_face) {
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
    textures_init();
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
    if (!S.px) return;
    power_hold_active(true);                      /* 240 MHz pendant la démo */
    restart();
    if (!v_tmr) v_tmr = lv_timer_create(tick_cb, 20, NULL);
    else lv_timer_resume(v_tmr);
}

static void on_hide(void)
{
    if (v_tmr) lv_timer_pause(v_tmr);
    demo_synth_stop();
    if (s_vox) { vox_free(); s_vox = false; }
    if (S.px) power_hold_active(false);
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
