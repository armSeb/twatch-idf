/*
 * app_steps.c — « Pas » : pas du jour et des 7 jours précédents.
 *
 *   ‹ Pas
 *        4 512                 pas du jour (compteur du BMA423)
 *    3,2 km · moy. 6 230/j
 *    ▂ ▅ ▃ █ ▆ ▂ ▄ ▇            8 barres : J-7 … aujourd'hui (en bleu)
 *    L M M J V S D L
 *
 * Compteur désactivé : explication et bouton pour l'activer (même réglage
 * que Réglages › Mouvement).
 */
#include <stdio.h>
#include <time.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "motion.h"
#include "settings.h"
#include "wclock.h"

#define NB      (MOTION_HIST_DAYS + 1)
#define BAR_W   18
#define BAR_GAP 8
#define BAR_H   84
#define STEP_M  0.7f                    /* longueur de pas moyenne (m) */

static lv_obj_t *v_num, *v_sub, *v_bar[NB], *v_day[NB], *v_off, *v_chart;
static lv_timer_t *v_tmr;

static void fmt_steps(char *b, size_t n, uint32_t v)
{
    if (v >= 1000) snprintf(b, n, "%lu %03lu", (unsigned long)(v / 1000), (unsigned long)(v % 1000));
    else snprintf(b, n, "%lu", (unsigned long)v);
}

static void refresh(void)
{
    if (!v_num) return;
    bool on = motion_steps_on();
    if (on) { lv_obj_add_flag(v_off, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(v_chart, LV_OBJ_FLAG_HIDDEN); }
    else    { lv_obj_remove_flag(v_off, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(v_chart, LV_OBJ_FLAG_HIDDEN); }

    uint32_t h[MOTION_HIST_DAYS], d[NB];
    uint32_t today = motion_steps_today();
    motion_history(h);
    for (int i = 0; i < MOTION_HIST_DAYS; i++) d[NB - 2 - i] = h[i];   /* d[0] = J-7 … d[NB-1] = aujourd'hui */
    d[NB - 1] = today;

    char b[48];
    fmt_steps(b, sizeof b, today);
    lv_label_set_text(v_num, b);
    lv_obj_set_style_text_color(v_num, on ? C_FG : C_DIM, 0);

    uint32_t sum = 0, ndays = 0, max = 1000;
    for (int i = 0; i < NB; i++) if (d[i] > max) max = d[i];
    for (int i = 0; i < MOTION_HIST_DAYS; i++) if (h[i]) { sum += h[i]; ndays++; }
    float km = today * STEP_M / 1000.0f;
    char avg[16] = "";
    if (ndays) fmt_steps(avg, sizeof avg, sum / ndays);
    if (ndays) snprintf(b, sizeof b, _("%.1f km · moy. %s/j"), km, avg);
    else snprintf(b, sizeof b, _("%.1f km aujourd'hui"), km);
    if (i18n_get() == LANG_FR) for (char *p = b; *p; p++) if (*p == '.' && p[1] >= '0' && p[1] <= '9') *p = ',';
    lv_label_set_text(v_sub, b);

    static const char *const INI[] = { N_("D"), N_("L"), N_("Ma"), N_("Me"), N_("J"), N_("V"), N_("S") };
    struct tm tm;
    wclock_local_tm(&tm);
    for (int i = 0; i < NB; i++) {
        int hpx = (int)((uint64_t)d[i] * BAR_H / max);
        if (hpx < 3) hpx = 3;
        lv_obj_set_height(v_bar[i], hpx);
        lv_obj_set_style_bg_color(v_bar[i], i == NB - 1 ? C_ACC : (d[i] ? lv_color_hex(0x475569) : C_OFF), 0);
        int wd = ((tm.tm_wday - (NB - 1 - i)) % 7 + 7) % 7;
        lv_label_set_text(v_day[i], _(INI[wd]));
        lv_obj_set_style_text_color(v_day[i], i == NB - 1 ? C_FG : C_DIM, 0);
    }
}

static void tick_cb(lv_timer_t *t) { refresh(); }

static void enable_cb(lv_event_t *e)
{
    settings_get()->step_count = true;
    settings_save();
    motion_apply();
    if (!motion_steps_on()) ui_toast(_("Capteur de mouvement indisponible"));
    refresh();
}

static void create(lv_obj_t *scr)
{
    int y = ui_header(scr, _("Pas"));
    v_num = ui_label(scr, &font_num_44, C_FG, "0");
    lv_obj_align(v_num, LV_ALIGN_TOP_MID, 0, y - 4);
    v_sub = ui_label(scr, &font_small_11, C_DIM, "");
    lv_obj_align(v_sub, LV_ALIGN_TOP_MID, 0, y + 44);

    v_chart = ui_container(scr);
    lv_obj_set_size(v_chart, NB * (BAR_W + BAR_GAP), BAR_H + 18);
    lv_obj_align(v_chart, LV_ALIGN_BOTTOM_MID, 0, -6);
    ui_bubble(v_chart);
    for (int i = 0; i < NB; i++) {
        int x = i * (BAR_W + BAR_GAP) + BAR_GAP / 2;
        v_bar[i] = lv_obj_create(v_chart);
        lv_obj_remove_style_all(v_bar[i]);
        lv_obj_set_style_bg_opa(v_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(v_bar[i], 4, 0);
        lv_obj_set_width(v_bar[i], BAR_W);
        lv_obj_align(v_bar[i], LV_ALIGN_BOTTOM_LEFT, x, -18);
        ui_bubble(v_bar[i]);
        v_day[i] = ui_label(v_chart, &font_small_11, C_DIM, "");
        lv_obj_align(v_day[i], LV_ALIGN_BOTTOM_LEFT, x, 0);
        lv_obj_set_width(v_day[i], BAR_W);
        lv_obj_set_style_text_align(v_day[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    v_off = ui_container(scr);
    lv_obj_set_size(v_off, 220, 110);
    lv_obj_align(v_off, LV_ALIGN_BOTTOM_MID, 0, -6);
    ui_bubble(v_off);
    lv_obj_t *m = ui_label(v_off, &font_small_11, C_DIM,
                           _("Compteur désactivé. Le capteur compte seul, l'ESP32 ne fait rien : quelques µA en plus."));
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, 210);
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *bt = ui_button(v_off, _("Activer le compteur"), C_ACC, enable_cb, NULL);
    lv_obj_set_size(bt, 200, 44);
    lv_obj_align(bt, LV_ALIGN_BOTTOM_MID, 0, 0);

    refresh();
    v_tmr = lv_timer_create(tick_cb, 5000, NULL);
}

static void destroy(void)
{
    if (v_tmr) { lv_timer_delete(v_tmr); v_tmr = NULL; }
    v_num = v_sub = v_off = v_chart = NULL;
    for (int i = 0; i < NB; i++) v_bar[i] = v_day[i] = NULL;
}

static void on_show(void) { if (v_tmr) lv_timer_resume(v_tmr); refresh(); }
static void on_hide(void) { if (v_tmr) lv_timer_pause(v_tmr); }

const app_t app_steps = {
    .name = N_("Pas"),
    .icon = SYM_WALK,
    .color = 0x22C55E,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
};
