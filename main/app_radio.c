/*
 * app_radio.c — lecteur de webradios.
 *
 *   ‹  ● Radio …………………………  🔊        barre : retour, stations (liste), son
 *        FIP                            station (nom)
 *     Artiste – Titre en cours          titre ICY, défilant
 *   MP3 · 128 kbit/s · 44,1 kHz ▮▮▮▯    format, tampon
 *     [ ⏮ ]   [ ■ / ▶ ]   [ ⏭ ]
 *
 * Écran éteint (délai normal ou bouton), l'écoute continue avec Wi-Fi et CPU
 * au plus bas ; quitter l'app l'arrête.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "radio.h"
#include "audio_fx.h"
#include "esp_heap_caps.h"

extern const app_t app_radio;

#define SYM_RADIO SYM_PODCAST
#define TOP_H 32

static bool m_want = true;
static rad_info_t m_i;
static rad_station_t *m_st;
static int m_nst;

static lv_obj_t *v_scr, *v_dot, *v_head, *v_name, *v_title, *v_fmt, *v_buf, *v_status, *v_st1, *v_st2;
static lv_obj_t *v_play, *v_play_lbl, *v_prev, *v_next, *v_setup, *v_spk;
static lv_obj_t *v_sheet, *v_sheet_title, *v_sheet_list, *v_vol_lbl, *v_tone_lbl;
static lv_timer_t *v_tick;
static int v_sheet_kind;
enum { SH_NONE, SH_ST, SH_SOUND };

static void refresh(void);

static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static bool configured(void)
{
    if (!m_st) return false;
    m_nst = rad_cfg_load(m_st);
    if (!m_nst) return false;
    wrx_cfg_t *c = heap_caps_malloc(sizeof *c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) return false;
    wrx_cfg_load(c);
    bool net = false;
    for (int i = 0; i < WRX_NETS; i++) net |= c->ssid[i][0] != 0;
    free(c);
    return net;
}

/* ------------------------------------------------------------------ */
static void start(void)
{
    if (!configured()) {
        lv_obj_remove_flag(v_setup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(v_setup);
        return;
    }
    lv_obj_add_flag(v_setup, LV_OBJ_FLAG_HIDDEN);
    if (radio_start() != ESP_OK) { ui_toast(_("Wi-Fi indisponible (mémoire ?)")); return; }
    if (radio_ble_paused()) ui_toast(_("Bluetooth en pause pendant l'écoute"));
}

static void play_cb(lv_event_t *e)
{
    if (radio_running()) { m_want = false; radio_stop(); }
    else { m_want = true; start(); }
    refresh();
}

static void step_station(int d)
{
    if (m_nst < 2) return;
    int i = (m_i.station + d + m_nst) % m_nst;
    radio_station(i);
    refresh();
}
static void prev_cb(lv_event_t *e) { step_station(-1); }
static void next_cb(lv_event_t *e) { step_station(+1); }
static void status_cb(lv_event_t *e) { if (m_i.state == RAD_FAIL) radio_retry(); }
static void back_cb(lv_event_t *e) { app_close(); }

static void setup_cb(lv_event_t *e)
{
    if (v_sheet) lv_obj_add_flag(v_sheet, LV_OBJ_FLAG_HIDDEN);
    v_sheet_kind = SH_NONE;
    app_open_settings_portal(&app_radio);
}

/* ------------------------------------------------------------------ */
/* affichage                                                            */
/* ------------------------------------------------------------------ */
static void status_draw(void)
{
    char t[96] = "", d[96] = "";
    const char *msg = m_i.msg ? _(m_i.msg) : NULL;
    lv_color_t col = C_FG;
    bool run = radio_running();
    if (!run) {
        snprintf(t, sizeof t, "%s", _("Arrêté"));
        col = C_DIM;
    } else if (m_i.state == RAD_FAIL) {
        col = C_BAD;
        snprintf(t, sizeof t, "%s", msg ? msg : _("Connexion impossible"));
        int n = snprintf(d, sizeof d, "%s%s", m_i.detail, m_i.detail[0] ? " · " : "");
        if (m_i.retry_s > 0) snprintf(d + n, sizeof d - n, _("nouvel essai dans %d s"), m_i.retry_s);
        else snprintf(d + n, sizeof d - n, "%s", _("toucher pour réessayer"));
    } else if (m_i.state != RAD_PLAY) {
        snprintf(t, sizeof t, "%s", msg ? msg : "");
        int n = snprintf(d, sizeof d, "%s%s", m_i.detail, m_i.detail[0] && m_i.retry_s > 0 ? " · " : "");
        if (m_i.retry_s > 0) snprintf(d + n, sizeof d - n, _("nouvel essai dans %d s"), m_i.retry_s);
        if (m_i.state == RAD_BUFFER && !d[0]) snprintf(d, sizeof d, "%d %%", m_i.fill_pct > 100 ? 100 : m_i.fill_pct);
    }
    if (t[0]) {
        set_text(v_st1, t);
        set_text(v_st2, d);
        lv_obj_set_style_text_color(v_st1, col, 0);
        lv_obj_remove_flag(v_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v_title, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh(void)
{
    if (!v_scr) return;
    radio_info(&m_i);
    bool run = radio_running(), play = run && m_i.state == RAD_PLAY;
    lv_obj_set_style_bg_color(v_dot, !run ? C_OFF : play ? C_OK : m_i.state == RAD_FAIL ? C_BAD : C_WARN, 0);
    /* station */
    char nm[48];
    if (m_i.name[0]) strlcpy(nm, m_i.name, sizeof nm);
    else if (m_st && m_i.station < m_nst) rad_label(&m_st[m_i.station], nm, sizeof nm);
    else strlcpy(nm, "Radio", sizeof nm);
    set_text(v_name, nm);
    char hb[24];
    if (m_nst > 1) snprintf(hb, sizeof hb, "Radio · %d/%d", m_i.station + 1, m_nst);
    else snprintf(hb, sizeof hb, "Radio");
    set_text(v_head, hb);
    set_text(v_title, m_i.title[0] ? m_i.title : (play ? "" : ""));
    /* format et tampon */
    char f[64] = "";
    if (play && m_i.codec[0]) {
        int n = snprintf(f, sizeof f, "%s", m_i.codec);
        if (m_i.kbps > 0) n += snprintf(f + n, sizeof f - n, " · %d kbit/s", m_i.kbps);
        if (m_i.rate > 0) n += snprintf(f + n, sizeof f - n, " · %d%c%d kHz", m_i.rate / 1000,
                                        i18n_get() == LANG_FR ? ',' : '.', (m_i.rate % 1000) / 100);
        if (m_i.boost) snprintf(f + n, sizeof f - n, " ·  CPU+");
    }
    set_text(v_fmt, f);
    /* tampon : seulement quand il veut dire quelque chose */
    bool showbuf = run && (m_i.state == RAD_PLAY || m_i.state == RAD_BUFFER);
    int bv = m_i.state == RAD_BUFFER ? (m_i.fill_pct > 100 ? 100 : m_i.fill_pct) : m_i.buf_pct;
    lv_bar_set_value(v_buf, showbuf ? bv : 0, LV_ANIM_OFF);
    if (showbuf) lv_obj_remove_flag(v_buf, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(v_buf, LV_OBJ_FLAG_HIDDEN);
    set_text(v_spk, m_i.volume > 30 ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(v_spk, m_i.volume ? C_FG : C_WARN, 0);
    set_text(v_play_lbl, run ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_color(v_play, run ? lv_color_hex(0xDC2626) : lv_color_hex(0x16A34A), 0);
    lv_obj_set_state(v_prev, LV_STATE_DISABLED, m_nst < 2);
    lv_obj_set_state(v_next, LV_STATE_DISABLED, m_nst < 2);
    status_draw();
}

static void tick_cb(lv_timer_t *t) { refresh(); }

/* ------------------------------------------------------------------ */
/* feuilles : stations, son                                             */
/* ------------------------------------------------------------------ */
static void sheet_close(void)
{
    v_sheet_kind = SH_NONE;
    v_vol_lbl = v_tone_lbl = NULL;
    if (v_sheet) lv_obj_add_flag(v_sheet, LV_OBJ_FLAG_HIDDEN);
}

static void sheet_back_cb(lv_event_t *e) { sheet_close(); }

static void sheet_gesture_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in && lv_indev_get_gesture_dir(in) == LV_DIR_RIGHT) {
        sheet_close();
        lv_indev_wait_release(in);
    }
}

static lv_obj_t *sheet_row(const char *txt, bool cur, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_button_create(v_sheet_list);
    lv_obj_set_size(b, lv_pct(100), 42);
    lv_obj_set_style_bg_color(b, cur ? lv_color_hex(0x1E3A5F) : C_CARD, 0);
    lv_obj_set_style_bg_color(b, C_OFF, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    ui_bubble(b);
    lv_obj_t *l = ui_label(b, &font_text_16, cur ? C_ACC : C_FG, txt);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, lv_pct(100), 20);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return b;
}

static void sheet_open(int kind, const char *title)
{
    lv_obj_clean(v_sheet_list);
    v_vol_lbl = v_tone_lbl = NULL;
    v_sheet_kind = kind;
    set_text(v_sheet_title, title);
    lv_obj_scroll_to_y(v_sheet_list, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(v_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v_sheet);
}

static void st_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    sheet_close();
    radio_station(i);
    if (!radio_running()) { m_want = true; start(); }
    refresh();
}

static void stations_cb(lv_event_t *e)
{
    sheet_open(SH_ST, _("Stations"));
    if (m_st) m_nst = rad_cfg_load(m_st);
    for (int i = 0; i < m_nst; i++) {
        char nm[48];
        rad_label(&m_st[i], nm, sizeof nm);
        lv_obj_t *r = sheet_row(nm, i == m_i.station, st_pick_cb, i);
        if (i == m_i.station) lv_obj_scroll_to_view(r, LV_ANIM_OFF);
    }
    char t[48];
    snprintf(t, sizeof t, LV_SYMBOL_SETTINGS "  %s", _("Wi-Fi et radios"));
    sheet_row(t, false, setup_cb, 0);
}

static const char *const TONE[TONE_LEVELS] = { N_("Neutre"), N_("Claire"), N_("Voix"), N_("Douce"), N_("Sourde") };

static void vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    radio_volume(v);
    char b[8];
    snprintf(b, sizeof b, "%d %%", v);
    set_text(v_vol_lbl, b);
}

static void tone_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    set_text(v_tone_lbl, _(TONE[v]));
    radio_tone(v);
}

static lv_obj_t *mk_slider(int lo, int hi, int val, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(v_sheet_list);
    lv_obj_set_size(s, lv_pct(92), 14);
    lv_obj_set_style_margin_top(s, 10, 0);
    lv_obj_set_style_margin_bottom(s, 10, 0);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, C_ACC, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, C_FG, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 12);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return s;
}

static void kv(const char *k, lv_obj_t **val)
{
    lv_obj_t *r = ui_container(v_sheet_list);
    lv_obj_set_size(r, lv_pct(100), 22);
    ui_bubble(r);
    ui_label(r, &font_text_16, C_FG, k);
    *val = ui_label(r, &font_text_16, C_ACC, "");
    lv_obj_align(*val, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void sound_cb(lv_event_t *e)
{
    sheet_open(SH_SOUND, _("Son"));
    kv(_("Volume"), &v_vol_lbl);
    mk_slider(0, 100, m_i.volume, vol_cb);
    char b[8];
    snprintf(b, sizeof b, "%d %%", m_i.volume);
    set_text(v_vol_lbl, b);
    kv(_("Tonalité"), &v_tone_lbl);
    int t = m_i.tone < TONE_LEVELS ? m_i.tone : 1;
    mk_slider(0, TONE_LEVELS - 1, t, tone_cb);
    set_text(v_tone_lbl, _(TONE[t]));
    lv_obj_t *h = ui_label(v_sheet_list, &font_small_11, C_DIM,
                           _("Tonalité : vers la droite, moins d'aigus et de souffle, plus de corps aux voix."));
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, lv_pct(100));
}

/* ------------------------------------------------------------------ */
/* construction                                                         */
/* ------------------------------------------------------------------ */
static lv_obj_t *flat_btn(lv_obj_t *p, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(b);
    return b;
}

static lv_obj_t *round_btn(lv_obj_t *scr, const char *txt, lv_event_cb_t cb, lv_color_t bg, int x)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 64, 52);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -6);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x22252B), LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(b);
    lv_obj_center(ui_label(b, &font_title_20, C_FG, txt));
    return b;
}

