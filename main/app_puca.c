/*
 * app_puca.c — « Púca » : animal virtuel du folklore irlandais.
 *
 *   ‹ Púca  Bébé · 5 h                 ♪
 *   ┌──────────────────────────────────┐
 *   │  ciel (heure du jour), collines  │   scène 80×50 pixels logiques,
 *   │  menhir, champignons, crottes    │   agrandie ×3 (240×150)
 *   │        le púca qui se promène    │
 *   └──────────────────────────────────┘
 *    🥔 ▬▬▬▬   ♥ ▬▬▬▬   ✦ ▬▬▬▬            satiété, humeur, énergie
 *   [nourrir][jouer][laver][soigner][nuit]
 *
 * Le modèle (puca_model.c) vit sans tâche de fond : il est avancé à chaque
 * affichage. La vue n'est qu'un petit moteur de sprites : un tampon de
 * 80×50 indices de palette redessiné à ~12 images/s tant que l'écran est
 * allumé, puis agrandi dans un canevas RGB565 en PSRAM.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "settings.h"
#include "wclock.h"
#include "motion.h"
#include "power.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "puca_model.h"
#include "puca_gfx.h"
#include "puca_sfx.h"

#define LW      80
#define LH      50
#define SC      3
#define CW      (LW * SC)
#define CH      (LH * SC)
#define TOP     26                       /* hauteur de la barre du haut */
#define FEET    47                       /* ligne sous les pattes */
#define TICK_MS 80
#define NPART   28

/* ------------------------------------------------------------------ modèle */
static puca_t m;
static bool m_loaded;

static void load(void)
{
    if (m_loaded) return;
    m_loaded = true;
    nvs_handle_t h;
    size_t n = sizeof m;
    bool ok = false;
    if (nvs_open("puca", NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "st", &m, &n) == ESP_OK && n == sizeof m && m.ver == 1;
        nvs_close(h);
    }
    if (!ok) {
        memset(&m, 0, sizeof m);
        m.rng = esp_random() | 1;
        puca_new(&m);
    }
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("puca", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "st", &m, sizeof m);
    nvs_commit(h);
    nvs_close(h);
}

static int32_t tz(void) { return wclock_tz_offset_min(); }

static void model_update(void)
{
    if (!wclock_time_valid()) return;
    time_t now = time(NULL);
    uint32_t day = (uint32_t)(((int64_t)now + tz() * 60) / 86400);
    puca_update(&m, now, tz(), motion_steps_on() ? motion_steps_today() : 0, day);
}

static bool asleep(void) { return wclock_time_valid() && puca_asleep(&m, time(NULL), tz()); }

/* ------------------------------------------------------------------ vue : état */
static lv_obj_t *v_canvas, *v_msg, *v_hint, *v_stage, *v_spk, *v_clov_lbl, *v_clov_img;
static lv_obj_t *v_bar[3], *v_btn[5], *v_light_img, *v_btnrow, *v_gamerow, *v_feed, *v_gone, *v_noclock;
static lv_timer_t *v_tmr;
static uint16_t *v_buf;                  /* 240×150 RGB565, PSRAM */
static uint32_t v_ms;                    /* horloge de l'animation */
static uint32_t v_msg_until;

EXT_RAM_BSS_ATTR static uint8_t s_bg[LH][LW];   /* décor fixe (0 = ciel) */
EXT_RAM_BSS_ATTR static uint8_t s_fb[LH][LW];
static bool s_bg_ok;

typedef struct { const puca_spr_t *s; float x, y, vx, vy, g; int16_t life; } part_t;
static part_t s_part[NPART];

/* le púca à l'écran */
static float p_x = 30, p_tx = 30, p_jump;
static int p_dir = 1;
static uint32_t p_pause, p_blink, p_next_blink;

/* animations */
enum { A_IDLE, A_HATCH, A_EGGTAP, A_EVOLVE, A_EAT, A_TREAT, A_HAPPY, A_REFUSE, A_CLEAN, A_HEAL, A_PET, A_GAME };
static int a_kind;
static uint32_t a_t0;
static int a_from_stage, a_from_form;        /* évolution : ancienne forme */
static float a_item_x, a_item_y;

/* jeu « gauche ou droite » */
#define G_ROUNDS 5
static int g_round, g_wins, g_phase, g_guess, g_look;
static uint8_t g_bits;                    /* manche i gagnée : bit i */
static uint32_t g_t;

/* ------------------------------------------------------------------ petits outils */
static uint32_t rnd(void) { return puca_rand(&m); }
static void sfx(puca_sfx_t s) { if (m.sound) puca_sfx(s); }

static void say(const char *txt, uint32_t ms)
{
    if (!v_msg) return;
    lv_label_set_text(v_msg, txt);
    lv_obj_remove_flag(v_msg, LV_OBJ_FLAG_HIDDEN);
    v_msg_until = v_ms + ms;
}

static void anim(int kind) { a_kind = kind; a_t0 = v_ms; }

static void part_add(const puca_spr_t *s, float x, float y, float vx, float vy, float g, int life)
{
    for (int i = 0; i < NPART; i++) {
        if (s_part[i].life <= 0) {
            s_part[i] = (part_t){ s, x, y, vx, vy, g, (int16_t)life };
            return;
        }
    }
}

static float frand(float a, float b) { return a + (b - a) * (float)(rnd() % 1000) / 1000.0f; }

/* ------------------------------------------------------------------ dessin */
static void blit(const puca_spr_t *s, int x, int y, bool flip, int mode)
{
    /* mode 0 : normal ; 1 : silhouette blanche ; 2 : seulement sur le ciel */
    for (int j = 0; j < s->h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= LH) continue;
        const uint8_t *row = s->px + j * s->w;
        for (int i = 0; i < s->w; i++) {
            uint8_t c = row[flip ? s->w - 1 - i : i];
            if (!c) continue;
            int xx = x + i;
            if (xx < 0 || xx >= LW) continue;
            if (mode == 2 && s_fb[yy][xx]) continue;
            s_fb[yy][xx] = mode == 1 ? PC_WHITE : c;
        }
    }
}

static const puca_body_t *body_of(int stage, int form)
{
    if (stage == PS_BABY) return &puca_body_baby;
    if (stage == PS_TEEN) return &puca_body_teen;
    if (form == PF_HARE) return &puca_body_hare;
    if (form == PF_GOAT) return &puca_body_goat;
    return &puca_body_horse;
}

