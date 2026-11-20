/*
 * app_flight.c — « Envol », jeu de vol au-dessus d'un paysage irlandais.
 *
 * On pilote en penchant le poignet (virage et inclinaison, montée et
 * descente) et on traverse des anneaux d'or avant la fin du chrono : chaque
 * anneau rend du temps, les enchaîner rapidement fait monter le combo, la
 * vitesse augmente au fil de la partie. Toucher l'écran donne un coup
 * d'accélérateur (il se recharge). Toucher le relief, c'est le crash. Le
 * record est gardé en mémoire.
 *
 * Le monde (voxel.c) est inventé à chaque lancement. Le relief est rendu en
 * 240×120 (pleine largeur, lignes doublées), avec un tampon de profondeur pour
 * cacher les anneaux derrière les collines ; l'interface est dessinée ensuite
 * à pleine résolution dans le canevas.
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
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "demo_gfx.h"
#include "demo_synth.h"
#include "voxel.h"

#define W  160                       /* colonnes rendues, élargies ×1,5 à l'écran */
#define H  120
#define PI 3.14159265f
#define FY 70.0f
#define NRING 3
#define RING_R 10.0f

typedef enum { ST_GEN, ST_TITLE, ST_COUNT, ST_PLAY, ST_CRASH, ST_OVER, ST_PAUSE } state_t;

typedef struct { float x, y, h, dir; } ring_t;

static gsurf_t S;                    /* relief 240×120 */
static gsurf_t C;                    /* canevas 240×240 (interface) */
static uint8_t *occ;                 /* occultation par le relief (voxel.c) */
static uint32_t s_fps_t0;
static int s_fps_n, s_fps;
static lv_obj_t *v_cv;
static lv_timer_t *v_tmr;
static bool s_tilt, s_sound = true, s_vox;

static state_t st;
static uint32_t st_ms, s_last_ms;
static vox_cam_t cam;
static float s_turn, s_climb, tilt0_x, tilt0_y;
static float speed, boost_t, boost_charge;
static float time_left;
static int score, combo, rings_n;
static uint32_t best;
static float last_ring_t, play_t;
static ring_t ring[NRING];
static bool s_new_best, s_timeout;
static char s_msg[32];
static float s_msg_t;
static float shake;

/* éclats (anneau traversé, crash) */
#define NSPARK 60
typedef struct { float x, y, vx, vy, life; uint16_t col; } spark_t;
static spark_t spark[NSPARK];

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* ------------------------------------------------------------------ record */
static void best_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof best;
    best = 0;
    if (nvs_open("flight", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "best", &best, &n) != ESP_OK || n != sizeof best) best = 0;
        nvs_close(h);
    }
}

