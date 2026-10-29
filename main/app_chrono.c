/*
 * app_chrono.c — Chronomètre au dixième, avec les trois derniers tours.
 * Le temps vient d'esp_timer (continu, light sleep compris) : le chrono
 * continue de tourner écran éteint ou app fermée, sans rien consommer.
 */
#include <stdio.h>
#include "app.h"
#include "ui_kit.h"
#include "esp_timer.h"

#define LAPS 3

/* modèle (survit à la vue) */
static bool    s_run;
static int64_t s_t0;         /* esp_timer au dernier départ */
static int64_t s_acc;        /* µs accumulées avant ce départ */
static int64_t s_lap_mark;   /* temps total au dernier tour */
static int64_t s_laps[LAPS]; /* durées des derniers tours, [0] = le plus récent */
static int     s_nlaps;

/* vue */
static lv_obj_t *v_time, *v_laps, *v_left, *v_right;
static lv_timer_t *v_tick;

static int64_t elapsed(void)
{
    return s_acc + (s_run ? esp_timer_get_time() - s_t0 : 0);
}

static void fmt(char *b, size_t n, int64_t us)
{
    int64_t ds = us / 100000;              /* dixièmes */
    if (ds > 99LL * 36000) ds = 99LL * 36000;   /* plafond : 99 h */
    int m = (int)(ds / 600), s = (int)(ds / 10 % 60), t = (int)(ds % 10);
    if (m >= 60) snprintf(b, n, "%d:%02d:%02d", m / 60, m % 60, s);
    else snprintf(b, n, "%02d:%02d.%d", m, s, t);
}

static void refresh(void)
{
    if (!v_time) return;
    char b[24];
    fmt(b, sizeof(b), elapsed());
    lv_label_set_text(v_time, b);

    char l[128] = "";
    size_t o = 0;
    for (int i = 0; i < s_nlaps && i < LAPS; i++) {
        char t[24];
        fmt(t, sizeof(t), s_laps[i]);
        o += snprintf(l + o, sizeof(l) - o, "%sTour %d   %s", i ? "\n" : "", s_nlaps - i, t);
        if (o >= sizeof(l)) break;
    }
    lv_label_set_text(v_laps, l);

    lv_label_set_text(lv_obj_get_child(v_right, 0), s_run ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_color(v_right, s_run ? C_WARN : C_OK, 0);
    bool idle = !s_run && elapsed() == 0;
    lv_label_set_text(lv_obj_get_child(v_left, 0), s_run ? "Tour" : "Zéro");
    if (idle) lv_obj_add_state(v_left, LV_STATE_DISABLED);
    else lv_obj_remove_state(v_left, LV_STATE_DISABLED);
}

static void tick_cb(lv_timer_t *t) { if (s_run) refresh(); }

static void right_cb(lv_event_t *e)
{
    if (s_run) { s_acc = elapsed(); s_run = false; }
    else { s_t0 = esp_timer_get_time(); s_run = true; }
    refresh();
}

static void left_cb(lv_event_t *e)
{
    if (s_run) {
        int64_t now = elapsed();
        for (int i = LAPS - 1; i > 0; i--) s_laps[i] = s_laps[i - 1];
        s_laps[0] = now - s_lap_mark;
        s_lap_mark = now;
        s_nlaps++;
    } else {
        s_acc = s_lap_mark = 0;
        s_nlaps = 0;
    }
    refresh();
}

static void create(lv_obj_t *scr)
{
    ui_header(scr, "Chrono");
    v_time = ui_label(scr, &font_num_44, C_FG, "");
    lv_obj_align(v_time, LV_ALIGN_TOP_MID, 0, 56);
    v_laps = ui_label(scr, &font_text_16, C_DIM, "");
    lv_obj_set_style_text_align(v_laps, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v_laps, LV_ALIGN_TOP_MID, 0, 112);

    v_left = ui_button(scr, "", C_CARD, left_cb, NULL);
    lv_obj_set_size(v_left, 104, 50);
    lv_obj_align(v_left, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    v_right = ui_button(scr, "", C_OK, right_cb, NULL);
    lv_obj_set_size(v_right, 104, 50);
    lv_obj_align(v_right, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
    lv_obj_set_style_text_font(lv_obj_get_child(v_right, 0), &font_title_20, 0);

    v_tick = lv_timer_create(tick_cb, 100, NULL);
    refresh();
}

static void destroy(void)
{
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    v_time = v_laps = v_left = v_right = NULL;
}

static void on_show(void) { if (v_tick) lv_timer_resume(v_tick); refresh(); }
static void on_hide(void) { if (v_tick) lv_timer_pause(v_tick); }

const app_t app_chrono = {
    .name = "Chrono",
    .icon = SYM_STOPWATCH,
    .color = 0x3B82F6,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
};
