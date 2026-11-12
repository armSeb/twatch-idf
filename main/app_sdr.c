/*
 * app_sdr.c — « SDR » : analyseur de spectre et waterfall 2,4 GHz à partir
 * des échantillons I/Q bruts de la radio Wi-Fi de l'ESP32 (voir sdr_rx.c).
 *
 * L'app vit en « mode SDR » (sdr_mode.h) : l'ouvrir redémarre la montre sans
 * Bluetooth ni Wi-Fi client, la quitter redémarre en mode normal.
 *
 *   ┌‹ titre / marqueur ───────── [+] [G]┐  32 px
 *   │ spectre (trace + maintien de crête) │  56 px
 *   │ waterfall (la plus récente en haut) │ 136 px
 *   └ graduation des fréquences ──────────┘  16 px
 *
 * Toucher : marqueur (fréquence, niveau). Appui long (vue bande) : zoom sur le
 * canal touché. Vue zoom : glisser ← → change de canal. Titre : pause.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "display.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdr_mode.h"
#include "sdr_rx.h"

extern const app_t app_sdr;

#define TOP_H   32
#define SPEC_H  56
#define WF_H    136
#define AXIS_H  16
#define SPAN_DB 48.0f                /* dynamique affichée */

/* modèle (survit à la veille) */
static int   m_view = SDR_VIEW_BAND, m_ch = 6, m_gain = 3;
static bool  m_paused, m_started;
static int   m_mark = -1;            /* colonne du marqueur, -1 = aucun */
static float m_floor = -90.0f;       /* bruit de fond estimé (dBFS) */
static float m_last[SDR_W], m_peak[SDR_W];
static bool  m_have, m_sat;
static esp_timer_handle_t m_boot_t;

/* vue */
static lv_obj_t *v_mline, *v_title, *v_sat, *v_minfo, *v_zoom, *v_gain, *v_spec, *v_wf, *v_touch, *v_axis[5], *v_msg;
static uint16_t *v_spec_buf, *v_wf_buf;
static lv_timer_t *v_tick;
static uint16_t s_pal[256];

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return lv_color_to_u16(lv_color_make(r, g, b));
}

/* palette « waterfall » : noir → bleu → cyan → jaune → rouge → blanc */
static void palette_init(void)
{
    static const struct { float t; uint8_t r, g, b; } K[] = {
        { 0.00f, 0, 0, 8 }, { 0.20f, 10, 20, 110 }, { 0.42f, 0, 150, 210 },
        { 0.62f, 240, 220, 40 }, { 0.82f, 240, 60, 20 }, { 1.00f, 255, 255, 255 },
    };
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        int k = 0;
        while (k < 4 && t > K[k + 1].t) k++;
        float u = (t - K[k].t) / (K[k + 1].t - K[k].t);
        s_pal[i] = rgb565((uint8_t)(K[k].r + u * (K[k + 1].r - K[k].r)),
                          (uint8_t)(K[k].g + u * (K[k + 1].g - K[k].g)),
                          (uint8_t)(K[k].b + u * (K[k + 1].b - K[k].b)));
    }
}

