/*
 * app_timer.c — Minuteur. Le décompte est un esp_timer one-shot : il réveille
 * la montre à l'échéance même écran éteint, poste un événement d'app, et le
 * système rouvre l'app qui sonne (vibrations répétées jusqu'à « OK »).
 */
#include <stdio.h>
#include <time.h>
#include "app.h"
#include "ui_kit.h"
#include "esp_timer.h"
#include "power.h"

#define EV_DONE  1
#define BUZZ_MAX 12              /* vibrations (toutes les 2,5 s) avant abandon */

extern const app_t app_timer;

/* modèle */
static int32_t s_set_s = 5 * 60; /* durée choisie */
static int64_t s_left_us;        /* restant quand en pause */
static int64_t s_end;            /* esp_timer à l'échéance quand en marche */
static bool    s_run, s_done;
static int     s_buzz;
static esp_timer_handle_t s_alarm;

/* vue */
static lv_obj_t *v_time, *v_info, *v_presets, *v_left, *v_right;
static lv_timer_t *v_tick;

static const int PRESETS_MIN[] = { 1, 3, 5, 10 };
#define NPRESETS (int)(sizeof(PRESETS_MIN) / sizeof(PRESETS_MIN[0]))

static void alarm_cb(void *arg)                 /* tâche esp_timer */
{
    app_post_event(&app_timer, EV_DONE);
}

static int64_t remaining(void)
{
    if (s_run) { int64_t r = s_end - esp_timer_get_time(); return r > 0 ? r : 0; }
    return s_left_us;
}

