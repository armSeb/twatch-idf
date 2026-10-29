/*
 * app_lamp.c — Lampe : écran blanc pleine luminosité ; toucher = rouge
 * (vision de nuit). La luminosité réglée revient en quittant l'app.
 */
#include "app.h"
#include "ui_kit.h"
#include "display.h"
#include "settings.h"

static lv_obj_t *s_scr, *s_hint;
static bool s_red;

static void paint(void)
{
    if (!s_scr) return;
    lv_obj_set_style_bg_color(s_scr, s_red ? lv_color_hex(0xFF1A00) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(s_hint, s_red ? lv_color_hex(0x7A0D00) : lv_color_hex(0x9A9A9A), 0);
}

static void tap_cb(lv_event_t *e)
{
    s_red = !s_red;
    paint();
}

static void create(lv_obj_t *scr)
{
    s_scr = scr;
    lv_obj_add_event_cb(scr, tap_cb, LV_EVENT_CLICKED, NULL);
    s_hint = ui_label(scr, &font_small_11, C_DIM, "Toucher : blanc / rouge");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    paint();
}

static void destroy(void) { s_scr = s_hint = NULL; }
static void on_show(void) { display_set_brightness(255); }
static void on_hide(void) { display_set_brightness(settings_get()->brightness); }

const app_t app_lamp = {
    .name = "Lampe",
    .icon = SYM_BULB,
    .color = 0xFACC15,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .screen_timeout_ms = 5 * 60 * 1000,
};