static inline int level(float db)
{
    float t = (db - (m_floor - 3.0f)) / SPAN_DB;
    int v = (int)(t * 255.0f);
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* ------------------------------------------------------------------ */
/* rendu                                                                */
/* ------------------------------------------------------------------ */
static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void track_floor(const float *db)
{
    static float tmp[SDR_W];
    memcpy(tmp, db, sizeof tmp);
    qsort(tmp, SDR_W, sizeof(float), cmpf);
    float p = tmp[SDR_W / 5];                    /* 20e centile ≈ bruit de fond */
    m_floor = m_have ? m_floor + 0.08f * (p - m_floor) : p;
}

static void wf_push(const float *db)
{
    if (!v_wf_buf) return;
    memmove(v_wf_buf + SDR_W, v_wf_buf, SDR_W * (WF_H - 1) * sizeof(uint16_t));
    for (int x = 0; x < SDR_W; x++) v_wf_buf[x] = s_pal[level(db[x])];
}

static void spec_draw(void)
{
    if (!v_spec_buf) return;
    static uint16_t bg, grid, fill, line, peak, mark;
    if (!bg) {
        bg = rgb565(4, 6, 12); grid = rgb565(28, 32, 42); fill = rgb565(14, 70, 92);
        line = rgb565(94, 234, 212); peak = rgb565(250, 204, 21); mark = rgb565(255, 255, 255);
    }
    for (int i = 0; i < SDR_W * SPEC_H; i++) v_spec_buf[i] = bg;
    /* lignes tous les 12 dB */
    for (int k = 1; k < 4; k++) {
        int y = SPEC_H * k / 4;
        for (int x = 0; x < SDR_W; x += 2) v_spec_buf[y * SDR_W + x] = grid;
    }
    if (!m_have) return;
    int prev = -1;
    for (int x = 0; x < SDR_W; x++) {
        int y = SPEC_H - 1 - level(m_last[x]) * (SPEC_H - 2) / 255;
        int yp = SPEC_H - 1 - level(m_peak[x]) * (SPEC_H - 2) / 255;
        for (int r = y + 1; r < SPEC_H; r++) v_spec_buf[r * SDR_W + x] = fill;
        /* trace continue : relie au point précédent */
        int a = prev < 0 ? y : prev, b = y;
        if (a > b) { int t = a; a = b; b = t; }
        for (int r = a; r <= b; r++) v_spec_buf[r * SDR_W + x] = line;
        if (yp >= 0 && yp < SPEC_H && yp < y) v_spec_buf[yp * SDR_W + x] = peak;
        prev = y;
    }
    if (m_mark >= 0 && m_mark < SDR_W)
        for (int r = 0; r < SPEC_H; r += 2) v_spec_buf[r * SDR_W + m_mark] = mark;
    if (v_mline) {                               /* le marqueur prolongé sur le waterfall */
        if (m_mark >= 0) { lv_obj_set_x(v_mline, m_mark); lv_obj_remove_flag(v_mline, LV_OBJ_FLAG_HIDDEN); }
        else lv_obj_add_flag(v_mline, LV_OBJ_FLAG_HIDDEN);
    }
}

static void title_refresh(void)
{
    if (!v_title) return;
    char b[64];
    if (!sdr_mode_active()) return;
    const char *err = sdr_rx_error();
    if (err) snprintf(b, sizeof b, _("Erreur radio (%s)"), err);
    else if (m_paused) snprintf(b, sizeof b, "%s", _("En pause"));
    else if (m_view == SDR_VIEW_BAND) snprintf(b, sizeof b, "2,4 GHz");
    else snprintf(b, sizeof b, _("Canal %d"), m_ch);
    if (strcmp(lv_label_get_text(v_title), b)) lv_label_set_text(v_title, b);
    /* marqueur : fréquence et niveau dans le coin du spectre, côté opposé */
    if (m_mark >= 0 && m_have) {
        int f10 = (int)lrintf(sdr_rx_mhz(m_view, m_ch, m_mark + 0.5f) * 10.0f);
        snprintf(b, sizeof b, "%d,%d MHz  %d dBFS", f10 / 10, f10 % 10, (int)lrintf(m_last[m_mark]));
        if (strcmp(lv_label_get_text(v_minfo), b)) lv_label_set_text(v_minfo, b);
        lv_obj_align(v_minfo, m_mark < SDR_W / 2 ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT,
                     m_mark < SDR_W / 2 ? -4 : 4, TOP_H + (m_sat ? 20 : 3));
        lv_obj_remove_flag(v_minfo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v_minfo, LV_OBJ_FLAG_HIDDEN);
    }
    if (m_sat) lv_obj_remove_flag(v_sat, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(v_sat, LV_OBJ_FLAG_HIDDEN);
}

static void axis_refresh(void)
{
    if (!v_axis[0]) return;
    int n = 0;
    char b[24];
    if (m_view == SDR_VIEW_BAND) {
        static const int F[] = { 2410, 2430, 2450, 2470 };
        for (; n < 4; n++) {
            snprintf(b, sizeof b, "%d", F[n]);
            lv_label_set_text(v_axis[n], b);
            lv_obj_set_x(v_axis[n], (int)sdr_rx_x(m_view, m_ch, F[n]) - 14);
            lv_obj_remove_flag(v_axis[n], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        int fc = 2407 + 5 * m_ch;
        const int D[] = { -8, 0, 8 };
        for (; n < 3; n++) {
            snprintf(b, sizeof b, "%d", fc + D[n]);
            lv_label_set_text(v_axis[n], b);
            lv_obj_set_x(v_axis[n], (int)sdr_rx_x(m_view, m_ch, fc + D[n]) - 14);
            lv_obj_remove_flag(v_axis[n], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (; n < 5; n++) lv_obj_add_flag(v_axis[n], LV_OBJ_FLAG_HIDDEN);
    if (v_zoom) lv_label_set_text(lv_obj_get_child(v_zoom, 0), m_view == SDR_VIEW_BAND ? "+" : "-");
    if (v_gain) {
        if (m_gain == 0) lv_label_set_text(lv_obj_get_child(v_gain, 0), "AGC");
        else { snprintf(b, sizeof b, "G%d", m_gain); lv_label_set_text(lv_obj_get_child(v_gain, 0), b); }
    }
}

static void reset_view(void)
{
    m_have = false;
    if (v_wf_buf) for (int i = 0; i < SDR_W * WF_H; i++) v_wf_buf[i] = s_pal[0];
    for (int x = 0; x < SDR_W; x++) m_peak[x] = -200.0f;
    if (v_wf) lv_obj_invalidate(v_wf);
}

static void apply_config(void)
{
    sdr_rx_config(m_view, m_ch, m_gain);
    reset_view();
    axis_refresh();
    title_refresh();
    spec_draw();
    if (v_spec) lv_obj_invalidate(v_spec);
}

static void tick_cb(lv_timer_t *t)
{
    sdr_row_t r;
    int got = 0;
    bool sat = false;
    while (got < 8 && sdr_rx_pop(&r)) {
        if (r.view != m_view || (m_view == SDR_VIEW_ZOOM && r.ch != m_ch) || m_paused) continue;
        track_floor(r.db);
        m_have = true;
        wf_push(r.db);
        memcpy(m_last, r.db, sizeof m_last);
        for (int x = 0; x < SDR_W; x++) {
            m_peak[x] -= 0.4f;                   /* crête qui retombe doucement */
            if (r.db[x] > m_peak[x]) m_peak[x] = r.db[x];
        }
        sat |= r.sat;
        got++;
    }
    if (got) {
        m_sat = sat;
        spec_draw();
        lv_obj_invalidate(v_spec);
        lv_obj_invalidate(v_wf);
    }
    title_refresh();
    if (v_msg) {
        if (sdr_rx_error() && !strcmp(sdr_rx_error(), "capture")) lv_obj_remove_flag(v_msg, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(v_msg, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------ */
/* interaction                                                          */
/* ------------------------------------------------------------------ */
static int touch_x(void)
{
    lv_point_t p;
    lv_indev_t *in = lv_indev_active();
    if (!in) return -1;
    lv_indev_get_point(in, &p);
    return p.x < 0 ? 0 : p.x >= SDR_W ? SDR_W - 1 : p.x;
}

static void tap_cb(lv_event_t *e)
{
    int x = touch_x();
    m_mark = (m_mark >= 0 && abs(x - m_mark) < 6) ? -1 : x;   /* toucher le marqueur l'enlève */
    spec_draw();
    lv_obj_invalidate(v_spec);
    title_refresh();
}

static void long_cb(lv_event_t *e)
{
    if (m_view != SDR_VIEW_BAND) return;
    int x = touch_x();
    float f = sdr_rx_mhz(SDR_VIEW_BAND, m_ch, x + 0.5f);
    int ch = (int)lrintf((f - 2407.0f) / 5.0f);
    m_ch = ch < 1 ? 1 : ch > 13 ? 13 : ch;
    m_view = SDR_VIEW_ZOOM;
    m_mark = -1;
    apply_config();
    lv_indev_wait_release(lv_indev_active());
}

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_dir_t d = lv_indev_get_gesture_dir(in);
    if (m_view == SDR_VIEW_ZOOM && (d == LV_DIR_LEFT || d == LV_DIR_RIGHT)) {
        int ch = m_ch + (d == LV_DIR_LEFT ? 1 : -1);
        if (ch >= 1 && ch <= 13) { m_ch = ch; m_mark = -1; apply_config(); }
        lv_indev_wait_release(in);
    } else if (d == LV_DIR_RIGHT) {
        lv_indev_wait_release(in);
        app_close();                             /* vue bande : → = quitter, comme partout */
    }
}

static void zoom_cb(lv_event_t *e)
{
    if (m_view == SDR_VIEW_BAND) {
        if (m_mark >= 0) {
            float f = sdr_rx_mhz(SDR_VIEW_BAND, m_ch, m_mark + 0.5f);
            int ch = (int)lrintf((f - 2407.0f) / 5.0f);
            m_ch = ch < 1 ? 1 : ch > 13 ? 13 : ch;
        }
        m_view = SDR_VIEW_ZOOM;
    } else {
        m_view = SDR_VIEW_BAND;
    }
    m_mark = -1;
    apply_config();
}

static void gain_cb(lv_event_t *e)
{
    m_gain = (m_gain + 1) % SDR_GAIN_LEVELS;
    apply_config();
}

static void pause_cb(lv_event_t *e)
{
    m_paused = !m_paused;
    sdr_rx_pause(m_paused);
    title_refresh();
}

static void back_cb(lv_event_t *e) { app_close(); }

/* ------------------------------------------------------------------ */
/* construction                                                         */
/* ------------------------------------------------------------------ */
static lv_obj_t *top_btn(lv_obj_t *scr, const char *txt, const lv_font_t *f, int w, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 26);
    lv_obj_set_style_radius(b, 13, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A2E37), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = ui_label(b, f, C_FG, txt);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *mk_canvas(lv_obj_t *scr, uint16_t **buf, int h, int y)
{
    *buf = heap_caps_malloc(SDR_W * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*buf) *buf = heap_caps_malloc(SDR_W * h * sizeof(uint16_t), MALLOC_CAP_8BIT);
    lv_obj_t *c = lv_canvas_create(scr);
    if (*buf) lv_canvas_set_buffer(c, *buf, SDR_W, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(c, 0, y);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static void boot_cb(void *arg) { sdr_mode_reboot(true); }

static void create_splash(lv_obj_t *scr)
{
    lv_obj_t *ic = ui_label(scr, &font_title_20, lv_color_hex(0xA855F7), SYM_CHART);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -54);
    lv_obj_t *t = ui_label(scr, &font_title_20, C_FG, _("Mode SDR"));
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -22);
    lv_obj_t *m = ui_label(scr, &font_text_16, C_DIM,
                           _("Redémarrage sans Bluetooth pour libérer la radio…"));
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, 210);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, 22);
    lv_obj_t *sp = lv_spinner_create(scr);
    lv_obj_set_size(sp, 28, 28);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, lv_color_hex(0xA855F7), LV_PART_INDICATOR);
    lv_obj_align(sp, LV_ALIGN_CENTER, 0, 72);
}

static void create(lv_obj_t *scr)
{
    if (!sdr_mode_active()) { create_splash(scr); return; }
    palette_init();

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 36, TOP_H);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ar = ui_label(back, &font_title_20, C_ACC, LV_SYMBOL_LEFT);
    lv_obj_align(ar, LV_ALIGN_LEFT_MID, 10, 0);

    lv_obj_t *tb = lv_button_create(scr);       /* titre : toucher = pause */
    lv_obj_remove_style_all(tb);
    lv_obj_set_size(tb, 118, TOP_H);
    lv_obj_set_pos(tb, 34, 0);
    lv_obj_add_event_cb(tb, pause_cb, LV_EVENT_CLICKED, NULL);
    v_title = ui_label(tb, &font_text_16, C_FG, "");
    lv_label_set_long_mode(v_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v_title, 118);
    lv_obj_align(v_title, LV_ALIGN_LEFT_MID, 0, 0);

    v_zoom = top_btn(scr, "+", &font_title_20, 34, zoom_cb);
    lv_obj_align(v_zoom, LV_ALIGN_TOP_RIGHT, -52, 3);
    v_gain = top_btn(scr, "G3", &font_text_16, 46, gain_cb);
    lv_obj_align(v_gain, LV_ALIGN_TOP_RIGHT, -4, 3);

    v_spec = mk_canvas(scr, &v_spec_buf, SPEC_H, TOP_H);
    v_wf = mk_canvas(scr, &v_wf_buf, WF_H, TOP_H + SPEC_H);
    v_mline = lv_obj_create(scr);
    lv_obj_remove_style_all(v_mline);
    lv_obj_set_size(v_mline, 1, WF_H);
    lv_obj_set_y(v_mline, TOP_H + SPEC_H);
    lv_obj_set_style_bg_color(v_mline, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(v_mline, LV_OPA_40, 0);
    lv_obj_remove_flag(v_mline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v_mline, LV_OBJ_FLAG_HIDDEN);
    /* saturation du convertisseur : baisser le gain */
    v_sat = ui_label(scr, &font_small_11, C_FG, _("SATURÉ · baisser le gain"));
    lv_obj_set_style_bg_color(v_sat, lv_color_hex(0xB91C1C), 0);
    lv_obj_set_style_bg_opa(v_sat, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(v_sat, 4, 0);
    lv_obj_set_style_radius(v_sat, 4, 0);
    lv_obj_set_pos(v_sat, 4, TOP_H + 3);
    lv_obj_add_flag(v_sat, LV_OBJ_FLAG_HIDDEN);
    v_minfo = ui_label(scr, &font_small_11, C_FG, "");
    lv_obj_set_style_bg_color(v_minfo, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(v_minfo, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(v_minfo, 4, 0);
    lv_obj_set_style_radius(v_minfo, 4, 0);
    lv_obj_add_flag(v_minfo, LV_OBJ_FLAG_HIDDEN);

    /* surface tactile au-dessus du spectre et du waterfall */
    v_touch = lv_obj_create(scr);
    lv_obj_remove_style_all(v_touch);
    lv_obj_set_size(v_touch, SDR_W, SPEC_H + WF_H);
    lv_obj_set_pos(v_touch, 0, TOP_H);
    lv_obj_add_flag(v_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(v_touch, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(v_touch, tap_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(v_touch, long_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(v_touch, gesture_cb, LV_EVENT_GESTURE, NULL);

    for (int i = 0; i < 5; i++) {
        v_axis[i] = ui_label(scr, &font_small_11, C_DIM, "");
        lv_obj_set_width(v_axis[i], 28);
        lv_obj_set_style_text_align(v_axis[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_y(v_axis[i], TOP_H + SPEC_H + WF_H + 2);
    }

    v_msg = ui_label(scr, &font_text_16, C_FG, _("La radio ne répond pas.\nQuitter puis rouvrir l'app."));
    lv_obj_set_style_text_align(v_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(v_msg, lv_color_hex(0x3F1D1D), 0);
    lv_obj_set_style_bg_opa(v_msg, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(v_msg, 8, 0);
    lv_obj_set_style_radius(v_msg, 10, 0);
    lv_obj_center(v_msg);
    lv_obj_add_flag(v_msg, LV_OBJ_FLAG_HIDDEN);

    reset_view();
    axis_refresh();
    title_refresh();
    spec_draw();
    v_tick = lv_timer_create(tick_cb, 33, NULL);
    lv_timer_pause(v_tick);
}

static void destroy(void)
{
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    heap_caps_free(v_spec_buf);
    heap_caps_free(v_wf_buf);
    v_spec_buf = v_wf_buf = NULL;
    v_mline = v_title = v_sat = v_minfo = v_zoom = v_gain = v_spec = v_wf = v_touch = v_msg = NULL;
    memset(v_axis, 0, sizeof v_axis);
}

static void on_show(void)
{
    if (!sdr_mode_active()) {
        /* mode normal : courte annonce, puis redémarrage en mode SDR */
        if (!m_boot_t) {
            const esp_timer_create_args_t a = { .callback = boot_cb, .name = "sdr_boot" };
            esp_timer_create(&a, &m_boot_t);
        }
        if (m_boot_t) esp_timer_start_once(m_boot_t, 900 * 1000);
        return;
    }
    if (!m_started) {
        m_started = sdr_rx_start() == ESP_OK;
        sdr_rx_config(m_view, m_ch, m_gain);
        sdr_rx_pause(m_paused);
    }
    if (v_tick) lv_timer_resume(v_tick);
    title_refresh();
}

static void on_hide(void)
{
    if (!sdr_mode_active()) {
        if (m_boot_t) esp_timer_stop(m_boot_t);  /* parti avant le redémarrage */
        return;
    }
    if (v_tick) lv_timer_pause(v_tick);
    if (m_started) { sdr_rx_stop(); m_started = false; }   /* écran éteint : radio coupée */
    if (display_is_on()) {
        /* l'utilisateur quitte l'app : retour au mode normal (Bluetooth) */
        sdr_mode_reboot(false);
    }
}

const app_t app_sdr = {
    .name = "SDR",
    .icon = SYM_CHART,
    .color = 0xA855F7,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .screen_timeout_ms = 5 * 60 * 1000,
};