static void best_save(void)
{
    nvs_handle_t h;
    if (nvs_open("flight", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "best", &best, sizeof best);
    nvs_commit(h);
    nvs_close(h);
}

/* ------------------------------------------------------------------ anneaux */
static void ring_place(ring_t *r, const ring_t *prev, float px, float py, float pdir)
{
    float dir = prev ? pdir + (g_frnd() - 0.5f) * 1.3f : pdir;     /* le premier : droit devant */
    float d = prev ? 110 + g_frnd() * 50 : 90;
    r->x = (prev ? prev->x : px) + cosf(dir) * d;
    r->y = (prev ? prev->y : py) + sinf(dir) * d;
    r->dir = dir;
    float g = 0;
    for (int k = 0; k < 9; k++) {
        float a = k * 2 * PI / 8, rr = k ? 16 : 0;
        float v = vox_ground(r->x + cosf(a) * rr, r->y + sinf(a) * rr);
        if (v > g) g = v;
    }
    r->h = clampf(g + 18 + g_frnd() * 26, 30, 215);
}

static void rings_init(void)
{
    ring_place(&ring[0], NULL, cam.x, cam.y, cam.a);
    for (int i = 1; i < NRING; i++) ring_place(&ring[i], &ring[i - 1], 0, 0, ring[i - 1].dir);
}

static void rings_shift(void)
{
    ring[0] = ring[1];
    ring[1] = ring[2];
    ring_place(&ring[2], &ring[1], 0, 0, ring[1].dir);
}

static void dline(int x0, int y0, float z0, int x1, int y1, float z1, int th, uint16_t c)
{
    int n = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    if (n > 400) return;
    for (int k = 0; k <= n; k++) {
        float u = n ? k / (float)n : 0;
        int x = x0 + (int)((x1 - x0) * u), y = y0 + (int)((y1 - y0) * u);
        float z = z0 + (z1 - z0) * u;
        if (!vox_visible(occ, W, x, y, z)) continue;
        for (int j = 0; j < th; j++)
            for (int i = 0; i < th; i++) {
                int X = x + i - th / 2, Y = y + (j - th / 2) / 2;
                if ((unsigned)X < W && (unsigned)Y < H) S.px[Y * W + X] = c;
            }
    }
}

static void ring_draw(const ring_t *r, int rank, float t)
{
    float pxs[25], pys[25], pzs[25];
    bool ok[25];
    float nx = -sinf(r->dir), ny = cosf(r->dir);
    float sx, sy, z;
    for (int i = 0; i <= 24; i++) {
        float a = i * 2 * PI / 24;
        ok[i] = vox_project(&S, &cam, FY, r->x + nx * cosf(a) * RING_R, r->y + ny * cosf(a) * RING_R,
                            r->h + sinf(a) * RING_R, &pxs[i], &pys[i], &pzs[i]);
    }
    if (!vox_project(&S, &cam, FY, r->x, r->y, r->h, &sx, &sy, &z)) return;
    int th = z < 40 ? 4 : z < 90 ? 3 : 2;
    float pulse = 0.5f + 0.5f * sinf(t * 6);
    uint16_t c = rank == 0 ? g_white(g_rgb(255, 196, 40), (int)(pulse * 120)) : rank == 1 ? g_rgb(200, 150, 50) : g_rgb(150, 110, 60);
    uint16_t dark = g_blend(c, 0, 110);
    for (int i = 0; i < 24; i++) {
        if (!ok[i] || !ok[i + 1]) continue;
        dline((int)pxs[i], (int)pys[i] + 1, pzs[i], (int)pxs[i + 1], (int)pys[i + 1] + 1, pzs[i + 1], th, dark);
        dline((int)pxs[i], (int)pys[i], pzs[i], (int)pxs[i + 1], (int)pys[i + 1], pzs[i + 1], th, c);
    }
}

/* ------------------------------------------------------------------ éclats */
static void sparks_burst(float x, float y, int n, uint16_t c0, uint16_t c1, float sp)
{
    for (int i = 0, k = 0; i < NSPARK && k < n; i++)
        if (spark[i].life <= 0) {
            float a = g_frnd() * 2 * PI, v = (0.3f + g_frnd()) * sp;
            spark[i] = (spark_t){ x, y, cosf(a) * v, sinf(a) * v * 0.6f, 0.6f + g_frnd() * 0.6f, g_blend(c0, c1, (int)(g_frnd() * 256)) };
            k++;
        }
}

static void sparks_draw(float dt)
{
    for (int i = 0; i < NSPARK; i++) {
        spark_t *p = &spark[i];
        if (p->life <= 0) continue;
        p->life -= dt;
        p->x += p->vx * dt; p->y += p->vy * dt;
        p->vy += 60 * dt;
        int k = (int)(clampf(p->life, 0, 1) * 256);
        g_glow(&S, (int)p->x, (int)p->y, g_blend(0, p->col, k));
        g_glow(&S, (int)p->x + 1, (int)p->y, g_blend(0, p->col, k));
    }
}

/* ------------------------------------------------------------------ pilotage */
static void read_tilt(float *tx, float *ty)
{
    *tx = *ty = 0;
    int16_t ax, ay, az;
    if (!s_tilt || !motion_accel_read(&ax, &ay, &az)) return;
    float fx = ax / 1024.0f, fy = ay / 1024.0f;
    tilt0_x += (fx - tilt0_x) * 0.002f;              /* le neutre suit lentement la pose */
    tilt0_y += (fy - tilt0_y) * 0.002f;
    *tx = clampf((fx - tilt0_x) * 2.4f, -1, 1);
    *ty = clampf((fy - tilt0_y) * 3.0f, -1, 1);
}

static void calibrate(void)
{
    int16_t ax, ay, az;
    if (s_tilt && motion_accel_read(&ax, &ay, &az)) { tilt0_x = ax / 1024.0f; tilt0_y = ay / 1024.0f; }
}

static void autopilot(float t, float dt)
{
    float hl = 0, hr = 0;
    for (int k = 1; k <= 3; k++) {
        float d = 25.0f * k;
        hl += vox_ground(cam.x + cosf(cam.a - 0.45f) * d, cam.y + sinf(cam.a - 0.45f) * d);
        hr += vox_ground(cam.x + cosf(cam.a + 0.45f) * d, cam.y + sinf(cam.a + 0.45f) * d);
    }
    float turn = clampf((hl - hr) * 0.004f + sinf(t * 0.31f) * 0.3f, -0.9f, 0.9f);
    s_turn += (turn - s_turn) * 0.06f;
    cam.a += s_turn * dt * 0.9f;
    cam.roll += (s_turn * 0.6f - cam.roll) * 0.1f;
    cam.x += cosf(cam.a) * 40 * dt;
    cam.y += sinf(cam.a) * 40 * dt;
    float ground = 0;
    for (int k = 0; k < 8; k++) {
        float g = vox_ground(cam.x + cosf(cam.a) * k * 9, cam.y + sinf(cam.a) * k * 9) - k * 2.0f;
        if (g > ground) ground = g;
    }
    cam.h += (ground + 42 - cam.h) * 0.06f;
    if (cam.h < ground + 12) cam.h = ground + 12;
    cam.pitch += (0 - cam.pitch) * 0.1f;
}

static void spawn(void)
{
    cam = (vox_cam_t){ 0 };
    vox_find_valley(&cam.x, &cam.y, &cam.a);
    cam.h = vox_ground(cam.x, cam.y) + 40;
}

static void set_state(state_t s)
{
    st = s;
    st_ms = demo_synth_ms();
}

static void game_over(bool timeout)
{
    s_timeout = timeout;
    s_new_best = (uint32_t)score > best;
    if (s_new_best) { best = (uint32_t)score; best_save(); }
    demo_synth_sfx(timeout ? SFX_OVER : SFX_CRASH);
    if (timeout) set_state(ST_OVER);
    else {
        set_state(ST_CRASH);
        shake = 1;
        sparks_burst(W / 2, H * 0.7f, 50, g_rgb(255, 80, 20), g_rgb(255, 230, 120), 90);
    }
}

static void play_step(float dt)
{
    float tx, ty;
    read_tilt(&tx, &ty);
    play_t += dt;
    s_turn += (tx - s_turn) * 0.15f;
    s_climb += (ty - s_climb) * 0.15f;
    cam.a += s_turn * 1.3f * dt;
    cam.roll += (s_turn * 0.64f - cam.roll) * 0.15f;
    if (boost_t > 0) boost_t -= dt;
    else if (boost_charge < 1) boost_charge = clampf(boost_charge + dt / 4.0f, 0, 1);
    float v = speed * (boost_t > 0 ? 1.7f : 1);
    float ox = cam.x, oy = cam.y, oh = cam.h;
    cam.x += cosf(cam.a) * v * dt;
    cam.y += sinf(cam.a) * v * dt;
    cam.h = clampf(cam.h + s_climb * 46 * dt, 0, 235);
    cam.pitch += (s_climb * 16 - cam.pitch) * 0.12f;
    /* anneau : segment parcouru proche du centre */
    ring_t *r = &ring[0];
    for (int k = 0; k <= 4; k++) {
        float u = k / 4.0f;
        float x = ox + (cam.x - ox) * u, y = oy + (cam.y - oy) * u, h = oh + (cam.h - oh) * u;
        float dx = x - r->x, dy = y - r->y, dh = h - r->h;
        dx -= VOX_N * roundf(dx / VOX_N); dy -= VOX_N * roundf(dy / VOX_N);
        if (dx * dx + dy * dy + dh * dh < RING_R * RING_R * 1.1f) {
            bool quick = play_t - last_ring_t < 6.0f;
            combo = quick ? combo + 1 : 1;
            last_ring_t = play_t;
            score += combo;
            rings_n++;
            float bonus = rings_n < 10 ? 5 : rings_n < 25 ? 4 : 3;
            time_left += bonus;
            speed = clampf(speed + 1.5f, 0, 85);
            if (combo > 1) snprintf(s_msg, sizeof s_msg, _("COMBO ×%d"), combo);
            else snprintf(s_msg, sizeof s_msg, "+%d S", (int)bonus);
            s_msg_t = 1.2f;
            demo_synth_sfx(SFX_RING);
            sparks_burst(W / 2, H / 2, 40, g_rgb(255, 210, 60), 0xFFFF, 110);
            rings_shift();
            break;
        }
    }
    /* anneau raté : loin derrière */
    {
        float sx, sy, z;
        float dx = r->x - cam.x, dy = r->y - cam.y;
        dx -= VOX_N * roundf(dx / VOX_N); dy -= VOX_N * roundf(dy / VOX_N);
        if (!vox_project(&S, &cam, FY, r->x, r->y, r->h, &sx, &sy, &z) && dx * dx + dy * dy > 70 * 70) {
            time_left -= 3;
            combo = 0;
            snprintf(s_msg, sizeof s_msg, "%s", _("RATÉ ! −3 S"));
            s_msg_t = 1.2f;
            demo_synth_sfx(SFX_TICK);
            ring_place(&ring[0], NULL, cam.x, cam.y, cam.a);
            ring_place(&ring[1], &ring[0], 0, 0, ring[0].dir);
            ring_place(&ring[2], &ring[1], 0, 0, ring[1].dir);
        }
    }
    float prev = time_left;
    time_left -= dt;
    if (time_left < 5 && (int)prev != (int)time_left) demo_synth_sfx(SFX_TICK);
    if (cam.h < vox_ground(cam.x, cam.y) + 2.5f) { game_over(false); return; }
    if (time_left <= 0) { time_left = 0; game_over(true); }
}

/* ------------------------------------------------------------------ dessin */
static void hud_text(const char *s, int x, int y, int sc, uint16_t c) { g_text(&C, s, x, y, sc, c, true); }
static void hud_c(const char *s, int y, int sc, uint16_t c) { g_text_c(&C, s, y, g_text_fit(s, sc, 232), c, true); }

static void flare(void)
{
    float d = remainderf(VOX_SUN_A - cam.a, 2 * PI);
    if (fabsf(d) > 0.75f) return;
    float sx = W / 2 + tanf(d) * W / 2, sy = H * 0.33f + cam.pitch - H * 0.12f + (sx - W / 2) * cam.roll;
    float vx = W / 2 - sx, vy = H / 2 - sy;
    static const float POS[] = { 0.5f, 0.9f, 1.3f, 1.7f };
    static const uint8_t RAD[] = { 3, 6, 2, 9 };
    for (int k = 0; k < 4; k++) {
        int cx = (int)(sx + vx * POS[k]), cy = (int)(sy + vy * POS[k]), r = RAD[k];
        uint16_t c = k & 1 ? g_rgb(18, 10, 4) : g_rgb(6, 14, 10);
        for (int y = -r / 2; y <= r / 2; y++)
            for (int x = -r; x <= r; x++)
                if (x * x + 4 * y * y <= r * r) g_glow(&S, cx + x, cy + y, c);
    }
}

static void speed_lines(float t)
{
    for (int k = 0; k < 14; k++) {
        float a = (k * 2.39996f) + t * 0.1f;
        float r0 = 50 + fmodf(t * 300 + k * 37, 80);
        for (int i = 0; i < 18; i++) {
            float r = r0 + i;
            g_glow(&S, (int)(W / 2 + cosf(a) * r * 1.6f), (int)(H / 2 + sinf(a) * r * 0.5f), g_rgb(40, 40, 40));
        }
    }
}

static void arrow_to(const ring_t *r)
{
    float sx, sy, z;
    bool vis = vox_project(&S, &cam, FY, r->x, r->y, r->h, &sx, &sy, &z);
    if (vis && sx > 10 && sx < W - 10 && sy > 5 && sy < H - 5) return;
    /* direction de l'anneau dans le repère de la caméra */
    float dx = r->x - cam.x, dy = r->y - cam.y;
    dx -= VOX_N * roundf(dx / VOX_N); dy -= VOX_N * roundf(dy / VOX_N);
    float a = atan2f(dy, dx) - cam.a;
    a = remainderf(a, 2 * PI);
    float ex = sinf(a), ey = -cosf(a) + (r->h - cam.h) * -0.01f;
    float l = sqrtf(ex * ex + ey * ey) + 1e-6f;
    ex /= l; ey /= l;
    int cx = (int)(120 + ex * 96), cy = (int)(130 + ey * 88);
    uint16_t c = g_rgb(255, 200, 50);
    for (int i = 0; i < 14; i++) {
        float w = (14 - i) * 0.45f;
        for (int j = (int)-w; j <= (int)w; j++) {
            int X = (int)(cx + ex * (i - 7) + -ey * j), Y = (int)(cy + ey * (i - 7) + ex * j);
            if ((unsigned)X < 240 && (unsigned)Y < 240) C.px[Y * 240 + X] = c;
        }
    }
}

static void draw_world(float t, bool rings)
{
    float sh = shake > 0 ? shake : 0;
    vox_cam_t keep = cam;
    if (sh > 0) { cam.pitch += (g_frnd() - 0.5f) * 10 * sh; cam.roll += (g_frnd() - 0.5f) * 0.1f * sh; }
    vox_render(&S, occ, &cam, FY, t);
    if (rings) for (int i = NRING - 1; i >= 0; i--) ring_draw(&ring[i], i, t);
    cam = keep;
    flare();
}

/* 160×120 → 240×240 : colonnes 1, 2, 1, 2… (×1,5), lignes doublées */
static void upscale(void)
{
    for (int y = 0; y < H; y++) {
        const uint16_t *src = S.px + y * W;
        uint16_t *d = C.px + (2 * y) * 240;
        for (int x = 0; x < W; x += 2) {
            uint16_t a = src[x], b = src[x + 1];
            d[0] = a; d[1] = a; d[2] = b;
            d += 3;
        }
        memcpy(C.px + (2 * y + 1) * 240, C.px + (2 * y) * 240, 480);
    }
}

static void pause_draw(void)
{
    for (int i = 0; i < 240 * 240; i++) C.px[i] = g_blend(C.px[i], 0, 150);
    char b[32];
    hud_c(_("PAUSE"), 60, 4, 0xFFFF);
    hud_c(_("TOUCHE POUR REPRENDRE"), 110, 1, g_rgb(230, 230, 240));
    for (int y = 146; y < 182; y++)
        for (int x = 60; x < 180; x++)
            C.px[y * 240 + x] = (y == 146 || y == 181 || x == 60 || x == 179) ? g_rgb(255, 120, 90) : g_rgb(70, 20, 20);
    hud_c(_("QUITTER"), 158, 2, g_rgb(255, 220, 210));
    snprintf(b, sizeof b, _("%d IMAGES/S"), s_fps);
    hud_c(b, 196, 1, g_rgb(150, 150, 160));
    lv_obj_invalidate(v_cv);
}

static void frame(void)
{
    if (!S.px || !C.px) return;
    if (st == ST_PAUSE) return;                      /* image figée, rien à calculer */
    uint32_t ms = demo_synth_ms();
    s_fps_n++;
    if (ms - s_fps_t0 >= 1000) { s_fps = s_fps_n * 1000 / (int)(ms - s_fps_t0 ? ms - s_fps_t0 : 1); s_fps_n = 0; s_fps_t0 = ms; }
    float dt = (ms - s_last_ms) / 1000.0f;
    if (dt < 0 || dt > 0.1f) dt = 0.033f;
    s_last_ms = ms;
    float t = ms / 1000.0f, ts = (ms - st_ms) / 1000.0f;
    char b[48];

    if (st == ST_GEN) {
        int p = vox_progress();
        g_fill(&C, g_rgb(8, 10, 24));
        hud_c(_("CRÉATION DU MONDE"), 96, 2, g_rgb(200, 220, 255));
        for (int x = 40; x < 200; x++)
            for (int y = 126; y < 134; y++)
                C.px[y * 240 + x] = x - 40 < p * 160 / 100 ? g_rgb(255, 200, 60) : g_rgb(40, 40, 60);
        snprintf(b, sizeof b, "%d %%", p);
        hud_c(b, 144, 1, g_rgb(160, 170, 200));
        if (p >= 100) {
            spawn();
            set_state(ST_TITLE);
        }
        lv_obj_invalidate(v_cv);
        return;
    }

    if (st == ST_TITLE) autopilot(t, dt);
    else if (st == ST_COUNT) {                     /* vol stationnaire, au-dessus du relief */
        float tx, ty;
        read_tilt(&tx, &ty);
        cam.x += cosf(cam.a) * 10 * dt; cam.y += sinf(cam.a) * 10 * dt;
        float g = 0;
        for (int k = 0; k < 10; k++) { float v = vox_ground(cam.x + cosf(cam.a) * k * 12, cam.y + sinf(cam.a) * k * 12); if (v > g) g = v; }
        if (cam.h < g + 25) cam.h += (g + 25 - cam.h) * 0.1f;
        cam.roll *= 0.9f;
    }
    else if (st == ST_PLAY) play_step(dt);
    else if (st == ST_CRASH) { shake = clampf(shake - dt * 1.2f, 0, 1); if (ts > 1.6f) set_state(ST_OVER); }
    else if (st == ST_OVER) { cam.a += 0.12f * dt; cam.h += (vox_ground(cam.x, cam.y) + 50 - cam.h) * 0.02f; cam.roll *= 0.95f; cam.pitch *= 0.95f; }

    bool rings = st == ST_COUNT || st == ST_PLAY;
    draw_world(t, rings);
    if (st == ST_PLAY && boost_t > 0) speed_lines(t);
    sparks_draw(dt);
    if (st == ST_CRASH) {
        g_flash(&S, (int)(clampf(1 - ts * 2, 0, 1) * 256));
        for (int i = 0; i < W * H; i++) S.px[i] = g_blend(S.px[i], g_rgb(200, 30, 10), (int)(clampf(0.5f - ts * 0.25f, 0, 0.5f) * 256));
    }
    if (st == ST_OVER) g_fade(&S, 120);
    upscale();

    /* interface à pleine résolution */
    switch (st) {
    case ST_TITLE: {
        int y = (int)(40 + sinf(t * 1.5f) * 4);
        g_text_c(&C, _("ENVOL"), y + 2, 6, g_rgb(60, 30, 10), false);
        g_text_c(&C, _("ENVOL"), y, 6, g_rgb(255, 220, 120), false);
        snprintf(b, sizeof b, _("RECORD %u"), (unsigned)best);
        hud_c(b, 98, 2, 0xFFFF);
        if ((int)(t * 1.6f) & 1) hud_c(_("TOUCHE POUR DÉCOLLER"), 176, 2, g_rgb(255, 240, 200));
        hud_c(_("PENCHE LE POIGNET POUR PILOTER"), 204, 1, g_rgb(230, 230, 240));
        hud_text(s_sound ? _("SON") : _("MUET"), 196, 8, 1, g_rgb(220, 220, 220));
        break;
    }
    case ST_COUNT: {
        int n = 3 - (int)ts;
        if (n > 0) snprintf(b, sizeof b, "%d", n); else snprintf(b, sizeof b, "%s", _("GO !"));
        float k = fmodf(ts, 1.0f);
        int sc = 9 - (int)(k * 4);
        hud_c(b, 90 - sc * 3, sc, g_rgb(255, 230, 120));
        hud_c(_("TIENS LA MONTRE À PLAT"), 200, 1, 0xFFFF);
        if (ts >= 3.6f) { play_t = 0; last_ring_t = -10; set_state(ST_PLAY); }
        if (ts < 0.05f) rings_init();                    /* anneaux placés depuis la position du départ */
        static int last_n;
        if (ts < 0.05f) last_n = 4;
        if (n != last_n && n >= 0) { demo_synth_sfx(n > 0 ? SFX_TICK : SFX_GO); last_n = n; }
        break;
    }
    case ST_PLAY: case ST_CRASH: {
        snprintf(b, sizeof b, "%d", (int)ceilf(time_left));
        uint16_t tc = time_left < 5 && ((int)(t * 4) & 1) ? g_rgb(255, 60, 40) : 0xFFFF;
        g_text_c(&C, b, 10, 3, tc, true);
        snprintf(b, sizeof b, "%d", score);
        hud_text(b, 10, 12, 2, g_rgb(255, 210, 80));
        hud_text(_("PTS"), 10, 30, 1, g_rgb(220, 200, 150));
        if (st == ST_PLAY)                                /* pause : toucher ici */
            for (int y = 44; y < 56; y++) { for (int x = 12; x < 16; x++) C.px[y * 240 + x] = 0xFFFF; for (int x = 19; x < 23; x++) C.px[y * 240 + x] = 0xFFFF; }
        /* jauge d'accélérateur */
        for (int x = 0; x < 50; x++)
            for (int y = 0; y < 5; y++)
                C.px[(16 + y) * 240 + 180 + x] = x < boost_charge * 50 ? (boost_charge >= 1 ? g_rgb(80, 220, 255) : g_rgb(60, 130, 170)) : g_rgb(30, 30, 40);
        hud_text(_("TURBO"), 186, 24, 1, g_rgb(180, 220, 240));
        if (st == ST_PLAY) {
            arrow_to(&ring[0]);
            float alt = cam.h - vox_ground(cam.x, cam.y);
            if (alt < 10 && ((int)(t * 6) & 1)) hud_c(_("ALTITUDE !"), 210, 2, g_rgb(255, 70, 50));
            if (s_msg_t > 0) {
                s_msg_t -= dt;
                hud_c(s_msg, 64 - (int)((1.2f - s_msg_t) * 20), 2, g_rgb(255, 240, 160));
            }
            if (play_t < 3) hud_c(_("TRAVERSE LES ANNEAUX D'OR"), 216, 1, 0xFFFF);
        } else hud_c(_("CRASH !"), 100, 4, g_rgb(255, 230, 200));
        break;
    }
    case ST_OVER:
        hud_c(s_timeout ? _("TEMPS ÉCOULÉ") : _("CRASH !"), 44, 2, g_rgb(255, 200, 120));
        snprintf(b, sizeof b, "%d", score);
        hud_c(b, 72, 6, 0xFFFF);
        if (rings_n == 1) snprintf(b, sizeof b, "%s", _("1 ANNEAU"));
        else snprintf(b, sizeof b, _("%d ANNEAUX"), rings_n);
        hud_c(b, 124, 1, g_rgb(220, 220, 230));
        if (s_new_best) { if ((int)(t * 3) & 1) hud_c(_("NOUVEAU RECORD !"), 144, 2, g_rgb(255, 210, 60)); }
        else { snprintf(b, sizeof b, _("RECORD %u"), (unsigned)best); hud_c(b, 144, 2, g_rgb(200, 200, 210)); }
        if (ts > 1.5f && ((int)(t * 1.6f) & 1)) hud_c(_("TOUCHE POUR REJOUER"), 190, 2, g_rgb(255, 240, 200));
        break;
    default: break;
    }
    lv_obj_invalidate(v_cv);
}

static void tick_cb(lv_timer_t *t) { frame(); }

static void start_game(void)
{
    memset(spark, 0, sizeof spark);
    float g = 0;                                       /* au-dessus de tout ce qui est devant */
    for (int k = 0; k < 10; k++) { float v = vox_ground(cam.x + cosf(cam.a) * k * 12, cam.y + sinf(cam.a) * k * 12); if (v > g) g = v; }
    if (cam.h < g + 25) cam.h = g + 25;
    cam.roll = cam.pitch = 0;
    s_turn = s_climb = 0;
    speed = 42;
    boost_t = 0;
    boost_charge = 1;
    time_left = 25;
    score = combo = rings_n = 0;
    s_msg_t = 0;
    shake = 0;
    calibrate();
    set_state(ST_COUNT);
}

static void tap_cb(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    switch (st) {
    case ST_TITLE:
        if (p.x > 180 && p.y < 40) { s_sound = !s_sound; demo_synth_sound(s_sound); break; }
        if (vox_progress() < 100) break;
        start_game();
        break;
    case ST_PLAY:
        if (p.x < 80 && p.y < 60) { st = ST_PAUSE; pause_draw(); break; }   /* coin du score : pause */
        if (boost_charge >= 1) { boost_t = 1.2f; boost_charge = 0; demo_synth_sfx(SFX_BOOST); }
        break;
    case ST_PAUSE:
        if (p.y > 140 && p.y < 190 && p.x > 50 && p.x < 190) { app_close(); break; }
        st = ST_PLAY;
        s_last_ms = demo_synth_ms();
        break;
    case ST_OVER:
        if ((demo_synth_ms() - st_ms) > 1500) {
            spawn();
            set_state(ST_TITLE);
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ cycle de vie */
static void free_all(void)
{
    if (s_vox) { vox_free(); s_vox = false; }
    heap_caps_free(S.px); heap_caps_free(C.px); heap_caps_free(occ);
    S.px = C.px = NULL;
    occ = NULL;
}

static void create(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    S = (gsurf_t){ heap_caps_malloc(W * H * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), W, H };
    if (!S.px) S.px = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    C = (gsurf_t){ heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), 240, 240 };
    occ = heap_caps_malloc(W * VOX_OCC, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!occ) occ = heap_caps_malloc(W * VOX_OCC, MALLOC_CAP_8BIT);
    v_cv = lv_canvas_create(scr);
    if (!S.px || !C.px || !occ) {
        free_all();
        lv_obj_t *l = ui_label(scr, &font_text_16, C_FG, _("Mémoire insuffisante"));
        lv_obj_center(l);
        return;
    }
    memset(C.px, 0, 240 * 240 * 2);
    lv_canvas_set_buffer(v_cv, C.px, 240, 240, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(v_cv, 0, 0);
    lv_obj_add_flag(v_cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_cv, tap_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(v_cv);
    best_load();
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
    power_hold_active(true);
    s_tilt = motion_accel_begin();
    g_seed(esp_random() | 1);
    if (!s_vox) s_vox = vox_gen_start(esp_random());
    set_state(vox_progress() >= 100 ? ST_TITLE : ST_GEN);
    if (st == ST_TITLE && cam.h == 0) spawn();
    s_last_ms = 0;
    demo_synth_start(SONG_GAME, s_sound);
    st_ms = 0;
    if (!v_tmr) v_tmr = lv_timer_create(tick_cb, 20, NULL);
    else lv_timer_resume(v_tmr);
}

static void on_hide(void)
{
    if (v_tmr) lv_timer_pause(v_tmr);
    demo_synth_stop();
    if (S.px) power_hold_active(false);
    if (s_tilt) { motion_accel_end(); s_tilt = false; }
    if (st == ST_PLAY || st == ST_COUNT || st == ST_PAUSE) st = ST_TITLE;   /* partie abandonnée */
}

static bool keep_on(void) { return true; }

const app_t app_flight = {
    .name = N_("Envol"),
    .icon = SYM_PLANE,
    .color = 0x0EA5E9,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .keep_screen_on = keep_on,
};

#ifndef ESP_PLATFORM
int flight_sim_state(void) { return (int)st; }   /* simulateur */
#endif