static int body_w(void) { return body_of(m.stage, m.form)->frames[0]->w; }

static void draw_puca(const puca_body_t *b, int frame, const puca_spr_t *eye, const puca_spr_t *mouth,
                      int x, int y, bool flip, int mode)
{
    const puca_spr_t *f = b->frames[frame % b->nframes];
    blit(f, x, y, flip, mode);
    if (mode == 1) return;
    int w = f->w;
#define FX(px, gw) (flip ? w - (px) - (gw) : (px))
    if (eye) {
        blit(eye, x + FX(b->ex1, 3), y + b->ey1, flip, 0);
        if (b->ex2 >= 0) blit(eye, x + FX(b->ex2, 3), y + b->ey2, flip, 0);
    }
    if (mouth) blit(mouth, x + FX(b->mx, 3), y + b->my, flip, 0);
#undef FX
}

/* ciel : couleurs (haut, bas) et luminosité de la scène selon l'heure */
typedef struct { float h; uint32_t top, bot; float lum; } skykey_t;
static const skykey_t SKY[] = {
    { 0.0f,  0x0b1030, 0x232a5c, 0.50f },
    { 5.5f,  0x0b1030, 0x232a5c, 0.50f },
    { 7.0f,  0x5a4fb0, 0xffb38a, 0.80f },
    { 9.0f,  0x4aa8ec, 0xc8ecfa, 1.00f },
    { 17.5f, 0x4aa8ec, 0xc8ecfa, 1.00f },
    { 19.5f, 0x41307a, 0xff8f5e, 0.80f },
    { 21.0f, 0x0b1030, 0x232a5c, 0.50f },
    { 24.0f, 0x0b1030, 0x232a5c, 0.50f },
};

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    int g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    int bl = (int)((a & 255) * (1 - t) + (b & 255) * t);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static uint16_t to565(uint32_t c) { return lv_color_to_u16(lv_color_hex(c)); }

static float day_hour(void)
{
    if (!wclock_time_valid()) return 12;
    struct tm t;
    wclock_local_tm(&t);
    return (float)t.tm_hour + t.tm_min / 60.0f;
}

static void make_bg(void)
{
    for (int x = 0; x < LW; x++) {
        int far = 24 + (int)(3 * sinf(x * 0.09f + 1.0f) + 2 * sinf(x * 0.21f + 0.4f));
        int near = 33 + (int)(2.5f * sinf(x * 0.065f + 3.0f) + 1.2f * sinf(x * 0.19f));
        for (int y = 0; y < LH; y++) {
            uint8_t c = 0;
            if (y >= far) c = ((x + y) & 3) == 0 && y > far + 1 ? PC_HILL_FAR2 : PC_HILL_FAR;
            if (y >= near) c = y == near ? PC_HILL_HI : PC_HILL;
            if (y >= 41) c = PC_GRASS;
            if (y == 41 && (x * 7 % 5) == 0) c = PC_HILL_HI;
            if (y >= 48) c = ((x * 3 + y) % 5) == 0 ? PC_GRASS : PC_DIRT;
            s_bg[y][x] = c;
        }
    }
    /* menhir et cercle de champignons (le tertre des fées) */
    memcpy(s_fb, s_bg, sizeof s_bg);
    blit(&puca_it_stone, 2, 35, false, 0);
    blit(&puca_it_shroom, 66, 39, false, 0);
    blit(&puca_it_shroom, 72, 41, false, 0);
    blit(&puca_it_shroom, 61, 42, false, 0);
    memcpy(s_bg, s_fb, sizeof s_bg);
    s_bg_ok = true;
}

static const struct { uint8_t x, y; } STARS[] = {
    { 5, 3 }, { 13, 9 }, { 21, 2 }, { 29, 12 }, { 36, 5 }, { 44, 1 }, { 50, 10 }, { 57, 4 },
    { 70, 13 }, { 76, 6 }, { 9, 16 }, { 40, 17 }, { 25, 7 }, { 66, 18 }, { 74, 1 },
};

static const int POOP_X[PUCA_POOPS] = { 56, 47, 64, 39 };