static void build_sheet(lv_obj_t *scr)
{
    v_sheet = lv_obj_create(scr);
    lv_obj_remove_style_all(v_sheet);
    lv_obj_set_size(v_sheet, 240, 240);
    lv_obj_set_style_bg_color(v_sheet, C_BG, 0);
    lv_obj_set_style_bg_opa(v_sheet, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v_sheet, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(v_sheet, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(v_sheet, sheet_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_t *hdr = lv_button_create(v_sheet);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, 240, 40);
    lv_obj_set_style_bg_color(hdr, C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(hdr, sheet_back_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(hdr);
    lv_obj_align(ui_label(hdr, &font_title_20, C_ACC, LV_SYMBOL_LEFT), LV_ALIGN_LEFT_MID, 12, 0);
    v_sheet_title = ui_label(hdr, &font_title_20, C_FG, "");
    lv_obj_align(v_sheet_title, LV_ALIGN_LEFT_MID, 36, 0);
    v_sheet_list = lv_obj_create(v_sheet);
    lv_obj_remove_style_all(v_sheet_list);
    lv_obj_set_size(v_sheet_list, 240, 200);
    lv_obj_set_pos(v_sheet_list, 0, 40);
    lv_obj_set_style_pad_hor(v_sheet_list, 10, 0);
    lv_obj_set_style_pad_bottom(v_sheet_list, 10, 0);
    lv_obj_set_style_pad_row(v_sheet_list, 6, 0);
    lv_obj_set_flex_flow(v_sheet_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v_sheet_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(v_sheet_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v_sheet_list, LV_SCROLLBAR_MODE_ACTIVE);
    ui_bubble(v_sheet_list);
}

static void build_setup(lv_obj_t *scr)
{
    v_setup = lv_obj_create(scr);
    lv_obj_remove_style_all(v_setup);
    lv_obj_set_size(v_setup, 240, 240 - TOP_H);
    lv_obj_set_pos(v_setup, 0, TOP_H);
    lv_obj_set_style_bg_color(v_setup, C_BG, 0);
    lv_obj_set_style_bg_opa(v_setup, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v_setup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v_setup, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    ui_bubble(v_setup);
    lv_obj_t *ic = ui_label(v_setup, &font_title_20, lv_color_hex(0x8B5CF6), SYM_RADIO);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *t = ui_label(v_setup, &font_text_16, C_FG,
                           _("Indiquer les réseaux Wi-Fi et les webradios (adresses des flux), depuis le téléphone."));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, 216);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_t *b = ui_button(v_setup, _("Configurer"), C_ACC, setup_cb, NULL);
    lv_obj_set_size(b, 200, 44);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void create(lv_obj_t *scr)
{
    v_scr = scr;
    if (!m_st) m_st = heap_caps_malloc(RAD_MAX * sizeof *m_st, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m_st) m_nst = rad_cfg_load(m_st);

    lv_obj_t *bk = flat_btn(scr, back_cb);
    lv_obj_set_size(bk, 34, TOP_H);
    lv_obj_center(ui_label(bk, &font_title_20, C_ACC, LV_SYMBOL_LEFT));
    lv_obj_t *hb = flat_btn(scr, stations_cb);
    lv_obj_set_size(hb, 168, TOP_H);
    lv_obj_set_pos(hb, 34, 0);
    v_dot = lv_obj_create(hb);
    lv_obj_remove_style_all(v_dot);
    lv_obj_set_size(v_dot, 8, 8);
    lv_obj_set_style_radius(v_dot, 4, 0);
    lv_obj_set_style_bg_opa(v_dot, LV_OPA_COVER, 0);
    lv_obj_align(v_dot, LV_ALIGN_LEFT_MID, 2, 0);
    v_head = ui_label(hb, &font_text_16, C_DIM, "Radio");
    lv_label_set_long_mode(v_head, LV_LABEL_LONG_DOT);
    lv_obj_set_size(v_head, 150, 20);
    lv_obj_align(v_head, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *sb = flat_btn(scr, sound_cb);
    lv_obj_set_size(sb, 38, TOP_H);
    lv_obj_set_pos(sb, 202, 0);
    v_spk = ui_label(sb, &font_text_16, C_FG, LV_SYMBOL_VOLUME_MID);
    lv_obj_center(v_spk);

    /* station : toucher = liste */
    lv_obj_t *nb = flat_btn(scr, stations_cb);
    lv_obj_set_size(nb, 228, 54);
    lv_obj_set_pos(nb, 6, 38);
    v_name = ui_label(nb, &font_title_20, C_FG, "");
    lv_label_set_long_mode(v_name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(v_name, 220, 50);
    lv_obj_set_style_text_align(v_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(v_name);

    v_title = ui_label(scr, &font_text_16, C_ACC, "");
    lv_label_set_long_mode(v_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(v_title, 224, 42);
    lv_obj_set_style_text_align(v_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(v_title, 8, 96);
    lv_obj_set_style_clip_corner(v_title, true, 0);

    v_status = lv_obj_create(scr);
    lv_obj_remove_style_all(v_status);
    lv_obj_set_size(v_status, 240, 46);
    lv_obj_set_pos(v_status, 0, 94);
    lv_obj_set_flex_flow(v_status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v_status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(v_status, 2, 0);
    lv_obj_remove_flag(v_status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_status, status_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(v_status);
    v_st1 = ui_label(v_status, &font_text_16, C_FG, "");
    v_st2 = ui_label(v_status, &font_small_11, C_DIM, "");
    for (int k = 0; k < 2; k++) {
        lv_obj_t *l = k ? v_st2 : v_st1;
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_size(l, 228, k ? 14 : 20);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        ui_bubble(l);
    }

    v_fmt = ui_label(scr, &font_small_11, C_DIM, "");
    lv_obj_set_width(v_fmt, 228);
    lv_obj_set_style_text_align(v_fmt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(v_fmt, 6, 144);
    v_buf = lv_bar_create(scr);
    lv_obj_set_size(v_buf, 120, 4);
    lv_obj_set_pos(v_buf, 60, 164);
    lv_bar_set_range(v_buf, 0, 100);
    lv_obj_set_style_bg_color(v_buf, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(v_buf, C_DIM, LV_PART_INDICATOR);
    ui_bubble(v_buf);

    v_prev = round_btn(scr, LV_SYMBOL_PREV, prev_cb, C_OFF, -78);
    v_play = round_btn(scr, LV_SYMBOL_PLAY, play_cb, lv_color_hex(0x16A34A), 0);
    v_play_lbl = lv_obj_get_child(v_play, 0);
    v_next = round_btn(scr, LV_SYMBOL_NEXT, next_cb, C_OFF, 78);

    build_setup(scr);
    build_sheet(scr);
    v_tick = lv_timer_create(tick_cb, 250, NULL);
    if (m_want && !radio_running()) start();
    refresh();
}

static void destroy(void)
{
    if (radio_running()) radio_stop();
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    v_scr = v_dot = v_head = v_name = v_title = v_fmt = v_buf = v_status = v_st1 = v_st2 = NULL;
    v_play = v_play_lbl = v_prev = v_next = v_setup = v_spk = NULL;
    v_sheet = v_sheet_title = v_sheet_list = v_vol_lbl = v_tone_lbl = NULL;
    v_sheet_kind = SH_NONE;
}

static void on_show(void)
{
    radio_screen(true);
    if (v_tick) lv_timer_resume(v_tick);
    if (m_want && !radio_running() && v_scr) start();
    refresh();
}

/* écran éteint (ou notification par-dessus) : l'écoute continue en mode
 * économe ; on quitte l'app : arrêt */
static void on_hide(void)
{
    if (v_tick) lv_timer_pause(v_tick);
    if (app_leaving_for_good()) {
        sheet_close();
        if (radio_running()) radio_stop();
    } else {
        radio_screen(false);
    }
}

static bool busy(void) { return radio_running(); }

const app_t app_radio = {
    .name = "Radio",
    .icon = SYM_RADIO,
    .color = 0x8B5CF6,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .busy = busy,
    .background = busy,
};
