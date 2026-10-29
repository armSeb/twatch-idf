/*
 * app_tvbgone.c — TV-B-Gone : envoie à la suite les codes « marche/arrêt »
 * de centaines de téléviseurs par la LED infrarouge (GPIO 13).
 *
 * L'émission tourne dans une tâche dédiée (une salve dure 30 à 90 s) ; le
 * périphérique RMT n'est initialisé que le temps de la salve (il bloque le
 * CPU à pleine vitesse tant qu'il est actif). Codes : composant
 * components/tvbgone (CC BY-SA 4.0, Mitch Altman & Limor Fried).
 */
#include <stdio.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "board.h"
#include "power.h"
#include "tvbgone_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EV_DONE 1

extern const app_t app_tvbgone;

/* modèle */
static volatile bool s_run;
static int s_region = 0;                 /* 0 Europe, 1 Amérique du Nord / Asie, 2 les deux */
static int s_result;                     /* 0 rien, 1 fini, 2 interrompu, 3 erreur */
static const tvbgone_core_region_t REGION[] = {
    TVBGONE_CORE_REGION_EU, TVBGONE_CORE_REGION_NA, TVBGONE_CORE_REGION_BOTH
};

/* vue */
static lv_obj_t *v_seg, *v_btn, *v_btn_lbl, *v_status, *v_bar;
static lv_timer_t *v_tick;

static void send_task(void *arg)
{
    esp_err_t e = tvbgone_core_send(REGION[s_region], TVBGONE_CORE_SEND_MODE_SINGLE);
    s_result = e == ESP_OK ? 1 : e == ESP_ERR_INVALID_STATE ? 2 : 3;
    app_post_event(&app_tvbgone, EV_DONE);
    vTaskDelete(NULL);
}

static void refresh(void)
{
    if (!v_btn) return;
    tvbgone_core_status_t st = { 0 };
    if (s_run) tvbgone_core_get_status(&st);
    lv_obj_set_style_bg_color(v_btn, s_run ? C_OFF : lv_color_hex(0xDC2626), 0);
    lv_label_set_text(v_btn_lbl, s_run ? LV_SYMBOL_STOP : LV_SYMBOL_POWER);
    if (s_run) lv_obj_add_state(v_seg, LV_STATE_DISABLED);
    else lv_obj_remove_state(v_seg, LV_STATE_DISABLED);

    if (s_run && st.total_codes) {
        lv_label_set_text_fmt(v_status, _("Code %u / %u"), (unsigned)st.current_code_number,
                              (unsigned)st.total_codes);
        lv_obj_remove_flag(v_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_range(v_bar, 0, st.total_codes);
        lv_bar_set_value(v_bar, st.current_code_number, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(v_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(v_status, s_run ? _("Démarrage…") :
                          s_result == 1 ? _("Terminé : tous les codes envoyés") :
                          s_result == 2 ? _("Interrompu") :
                          s_result == 3 ? _("Erreur infrarouge") :
                                          _("Viser la télé, puis appuyer"));
    }
}

static void btn_cb(lv_event_t *e)
{
    if (s_run) { tvbgone_core_stop(); return; }     /* la tâche se termine et prévient */
    tvbgone_core_config_t cfg;
    tvbgone_core_get_default_config(&cfg);
    cfg.ir_led_gpio = BOARD_IR_GPIO;
    if (tvbgone_core_init(&cfg) != ESP_OK) { s_result = 3; refresh(); return; }
    s_run = true;
    s_result = 0;
    if (xTaskCreate(send_task, "tvbgone", 3072, NULL, 4, NULL) != pdPASS) {
        s_run = false;
        tvbgone_core_deinit();
        s_result = 3;
    }
    refresh();
}

static void seg_cb(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(v_seg);
    if (i <= 2) s_region = (int)i;
}

static void tick_cb(lv_timer_t *t) { if (s_run) refresh(); }

static void create(lv_obj_t *scr)
{
    ui_header(scr, "TV-B-Gone");
    static const char *map[4];
    map[0] = "Europe"; map[1] = "USA"; map[2] = _("Tous"); map[3] = "";
    v_seg = ui_segmented(scr, map, seg_cb);
    lv_obj_set_size(v_seg, 216, 36);
    lv_obj_align(v_seg, LV_ALIGN_TOP_MID, 0, 44);
    lv_buttonmatrix_set_button_ctrl(v_seg, s_region, LV_BUTTONMATRIX_CTRL_CHECKED);

    v_btn = lv_button_create(scr);
    lv_obj_set_size(v_btn, 96, 96);
    lv_obj_align(v_btn, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_radius(v_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(v_btn, 0, 0);
    lv_obj_add_event_cb(v_btn, btn_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(v_btn);
    v_btn_lbl = ui_label(v_btn, &font_title_20, C_FG, "");
    lv_obj_center(v_btn_lbl);

    v_status = ui_label(scr, &font_text_16, C_DIM, "");
    lv_obj_set_width(v_status, 224);
    lv_obj_set_style_text_align(v_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v_status, LV_ALIGN_TOP_MID, 0, 192);
    v_bar = lv_bar_create(scr);
    lv_obj_set_size(v_bar, 200, 6);
    lv_obj_align(v_bar, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_set_style_bg_color(v_bar, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(v_bar, lv_color_hex(0xDC2626), LV_PART_INDICATOR);
    ui_bubble(v_bar);

    v_tick = lv_timer_create(tick_cb, 300, NULL);
    refresh();
}

static void destroy(void)
{
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    v_seg = v_btn = v_btn_lbl = v_status = v_bar = NULL;
}

static void on_show(void) { if (v_tick) lv_timer_resume(v_tick); refresh(); }
static void on_hide(void) { if (v_tick) lv_timer_pause(v_tick); }

static bool on_event(int code)
{
    if (code != EV_DONE) return false;
    s_run = false;
    tvbgone_core_deinit();             /* rend le RMT et le verrou de fréquence */
    if (s_result == 1) power_vibrate(VIB_SHORT);
    refresh();
    return false;                      /* fin de salve : pas besoin d'allumer */
}

static bool busy(void) { return s_run; }

const app_t app_tvbgone = {
    .name = "TV-B-Gone",
    .icon = SYM_TV,
    .color = 0xDC2626,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .on_event = on_event,
    .busy = busy,
    .screen_timeout_ms = 30000,
};