static void render(void)
{
    if (!v_buf) return;
    if (!s_bg_ok) make_bg();
    memcpy(s_fb, s_bg, sizeof s_bg);

    /* lumière de la scène */
    float hr = day_hour();
    int k = 0;
    while (k < (int)(sizeof SKY / sizeof SKY[0]) - 2 && hr >= SKY[k + 1].h) k++;
    float t = (hr - SKY[k].h) / (SKY[k + 1].h - SKY[k].h);
    uint32_t sky_top = mix(SKY[k].top, SKY[k + 1].top, t), sky_bot = mix(SKY[k].bot, SKY[k + 1].bot, t);
    float lum = SKY[k].lum + (SKY[k + 1].lum - SKY[k].lum) * t;
    bool night = lum < 0.6f;
    bool sleeping = m.stage >= PS_BABY && m.stage <= PS_ADULT && asleep();
    uint32_t tint = 0x0a0c22;                 /* teinte vers laquelle on assombrit */
    if (sleeping && m.light_off) {             /* nuit noire : seuls brillent les yeux, la lune, les étoiles */
        lum = 0.30f; sky_top = mix(sky_top, 0x02030a, 0.6f); sky_bot = mix(sky_bot, 0x05060f, 0.7f);
    } else if (sleeping) {                     /* lampe allumée : lumière chaude */
        lum = 0.80f; tint = 0x3a2208;
    }

    /* ciel : étoiles, lune ou soleil, nuages */
    if (night) {
        for (size_t i = 0; i < sizeof STARS / sizeof STARS[0]; i++)
            if (!s_fb[STARS[i].y][STARS[i].x] && ((v_ms / 400 + i * 7) % 11))
                s_fb[STARS[i].y][STARS[i].x] = PC_STAR;
        blit(&puca_it_moon, 62, 4, false, 2);
    } else {
        for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++)
            if (i * i + j * j <= 5 && !s_fb[6 + j][66 + i]) s_fb[6 + j][66 + i] = PC_SPARK;
        float cx = fmodf(v_ms / 900.0f, LW + 30) - 15;
        blit(&puca_it_cloud, (int)cx, 4, false, 2);
        blit(&puca_it_cloud, (int)fmodf(cx + 47, LW + 30) - 15, 11, false, 2);
    }

    /* crottes, avec mouches d'odeur */
    for (int i = 0; i < m.poops && i < PUCA_POOPS; i++) {
        if (a_kind == A_CLEAN && a_item_x + 10 > POOP_X[i]) continue;
        blit(&puca_it_poop, POOP_X[i], FEET - 7, false, 0);
        if ((v_ms / 300 + i) % 2) blit(&puca_it_stink, POOP_X[i] + 3, FEET - 12 - (int)((v_ms / 150 + i * 3) % 3), false, 0);
    }

    /* l'œuf ou le púca */
    uint32_t at = v_ms - a_t0;
    if (m.stage == PS_EGG || a_kind == A_HATCH) {
        bool hatched = a_kind == A_HATCH && at > 900;
        if (!hatched) {
            const puca_spr_t *e = m.egg_taps >= 4 ? &puca_egg_2 : m.egg_taps >= 2 ? &puca_egg_1 : &puca_egg_0;
            int dx = 0;
            if (a_kind == A_EGGTAP && at < 300) dx = (at / 60) % 2 ? 1 : -1;
            else if (a_kind == A_HATCH) dx = (at / 40) % 2 ? 1 : -1;
            else if ((v_ms / 1800) % 3 == 0 && (v_ms / 120) % 4 < 2) dx = (v_ms / 120) % 2 ? 1 : -1;
            blit(e, 33 + dx, FEET - e->h, false, a_kind == A_HATCH && at > 800 ? 1 : 0);
        }
    }
    if (m.stage >= PS_BABY && m.stage <= PS_ADULT) {
        const puca_body_t *b = body_of(m.stage, m.form);
        int frame = sleeping ? (v_ms / 1100) % 2 : (v_ms / 450) % 2;
        const puca_spr_t *eye = &puca_eye_open, *mouth = &puca_mouth_smile;
        if (m.stage == PS_ADULT && m.form != PF_GOAT) mouth = &puca_mouth_none;
        if (sleeping) { eye = &puca_eye_closed; mouth = &puca_mouth_none; }
        else if (m.sick) { eye = &puca_eye_sick; mouth = &puca_mouth_flat; }
        else if (m.mood < 250 || m.hunger < 150) { eye = &puca_eye_sad; mouth = &puca_mouth_frown; }
        else if (v_ms < p_blink) eye = &puca_eye_blink;

        switch (a_kind) {
        case A_EAT: case A_TREAT:
            if (at > 500 && at < 1700) { eye = &puca_eye_happy; mouth = (at / 180) % 2 ? &puca_mouth_open : &puca_mouth_chew; }
            else if (at >= 1700) { eye = &puca_eye_happy; mouth = &puca_mouth_smile; }
            break;
        case A_HAPPY: case A_PET: eye = &puca_eye_happy; mouth = &puca_mouth_open; break;
        case A_REFUSE: eye = &puca_eye_blink; mouth = &puca_mouth_flat; break;
        case A_HEAL: eye = at < 900 ? &puca_eye_wide : &puca_eye_happy; mouth = &puca_mouth_smile; break;
        case A_HATCH: eye = &puca_eye_happy; mouth = &puca_mouth_open; break;
        case A_GAME:
            if (g_phase == 1) { eye = &puca_eye_wide; mouth = &puca_mouth_open; }
            else if (g_phase == 2) { eye = g_wins * 2 > G_ROUNDS ? &puca_eye_happy : &puca_eye_sad;
                                     mouth = g_wins * 2 > G_ROUNDS ? &puca_mouth_open : &puca_mouth_frown; }
            break;
        default: break;
        }
        int bw = b->frames[0]->w, bh = b->frames[0]->h;
        int x = (int)p_x, y = FEET - bh + (int)p_jump;
        bool flip = p_dir < 0;
        if (a_kind == A_EVOLVE && at < 2600) {
            /* évolution : l'ancienne et la nouvelle silhouette s'échangent de plus en plus vite */
            const puca_body_t *ob = body_of(a_from_stage, a_from_form);
            uint32_t per = at < 1200 ? 260 : at < 2000 ? 130 : 60;
            bool newer = (at / per) % 2;
            const puca_body_t *sb = newer ? b : ob;
            int sx = (int)p_x + (bw - sb->frames[0]->w) / 2;
            draw_puca(sb, 0, NULL, NULL, sx, FEET - sb->frames[0]->h, flip, 1);
        } else if (a_kind == A_HATCH && at <= 900) {
            /* encore dans l'œuf */
        } else {
            draw_puca(b, frame, eye, mouth, x, y, flip, 0);
        }
        (void)bw;
        if (!sleeping && m.sick && a_kind != A_HEAL)
            blit(&puca_it_dizzy, x + bw / 2 - 2, y - 6 - (int)((v_ms / 250) % 2), false, 0);
        if (!sleeping && !m.sick && m.mood < 250 && a_kind == A_IDLE) {
            blit(&puca_it_raincloud, x + bw / 2 - 6, y - 9, false, 0);
            if ((v_ms / 200) % 2) blit(&puca_it_rain, x + bw / 2 - 3 + (int)(v_ms / 200 % 4) * 2, y - 3, false, 0);
        }
    }

    /* objets en mouvement */
    if (a_kind == A_EAT || a_kind == A_TREAT) {
        bool cake = a_kind == A_TREAT;
        const puca_spr_t *s = cake ? (at < 900 ? &puca_it_cake : at < 1300 ? &puca_it_cake2 : &puca_it_cake3)
                                   : (at < 900 ? &puca_it_potato : at < 1300 ? &puca_it_potato2 : &puca_it_potato3);
        if (at < 1700) blit(s, (int)a_item_x, (int)a_item_y, false, 0);
    } else if (a_kind == A_CLEAN) {
        blit(&puca_it_wave, (int)a_item_x, FEET - 6, false, 0);
    } else if (a_kind == A_HEAL && at < 900) {
        blit(&puca_it_potion, (int)a_item_x, (int)a_item_y, false, 0);
    } else if (a_kind == A_GAME && g_phase < 2) {
        /* manches : cœurs gagnés, points restants */
        for (int i = 0; i < G_ROUNDS; i++) {
            int xx = 26 + i * 6;
            if (i < g_round || (i == g_round && g_phase == 1))
                blit((g_bits >> i) & 1 ? &puca_it_heart_s : &puca_it_dot, xx, 2, false, 0);
            else blit(&puca_it_dot_off, xx, 2, false, 0);
        }
    }

    /* particules */
    for (int i = 0; i < NPART; i++) {
        part_t *p = &s_part[i];
        if (p->life <= 0) continue;
        if (p->life > 2 || (v_ms / TICK_MS) % 2) blit(p->s, (int)p->x, (int)p->y, false, 0);
    }

    /* palette du moment, puis agrandissement ×3 */
    uint16_t pal[PC_COUNT], sky[LH];
    for (int i = 1; i < PC_COUNT; i++) {
        uint32_t c = PUCA_PAL[i];
        if (!PUCA_GLOW[i] && lum < 0.99f) {
            c = mix(c, tint, (1.0f - lum) * 0.9f);
        }
        pal[i] = to565(c);
    }
    for (int y = 0; y < LH; y++) sky[y] = to565(mix(sky_top, sky_bot, y >= 30 ? 1.0f : y / 30.0f));
    bool flash = (a_kind == A_HATCH && at > 800 && at < 960) || (a_kind == A_EVOLVE && at > 2550 && at < 2750);
    if (flash) { for (int i = 1; i < PC_COUNT; i++) pal[i] = 0xFFFF; for (int y = 0; y < LH; y++) sky[y] = 0xFFFF; }

    uint16_t line[CW];
    for (int y = 0; y < LH; y++) {
        for (int x = 0; x < LW; x++) {
            uint8_t c = s_fb[y][x];
            uint16_t v = c ? pal[c] : sky[y];
            line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = v;
        }
        uint16_t *dst = v_buf + y * SC * CW;
        memcpy(dst, line, sizeof line);
        memcpy(dst + CW, line, sizeof line);
        memcpy(dst + 2 * CW, line, sizeof line);
    }
    lv_obj_invalidate(v_canvas);
}