static void refresh(void)
{
    if (!v_time) return;
    int64_t r = remaining();
    int s = (int)((r + 999999) / 1000000);       /* arrondi au-dessus : 0 = fini */
    char b[24];
    if (s >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, sizeof(b), "%02d:%02d", s / 60, s % 60);
    lv_label_set_text(v_time, b);
    lv_obj_set_style_text_color(v_time, s_done ? C_BAD : s_run ? C_FG : C_DIM, 0);

    bool idle = !s_run && !s_done && s_left_us == (int64_t)s_set_s * 1000000;
    if (idle) lv_obj_remove_flag(v_presets, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(v_presets, LV_OBJ_FLAG_HIDDEN);

    if (s_done) {
        lv_label_set_text(v_info, "Terminé !");
        lv_obj_set_style_text_color(v_info, C_BAD, 0);
    } else if (s_run) {
        time_t now = time(NULL) + (time_t)(r / 1000000);
        struct tm tm;
        localtime_r(&now, &tm);
        lv_label_set_text_fmt(v_info, "Sonne à %02d:%02d", tm.tm_hour, tm.tm_min);
        lv_obj_set_style_text_color(v_info, C_DIM, 0);
    } else {
        lv_label_set_text(v_info, idle ? "" : "En pause");
        lv_obj_set_style_text_color(v_info, C_DIM, 0);
    }
    if (idle) lv_obj_add_flag(v_info, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(v_info, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *rl = lv_obj_get_child(v_right, 0);
    if (s_done) {
        lv_label_set_text(rl, "OK");
        lv_obj_set_style_bg_color(v_right, C_ACC, 0);
    } else {
        lv_label_set_text(rl, s_run ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_obj_set_style_bg_color(v_right, s_run ? C_WARN : C_OK, 0);
    }
    lv_label_set_text(lv_obj_get_child(v_left, 0), idle ? "+1 min" : "Annuler");
}

static void stop_alarm(void)
{
    if (s_alarm) esp_timer_stop(s_alarm);
}

static void reset(void)
{
    stop_alarm();
    s_run = s_done = false;
    s_left_us = (int64_t)s_set_s * 1000000;
}

static void right_cb(lv_event_t *e)
{
    if (s_done) { reset(); refresh(); return; }
    if (s_run) {
        s_left_us = remaining();
        s_run = false;
        stop_alarm();
    } else if (s_left_us > 0) {
        if (!s_alarm) {
            const esp_timer_create_args_t a = { .callback = alarm_cb, .name = "minuteur" };
            if (esp_timer_create(&a, &s_alarm) != ESP_OK) { ui_toast("Minuteur indisponible"); return; }
        }
        s_end = esp_timer_get_time() + s_left_us;
        esp_timer_start_once(s_alarm, (uint64_t)s_left_us);
        s_run = true;
    }
    refresh();
}

static void left_cb(lv_event_t *e)
{
    bool idle = !s_run && !s_done && s_left_us == (int64_t)s_set_s * 1000000;
    if (idle) {
        if (s_set_s < 99 * 60) s_set_s += 60;
        s_left_us = (int64_t)s_set_s * 1000000;
    } else {
        reset();
    }
    refresh();
}

static void preset_cb(lv_event_t *e)
{
    s_set_s = PRESETS_MIN[(int)(uintptr_t)lv_event_get_user_data(e)] * 60;
    s_left_us = (int64_t)s_set_s * 1000000;
    refresh();
}

static void tick_cb(lv_timer_t *t)
{
    if (s_run) refresh();
    static int n;
    if (s_done && s_buzz > 0 && s_buzz < BUZZ_MAX && ++n >= 10) {   /* 10 × 250 ms */
        n = 0;
        s_buzz++;
        power_vibrate(VIB_CALL);
    }
}

static void create(lv_obj_t *scr)
{
    if (!s_run && !s_done && s_left_us == 0) s_left_us = (int64_t)s_set_s * 1000000;
    ui_header(scr, "Minuteur");
    v_time = ui_label(scr, &font_num_44, C_FG, "");
    lv_obj_align(v_time, LV_ALIGN_TOP_MID, 0, 52);

    v_info = ui_label(scr, &font_title_20, C_DIM, "");
    lv_obj_align(v_info, LV_ALIGN_TOP_MID, 0, 120);

    v_presets = ui_container(scr);
    lv_obj_set_size(v_presets, 228, 44);
    lv_obj_align(v_presets, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_remove_flag(v_presets, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(v_presets, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(v_presets, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < NPRESETS; i++) {
        char b[8];
        snprintf(b, sizeof(b), "%d'", PRESETS_MIN[i]);
        lv_obj_t *p = ui_button(v_presets, b, C_CARD, preset_cb, (void *)(uintptr_t)i);
        lv_obj_set_size(p, 50, 42);
    }

    v_left = ui_button(scr, "", C_CARD, left_cb, NULL);
    lv_obj_set_size(v_left, 104, 50);
    lv_obj_align(v_left, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    v_right = ui_button(scr, "", C_OK, right_cb, NULL);
    lv_obj_set_size(v_right, 104, 50);
    lv_obj_align(v_right, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
    lv_obj_set_style_text_font(lv_obj_get_child(v_right, 0), &font_title_20, 0);

    v_tick = lv_timer_create(tick_cb, 250, NULL);
    refresh();
}

static void destroy(void)
{
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    v_time = v_info = v_presets = v_left = v_right = NULL;
}

static void on_show(void) { if (v_tick) lv_timer_resume(v_tick); refresh(); }
static void on_hide(void)
{
    if (v_tick) lv_timer_pause(v_tick);
    if (s_done) reset();               /* quitter l'alarme = l'acquitter */
}

static bool on_event(int code)
{
    if (code != EV_DONE || !s_run) return false;
    s_run = false;
    s_done = true;
    s_left_us = 0;
    s_buzz = 1;
    power_vibrate(VIB_CALL);
    refresh();
    return true;                       /* ouvrir l'app et allumer l'écran */
}

const app_t app_timer = {
    .name = "Minuteur",
    .icon = SYM_HOURGLASS,
    .color = 0xF59E0B,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .on_event = on_event,
    .screen_timeout_ms = 30000,        /* l'alarme reste visible */
};