/* ------------------------------------------------------------------ comportement */
static void refresh_ui(void);

static void finish_anim(void)
{
    a_kind = A_IDLE;
    p_jump = 0;
    refresh_ui();
}

static void step_puca(void)
{
    uint32_t at = v_ms - a_t0;
    bool sleeping = asleep();
    int bw = m.stage >= PS_BABY && m.stage <= PS_ADULT ? body_w() : 14;

    /* promenade */
    if (a_kind == A_IDLE && !sleeping && m.stage >= PS_BABY && m.stage <= PS_ADULT) {
        if (v_ms >= p_pause) {
            float speed = m.sick || m.energy < 200 ? 0.15f : 0.35f;
            if (fabsf(p_tx - p_x) < 0.5f) {
                p_tx = frand(4, (float)(LW - bw - 4));
                p_pause = v_ms + 800 + rnd() % 3500;
            } else {
                p_dir = p_tx > p_x ? 1 : -1;
                p_x += p_dir * speed;
            }
        }
        if (v_ms >= p_next_blink) { p_blink = v_ms + 140; p_next_blink = v_ms + 1800 + rnd() % 3500; }
    }
    if (sleeping && a_kind == A_IDLE && (v_ms / TICK_MS) % 16 == 0)
        part_add(&puca_it_z, p_x + bw * (p_dir > 0 ? 0.7f : 0.1f), FEET - 18, 0.12f * p_dir, -0.18f, 0, 30);

    switch (a_kind) {
    case A_EGGTAP:
        if (at > 320) finish_anim();
        break;
    case A_HATCH:
        if (at > 900 && at - TICK_MS <= 900) {
            for (int i = 0; i < 8; i++) part_add(&puca_it_shell_l, 38, FEET - 10, frand(-0.9f, 0.9f), frand(-1.4f, -0.6f), 0.08f, 25);
            for (int i = 0; i < 6; i++) part_add(&puca_it_spark, frand(25, 50), frand(20, 40), 0, -0.1f, 0, 14);
            p_x = 32; p_dir = 1;
        }
        if (at > 1800) finish_anim();
        break;
    case A_EVOLVE:
        if ((v_ms / TICK_MS) % 3 == 0 && at < 2600)
            part_add(&puca_it_spark_s, p_x + frand(-6, (float)bw + 4), frand(15, 45), 0, -0.2f, 0, 10);
        if (at > 2600 && at - TICK_MS <= 2600) {
            for (int i = 0; i < 10; i++) part_add(&puca_it_spark, p_x + bw / 2.0f, 30, frand(-1.2f, 1.2f), frand(-1.2f, 0.5f), 0.02f, 20);
            sfx(SFX_HAPPY);
        }
        if (at > 3600) { m.prev_stage = m.stage; save(); finish_anim(); }
        break;
    case A_EAT: case A_TREAT:
        if (at < 900) {
            float tx = p_x + (p_dir > 0 ? bw - 2 : -7);
            a_item_x = tx;
            a_item_y = -8 + (FEET - 8 + 8) * (at / 900.0f) * (at / 900.0f);
            if (a_item_y > FEET - 8) a_item_y = FEET - 8;
        }
        if (a_kind == A_TREAT && at > 1300 && at - TICK_MS <= 1300)
            for (int i = 0; i < 4; i++) part_add(&puca_it_heart, p_x + bw / 2.0f + frand(-6, 6), FEET - 25, frand(-0.3f, 0.3f), -0.35f, 0, 22);
        if (at > 2200) finish_anim();
        break;
    case A_HAPPY: case A_PET: {
        int hops = a_kind == A_PET ? 1 : 2;
        float per = 520.0f;
        if (at < per * hops) {
            float u = fmodf((float)at, per) / per;
            p_jump = -10.0f * 4 * u * (1 - u) * (a_kind == A_PET ? 0.5f : 1.0f);
        } else p_jump = 0;
        if (at > per * hops + 300) finish_anim();
        break;
    }
    case A_REFUSE:
        p_dir = (at / 160) % 2 ? -1 : 1;
        if (at > 900) finish_anim();
        break;
    case A_CLEAN:
        a_item_x = -14 + (LW + 16) * (at / 1400.0f);
        if (at > 1400) { for (int i = 0; i < 6; i++) part_add(&puca_it_spark_s, frand(36, 72), frand(36, 46), 0, -0.15f, 0, 12); finish_anim(); }
        break;
    case A_HEAL:
        if (at < 900) { a_item_x = p_x + bw / 2.0f - 3; a_item_y = -8 + (FEET - 30) * (at / 900.0f); }
        if (at > 900 && at - TICK_MS <= 900)
            for (int i = 0; i < 8; i++) part_add(&puca_it_spark_s, p_x + bw / 2.0f, FEET - 18, cosf(i * 0.785f) * 0.6f, sinf(i * 0.785f) * 0.6f, 0, 16);
        if (at > 1800) finish_anim();
        break;
    default: break;
    }

    if (m.stage == PS_GONE && (v_ms / TICK_MS) % 5 == 0)       /* le tertre des fées scintille */
        part_add((rnd() & 1) ? &puca_it_spark_s : &puca_it_spark, frand(58, 76), frand(32, 42), 0, -0.12f, 0, 16);

    /* particules */
    for (int i = 0; i < NPART; i++) {
        part_t *p = &s_part[i];
        if (p->life <= 0) continue;
        p->x += p->vx; p->y += p->vy; p->vy += p->g;
        p->life--;
    }
}

/* ------------------------------------------------------------------ jeu « gauche ou droite » */
static void game_end(void)
{
    puca_play_done(&m, g_wins, G_ROUNDS);
    save();
    bool won = g_wins * 2 > G_ROUNDS;
    if (won) {
        sfx(SFX_WIN);
        part_add(&puca_it_clover, p_x + 4, FEET - 30, 0, -0.25f, 0, 30);
        char b[48];
        snprintf(b, sizeof b, _("Gagné %d/%d ! +1 trèfle"), g_wins, G_ROUNDS);
        say(b, 2200);
    } else {
        sfx(SFX_LOSE);
        char b[48];
        snprintf(b, sizeof b, _("Perdu… %d/%d"), g_wins, G_ROUNDS);
        say(b, 2000);
    }
}

static void game_step(void)
{
    if (a_kind != A_GAME) return;
    uint32_t at = v_ms - g_t;
    if (g_phase == 0) {                      /* il hésite : tourne la tête */
        if ((v_ms / 260) % 2) p_dir = (v_ms / 520) % 2 ? 1 : -1;
    } else if (g_phase == 1) {               /* il a choisi */
        p_dir = g_look;
        if (at > 900) {
            g_round++;
            if (g_round >= G_ROUNDS) {
                g_phase = 2; g_t = v_ms;
                game_end();
                if (g_wins * 2 > G_ROUNDS) { anim(A_GAME); }
            } else {
                g_phase = 0; g_t = v_ms;
            }
        }
    } else if (g_phase == 2) {
        if (g_wins * 2 > G_ROUNDS && at < 1100) {
            float u = fmodf((float)at, 550) / 550.0f;
            p_jump = -9.0f * 4 * u * (1 - u);
        } else p_jump = 0;
        if (at > 1800) {
            lv_obj_add_flag(v_gamerow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(v_btnrow, LV_OBJ_FLAG_HIDDEN);
            finish_anim();
        }
    }
}

static void guess_cb(lv_event_t *e)
{
    if (a_kind != A_GAME || g_phase != 0) return;
    g_guess = (int)(intptr_t)lv_event_get_user_data(e);
    g_look = (rnd() & 1) ? 1 : -1;
    g_phase = 1;
    g_t = v_ms;
    if (g_look == g_guess) {
        g_wins++;
        g_bits |= (uint8_t)(1u << g_round);
        sfx(SFX_RIGHT);
        part_add(&puca_it_heart_s, p_x + body_w() / 2.0f, FEET - 28, 0, -0.3f, 0, 12);
    } else {
        sfx(SFX_WRONG);
    }
}

/* ------------------------------------------------------------------ actions */
static const char *res_text(puca_res_t r)
{
    switch (r) {
    case PR_ASLEEP:    return _("Chut, il dort…");
    case PR_FULL:      return _("Il n'a plus faim");
    case PR_NO_CLOVER: return _("Plus de trèfle : marche ou joue");
    case PR_TIRED:     return _("Trop fatigué pour jouer");
    case PR_SICK:      return _("Malade : soigne-le d'abord");
    default:           return "";
    }
}

static bool busy_anim(void) { return a_kind != A_IDLE; }

static void refuse(puca_res_t r)
{
    say(res_text(r), 1600);
    if (r != PR_ASLEEP) { anim(A_REFUSE); sfx(SFX_REFUSE); }
}

static void feed_choice_cb(lv_event_t *e)
{
    int treat = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_add_flag(v_feed, LV_OBJ_FLAG_HIDDEN);
    time_t now = time(NULL);
    puca_res_t r = treat ? puca_treat(&m, now, tz()) : puca_feed(&m, now, tz());
    if (r != PR_OK) { refuse(r); return; }
    save();
    anim(treat ? A_TREAT : A_EAT);
    sfx(treat ? SFX_TREAT : SFX_EAT);
    refresh_ui();
}

static void act_cb(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (busy_anim() || m.stage < PS_BABY || m.stage > PS_ADULT) return;
    time_t now = time(NULL);
    puca_res_t r;
    switch (a) {
    case 0:                                  /* nourrir : repas ou gâteau */
        if (asleep()) { refuse(PR_ASLEEP); return; }
        lv_obj_remove_flag(v_feed, LV_OBJ_FLAG_HIDDEN);
        refresh_ui();
        return;
    case 1:                                  /* jouer */
        r = puca_play_ok(&m, now, tz());
        if (r != PR_OK) { refuse(r); return; }
        g_round = g_wins = g_phase = 0;
        g_bits = 0;
        g_t = v_ms;
        anim(A_GAME);
        lv_obj_add_flag(v_btnrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v_gamerow, LV_OBJ_FLAG_HIDDEN);
        say(_("Gauche ou droite ?"), 1800);
        return;
    case 2:                                  /* laver */
        if (puca_clean(&m) != PR_OK) { say(_("Tout est propre"), 1400); return; }
        save(); anim(A_CLEAN); sfx(SFX_CLEAN);
        break;
    case 3:                                  /* soigner */
        if (puca_heal(&m) != PR_OK) { say(_("Il va très bien"), 1400); return; }
        save(); anim(A_HEAL); sfx(SFX_HEAL);
        break;
    case 4:                                  /* lumière */
        if (!asleep()) { say(_("Il fait jour, il est réveillé"), 1600); return; }
        puca_light(&m);
        save(); sfx(SFX_LIGHT);
        break;
    }
    refresh_ui();
}

static void canvas_cb(lv_event_t *e)
{
    if (m.stage == PS_EGG && a_kind != A_HATCH) {
        if (!wclock_time_valid()) return;
        if (puca_egg_tap(&m, time(NULL))) {
            save();
            anim(A_HATCH);
            sfx(SFX_HATCH);
            if (settings_get()->vibrate) power_vibrate(VIB_SHORT);
            lv_obj_add_flag(v_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            anim(A_EGGTAP);
            sfx(SFX_TAP);
        }
        refresh_ui();
        return;
    }
    if (m.stage < PS_BABY || m.stage > PS_ADULT || busy_anim() || asleep()) return;
    /* une caresse : petit bond et un cœur */
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    int lx = (pt.x - (int)lv_obj_get_x(v_canvas)) / SC;
    if (lx >= (int)p_x - 3 && lx <= (int)p_x + body_w() + 3) {
        anim(A_PET);
        part_add(&puca_it_heart_s, p_x + body_w() / 2.0f, FEET - 24, 0, -0.3f, 0, 14);
        sfx(SFX_TAP);
    }
}

static void new_egg_cb(lv_event_t *e)
{
    memset(s_part, 0, sizeof s_part);
    puca_new(&m);
    save();
    lv_obj_add_flag(v_gone, LV_OBJ_FLAG_HIDDEN);
    refresh_ui();
}

static void spk_cb(lv_event_t *e)
{
    m.sound = !m.sound;
    save();
    refresh_ui();
    if (m.sound) puca_sfx(SFX_TAP);
}

static void back_cb(lv_event_t *e) { app_close(); }

static void feed_close_cb(lv_event_t *e) { lv_obj_add_flag(v_feed, LV_OBJ_FLAG_HIDDEN); }

/* ------------------------------------------------------------------ icônes pixel */
typedef struct { lv_image_dsc_t dsc; uint32_t *px; const puca_spr_t *spr; int scale; } icon_t;
static icon_t s_icons[16];
static int s_nicons;

static const lv_image_dsc_t *icon(const puca_spr_t *s, int scale)
{
    for (int i = 0; i < s_nicons; i++)
        if (s_icons[i].spr == s && s_icons[i].scale == scale) return &s_icons[i].dsc;
    if (s_nicons >= (int)(sizeof s_icons / sizeof s_icons[0])) return NULL;
    icon_t *ic = &s_icons[s_nicons];
    int w = s->w * scale, h = s->h * scale;
    ic->px = heap_caps_malloc(w * h * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ic->px) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t c = s->px[(y / scale) * s->w + x / scale];
            ic->px[y * w + x] = c ? 0xFF000000u | PUCA_PAL[c] : 0;
        }
    memset(&ic->dsc, 0, sizeof ic->dsc);
    ic->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ic->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    ic->dsc.header.w = w;
    ic->dsc.header.h = h;
    ic->dsc.header.stride = w * 4;
    ic->dsc.data = (const uint8_t *)ic->px;
    ic->dsc.data_size = w * h * 4;
    ic->spr = s;
    ic->scale = scale;
    s_nicons++;
    return &ic->dsc;
}

/* ------------------------------------------------------------------ interface */
static const char *stage_name(void)
{
    switch (m.stage) {
    case PS_EGG:  return _("Œuf");
    case PS_BABY: return _("Bébé");
    case PS_TEEN: return _("Jeune");
    case PS_GONE: return "";
    default: return m.form == PF_HARE ? _("Lièvre") : m.form == PF_GOAT ? _("Bouc") : _("Cheval");
    }
}

static void refresh_ui(void)
{
    if (!v_canvas) return;
    char b[48];
    uint32_t h = wclock_time_valid() ? puca_age_h(&m, time(NULL)) : 0;
    if (m.stage >= PS_BABY && m.stage <= PS_ADULT) {
        if (h < 48) snprintf(b, sizeof b, "%s · %lu h", stage_name(), (unsigned long)h);
        else snprintf(b, sizeof b, _("%s · %lu j"), stage_name(), (unsigned long)(h / 24));
    } else snprintf(b, sizeof b, "%s", stage_name());
    lv_label_set_text(v_stage, b);
    lv_label_set_text(v_spk, m.sound ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_MUTE);

    bool alive = m.stage >= PS_BABY && m.stage <= PS_ADULT;
    snprintf(b, sizeof b, "%u", (unsigned)m.clovers);
    lv_label_set_text(v_clov_lbl, b);
    if (alive) { lv_obj_remove_flag(v_clov_lbl, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(v_clov_img, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(v_clov_lbl, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(v_clov_img, LV_OBJ_FLAG_HIDDEN); }

    const uint16_t vals[3] = { m.hunger, m.mood, m.energy };
    for (int i = 0; i < 3; i++) lv_bar_set_value(v_bar[i], alive ? vals[i] / 10 : 0, LV_ANIM_ON);

    bool sl = asleep();
    const bool en[5] = { alive && !sl, alive && !sl && !m.sick, alive && m.poops > 0, alive && m.sick, alive && sl };
    for (int i = 0; i < 5; i++) lv_obj_set_style_opa(v_btn[i], en[i] ? LV_OPA_COVER : LV_OPA_40, 0);
    /* le soin clignote quand il est malade */
    lv_obj_set_style_bg_color(v_btn[3], m.sick ? lv_color_hex(0x5b2a86) : C_CARD, 0);
    lv_obj_set_style_bg_color(v_btn[2], m.poops >= 2 ? lv_color_hex(0x5a3b1c) : C_CARD, 0);
    if (v_light_img) lv_image_set_src(v_light_img, icon(m.light_off ? &puca_it_b_sun : &puca_it_b_moon, 3));

    if (m.stage == PS_EGG && a_kind != A_HATCH) lv_obj_remove_flag(v_hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(v_hint, LV_OBJ_FLAG_HIDDEN);
    if (m.stage == PS_GONE) lv_obj_remove_flag(v_gone, LV_OBJ_FLAG_HIDDEN);
    if (!wclock_time_valid()) lv_obj_remove_flag(v_noclock, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(v_noclock, LV_OBJ_FLAG_HIDDEN);
}

static void handle_events(void)
{
    /* ce qui s'est passé pendant l'absence (ou depuis la dernière mise à jour) */
    if (m.events & PE_GONE) {
        memset(s_part, 0, sizeof s_part);
        lv_obj_remove_flag(v_gone, LV_OBJ_FLAG_HIDDEN);
    } else if ((m.events & PE_EVOLVED) || (m.stage != m.prev_stage && m.stage >= PS_TEEN && m.stage <= PS_ADULT)) {
        a_from_stage = m.prev_stage == PS_EGG ? PS_BABY : m.prev_stage;
        a_from_form = PF_NONE;
        p_x = (LW - body_w()) / 2.0f;
        anim(A_EVOLVE);
        sfx(SFX_EVOLVE);
        say(_("Il grandit !"), 3400);
    } else if (m.events & PE_SICK) {
        say(_("Il est tombé malade"), 2000);
    } else if (m.events & PE_CLOVER) {
        say(_("Des trèfles grâce à tes pas !"), 2000);
        part_add(&puca_it_clover, 8, 20, 0, -0.2f, 0, 20);
        sfx(SFX_CLOVER);
    }
    m.events = 0;
    save();
}

static void tick_cb(lv_timer_t *t)
{
    v_ms += TICK_MS;
    step_puca();
    game_step();
    render();
    if (v_msg_until && v_ms >= v_msg_until) { lv_obj_add_flag(v_msg, LV_OBJ_FLAG_HIDDEN); v_msg_until = 0; }
    if ((v_ms % 30000) < TICK_MS && a_kind == A_IDLE) {        /* toutes les 30 s */
        model_update();
        if (m.events & PE_POOP) sfx(SFX_TAP);
        if (m.events) handle_events();
        refresh_ui();
    }
}

static lv_obj_t *mk_btn(lv_obj_t *par, int x, int w, const puca_spr_t *s, int scale, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_pos(b, x, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2c3340), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    ui_bubble(b);
    const lv_image_dsc_t *d = icon(s, scale);
    if (d) {
        lv_obj_t *im = lv_image_create(b);
        lv_image_set_src(im, d);
        lv_obj_center(im);
    }
    return b;
}

static void create(lv_obj_t *scr)
{
    load();
    v_ms = 0;
    a_kind = A_IDLE;
    memset(s_part, 0, sizeof s_part);

    /* barre du haut */
    lv_obj_t *bk = ui_label(scr, &font_title_20, C_ACC, LV_SYMBOL_LEFT);
    lv_obj_set_pos(bk, 8, 2);
    lv_obj_add_flag(bk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(bk, 12);
    lv_obj_add_event_cb(bk, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = ui_label(scr, &font_title_20, C_FG, "Púca");
    lv_obj_set_pos(t, 30, 2);
    v_stage = ui_label(scr, &font_small_11, C_DIM, "");
    lv_obj_align_to(v_stage, t, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -3);
    v_spk = ui_label(scr, &font_text_16, C_DIM, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_pos(v_spk, 212, 5);
    lv_obj_add_flag(v_spk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(v_spk, 12);
    lv_obj_add_event_cb(v_spk, spk_cb, LV_EVENT_CLICKED, NULL);

    /* scène */
    v_buf = heap_caps_malloc(CW * CH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    v_canvas = lv_canvas_create(scr);
    if (v_buf) lv_canvas_set_buffer(v_canvas, v_buf, CW, CH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(v_canvas, 0, TOP);
    lv_obj_add_flag(v_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_canvas, canvas_cb, LV_EVENT_PRESSED, NULL);
    ui_bubble(v_canvas);

    v_clov_img = lv_image_create(v_canvas);
    lv_image_set_src(v_clov_img, icon(&puca_it_clover, 2));
    lv_obj_set_pos(v_clov_img, 4, 4);
    v_clov_lbl = ui_label(v_canvas, &font_text_16, C_FG, "0");
    lv_obj_set_style_text_color(v_clov_lbl, lv_color_white(), 0);
    lv_obj_set_pos(v_clov_lbl, 22, 2);

    v_msg = ui_label(v_canvas, &font_text_16, C_FG, "");
    lv_obj_set_style_bg_color(v_msg, lv_color_hex(0x0b0d14), 0);
    lv_obj_set_style_bg_opa(v_msg, LV_OPA_80, 0);
    lv_obj_set_style_radius(v_msg, 10, 0);
    lv_obj_set_style_pad_hor(v_msg, 8, 0);
    lv_obj_set_style_pad_ver(v_msg, 3, 0);
    lv_obj_align(v_msg, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_add_flag(v_msg, LV_OBJ_FLAG_HIDDEN);

    v_hint = ui_label(v_canvas, &font_small_11, C_FG, _("Touche l'œuf pour le réchauffer"));
    lv_obj_set_style_text_color(v_hint, lv_color_white(), 0);
    lv_obj_set_style_bg_color(v_hint, lv_color_hex(0x0b0d14), 0);
    lv_obj_set_style_bg_opa(v_hint, LV_OPA_60, 0);
    lv_obj_set_style_radius(v_hint, 8, 0);
    lv_obj_set_style_pad_hor(v_hint, 6, 0);
    lv_obj_set_style_pad_ver(v_hint, 2, 0);
    lv_obj_align(v_hint, LV_ALIGN_TOP_MID, 0, 6);

    /* jauges */
    static const puca_spr_t *const gi[3] = { &puca_it_potato, &puca_it_heart, &puca_it_bolt };
    static const uint32_t gc[3] = { 0xd99a5a, 0xff4d6d, 0xffd84a };
    for (int i = 0; i < 3; i++) {
        int x = 6 + i * 79;
        lv_obj_t *im = lv_image_create(scr);
        lv_image_set_src(im, icon(gi[i], i == 0 ? 2 : 2));
        lv_obj_set_pos(im, x, TOP + CH + 3 + (i == 2 ? 1 : 1));
        v_bar[i] = lv_bar_create(scr);
        lv_obj_set_size(v_bar[i], 52, 7);
        lv_obj_set_pos(v_bar[i], x + 19, TOP + CH + 8);
        lv_bar_set_range(v_bar[i], 0, 100);
        lv_obj_set_style_bg_color(v_bar[i], C_OFF, LV_PART_MAIN);
        lv_obj_set_style_bg_color(v_bar[i], lv_color_hex(gc[i]), LV_PART_INDICATOR);
        lv_obj_set_style_radius(v_bar[i], 3, LV_PART_MAIN);
        lv_obj_set_style_radius(v_bar[i], 3, LV_PART_INDICATOR);
        lv_obj_set_style_anim_duration(v_bar[i], 400, 0);
    }

    /* actions */
    v_btnrow = lv_obj_create(scr);
    lv_obj_remove_style_all(v_btnrow);
    lv_obj_set_size(v_btnrow, 240, 40);
    lv_obj_set_pos(v_btnrow, 0, 198);
    ui_bubble(v_btnrow);
    static const puca_spr_t *const bi[5] = { &puca_it_b_feed, &puca_it_b_play, &puca_it_b_clean, &puca_it_b_heal, &puca_it_b_moon };
    for (int i = 0; i < 5; i++) v_btn[i] = mk_btn(v_btnrow, 2 + i * 48, 44, bi[i], 3, act_cb, i);
    v_light_img = lv_obj_get_child(v_btn[4], 0);

    v_gamerow = lv_obj_create(scr);
    lv_obj_remove_style_all(v_gamerow);
    lv_obj_set_size(v_gamerow, 240, 40);
    lv_obj_set_pos(v_gamerow, 0, 198);
    ui_bubble(v_gamerow);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(v_gamerow);
        lv_obj_set_size(b, 114, 40);
        lv_obj_set_pos(b, 4 + i * 118, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, C_ACC, 0);
        lv_obj_add_event_cb(b, guess_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i ? 1 : -1));
        ui_bubble(b);
        lv_obj_t *l = ui_label(b, &font_title_20, C_FG, i ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
        lv_obj_center(l);
    }
    lv_obj_add_flag(v_gamerow, LV_OBJ_FLAG_HIDDEN);

    /* choix du repas */
    v_feed = lv_obj_create(scr);
    lv_obj_remove_style_all(v_feed);
    lv_obj_set_size(v_feed, 240, CH);
    lv_obj_set_pos(v_feed, 0, TOP);
    lv_obj_set_style_bg_color(v_feed, lv_color_hex(0x05060a), 0);
    lv_obj_set_style_bg_opa(v_feed, LV_OPA_70, 0);
    lv_obj_add_flag(v_feed, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_feed, feed_close_cb, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(v_feed);
        lv_obj_set_size(b, 104, 104);
        lv_obj_align(b, LV_ALIGN_CENTER, i ? 56 : -56, 0);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, C_CARD, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(i ? 0xd89a55 : 0x8a5a30), 0);
        lv_obj_add_event_cb(b, feed_choice_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *im = lv_image_create(b);
        lv_image_set_src(im, icon(i ? &puca_it_cake : &puca_it_potato, 5));
        lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_t *l = ui_label(b, &font_text_16, C_FG, i ? _("Gâteau") : _("Repas"));
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -6);
        if (i) {
            lv_obj_t *c = lv_image_create(b);
            lv_image_set_src(c, icon(&puca_it_clover, 2));
            lv_obj_align(c, LV_ALIGN_TOP_RIGHT, -2, 2);
        }
    }
    lv_obj_add_flag(v_feed, LV_OBJ_FLAG_HIDDEN);

    /* parti : nouvel œuf */
    v_gone = lv_obj_create(scr);
    lv_obj_remove_style_all(v_gone);
    lv_obj_set_size(v_gone, 240, CH);
    lv_obj_set_pos(v_gone, 0, TOP);
    lv_obj_t *gl = ui_label(v_gone, &font_text_16, C_FG, _("Ton púca est retourné au tertre des fées."));
    lv_obj_set_width(gl, 210);
    lv_label_set_long_mode(gl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(gl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(gl, lv_color_white(), 0);
    lv_obj_set_style_bg_color(gl, lv_color_hex(0x0b0d14), 0);
    lv_obj_set_style_bg_opa(gl, LV_OPA_70, 0);
    lv_obj_set_style_radius(gl, 10, 0);
    lv_obj_set_style_pad_all(gl, 6, 0);
    lv_obj_align(gl, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t *nb = ui_button(v_gone, _("Nouvel œuf"), C_ACC, new_egg_cb, NULL);
    lv_obj_align(nb, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(v_gone, LV_OBJ_FLAG_HIDDEN);

    v_noclock = ui_label(v_canvas, &font_text_16, C_FG, _("Heure inconnue : connecte l'iPhone"));
    lv_obj_set_style_text_color(v_noclock, lv_color_white(), 0);
    lv_obj_center(v_noclock);
    lv_obj_add_flag(v_noclock, LV_OBJ_FLAG_HIDDEN);

    p_x = 30; p_tx = 30; p_dir = 1; p_jump = 0;
    refresh_ui();
    render();
}

static void destroy(void)
{
    if (v_tmr) { lv_timer_delete(v_tmr); v_tmr = NULL; }
    save();
    heap_caps_free(v_buf);
    v_buf = NULL;
    for (int i = 0; i < s_nicons; i++) heap_caps_free(s_icons[i].px);
    s_nicons = 0;
    v_canvas = v_msg = v_hint = v_stage = v_spk = v_clov_lbl = v_clov_img = NULL;
    v_btnrow = v_gamerow = v_feed = v_gone = v_noclock = v_light_img = NULL;
    for (int i = 0; i < 3; i++) v_bar[i] = NULL;
    for (int i = 0; i < 5; i++) v_btn[i] = NULL;
}

static void on_show(void)
{
    load();
    model_update();
    handle_events();
    if (!v_tmr) v_tmr = lv_timer_create(tick_cb, TICK_MS, NULL);
    else lv_timer_resume(v_tmr);
    refresh_ui();
}

static void on_hide(void)
{
    if (v_tmr) lv_timer_pause(v_tmr);
    if (a_kind == A_GAME) {                   /* partie interrompue : comptée telle quelle */
        if (g_phase < 2) { puca_play_done(&m, g_wins, G_ROUNDS); }
        lv_obj_add_flag(v_gamerow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v_btnrow, LV_OBJ_FLAG_HIDDEN);
    }
    if (a_kind == A_EVOLVE) m.prev_stage = m.stage;
    a_kind = A_IDLE;
    p_jump = 0;
    if (v_feed) lv_obj_add_flag(v_feed, LV_OBJ_FLAG_HIDDEN);
    save();
}

const app_t app_puca = {
    .name = "Púca",
    .icon = SYM_GHOST,
    .color = 0x7C5CFF,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
};
