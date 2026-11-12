#include "ui.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "src/display/lv_display_private.h"   /* act_scr / prev_scr / scr_to_load */
#include "fonts/fonts.h"
#include "face_classic.h"
#include "face_pub.h"
#include "board.h"
#include "power.h"
#include "ble.h"
#include "ancs.h"
#include "media.h"
#include "ota.h"
#include "sdr_mode.h"
#include "app_events.h"
#include "wclock.h"
#include "settings.h"
#include "display.h"
#include "sound.h"
#include "crashlog.h"
#include "axp202.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_idf_version.h"

#include "ui_kit.h"
#include "i18n.h"
#include "app.h"

#define ANIM_MS UI_ANIM_MS

typedef enum {
    SCR_WATCH, SCR_LIST, SCR_DETAIL, SCR_CALL, SCR_MENU, SCR_POWER, SCR_PAIR, SCR_ABOUT, SCR_MEDIA, SCR_SET_DISP, SCR_SET_ALERT, SCR_SET_BT, SCR_OTA,
    SCR_LAUNCHER, SCR_QUICK, SCR_APP, SCR_COUNT
} scr_id_t;

static lv_obj_t *s_scr[SCR_COUNT];
static scr_id_t s_cur = SCR_WATCH;
/* écran d'où l'on vient (retour par « ‹ » ou →), posé à chaque ouverture */
static scr_id_t s_origin[SCR_COUNT];
static const app_t *s_app_cur;           /* app hébergée sur SCR_APP */
/* lanceur / contrôles rapides */
static lv_obj_t *q_info, *q_bri;

/* cadran */
static lv_obj_t *w_digital, *w_classic, *w_pub, *w_scr, *w_dnd;
enum { FACE_CLASSIC = 0, FACE_DIGITAL = 1, FACE_PUB = 2 };
static lv_obj_t *w_time, *w_date, *w_ble, *w_batt, *w_notif;
/* liste */
static lv_obj_t *l_list;
static bool l_dirty = true;
/* détail */
static lv_obj_t *d_header, *d_app, *d_when, *d_body, *d_title, *d_msg, *d_btns, *d_btn_pos, *d_btn_neg,
                *d_lbl_pos, *d_lbl_neg;
static uint32_t d_hash;
static scr_id_t d_return = SCR_WATCH;
/* appel */
static lv_obj_t *c_who;
static uint32_t c_uid;
static bool c_uid_valid;
/* menu */
static uint32_t s_pending_hash;   /* notif reçue écran éteint, pas encore vue */
static scr_id_t c_return = SCR_WATCH;   /* écran retrouvé après un appel */
/* bannière de notification (couche supérieure) : affichée par-dessus une app
 * ou un menu au lieu de quitter l'écran en cours */
static lv_obj_t *b_card, *b_icon, *b_app, *b_title, *b_msg;
static lv_timer_t *b_timer;
static uint32_t b_hash;
static void banner_hide(bool anim);
static lv_obj_t *m_sw_nwake, *m_sw_tap;
static lv_obj_t *m_slider, *m_sw_vib, *m_sw_bt, *m_bm_tmo, *m_bm_face, *m_bm_lang, *m_toast;
static void menu_lang_cb(lv_event_t *ev);
static lv_timer_t *w_clock_t;
static lv_obj_t *m_sum[6], *m_bt_info, *m_confirm;
static lv_obj_t *o_info, *o_bar, *o_status, *o_btn, *o_pw, *o_prev, *o_prev_modal, *o_title;
static int o_app = -1;           /* ≥ 0 : session « réglages » de cette app (WebRX), sinon mise à jour */
static void ota_ui_idle(void);
static void build_ota(void);
static void back_cb(lv_event_t *ev);
static void menu_bt_refresh(void);
static scr_id_t menu_parent(scr_id_t id);
static void nav_open(scr_id_t id, lv_screen_load_anim_t anim);
static void menu_refresh_summaries(void);
static lv_timer_t *m_toast_t;
/* énergie */
static lv_obj_t *p_lbl[7];
static lv_timer_t *p_timer;
static lv_timer_t *mu_timer;
static void media_refresh(bool full);
/* appairage */
static lv_obj_t *pa_code, *pa_hint;
static lv_timer_t *pa_timer;
/* à propos */
static lv_obj_t *a_lbl;

static void show(scr_id_t id, lv_screen_load_anim_t anim);
static void list_rebuild(void);
static void watch_refresh_status(void);
static void watch_update_time(bool force);

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
static void gesture_cb(lv_event_t *e);

static void bubble(lv_obj_t *o)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

/* Changement d'écran animé en cours : LVGL garde l'ancien écran (prev_scr)
 * et l'anime jusqu'au bout. Le détruire avant la fin (extinction, appui
 * bouton, libération d'un groupe pendant les 200 ms d'animation) laisse
 * LVGL écrire dans de la mémoire libérée → tas corrompu, plantage plus tard
 * dans un realloc. On termine donc net toute animation avant de détruire
 * un écran. */
static void scr_anim_settle(void)
{
    lv_display_t *d = lv_display_get_default();
    if (!d || (!d->prev_scr && !d->scr_to_load)) return;
    lv_obj_t *scr[3] = { d->prev_scr, d->scr_to_load, d->act_scr };
    for (int i = 0; i < 3; i++) {
        if (!scr[i]) continue;
        lv_anim_delete(scr[i], NULL);
        lv_obj_set_pos(scr[i], 0, 0);
        lv_obj_remove_local_style_prop(scr[i], LV_STYLE_OPA, 0);
    }
    if (d->scr_to_load) d->act_scr = d->scr_to_load;   /* animation pas encore démarrée */
    d->prev_scr = NULL;
    d->scr_to_load = NULL;
    d->draw_prev_over_act = false;
    d->del_prev = false;
    if (d->act_scr) lv_obj_invalidate(d->act_scr);
}

static void scr_delete(lv_obj_t *o)
{
    scr_anim_settle();
    lv_obj_delete(o);
}

static lv_obj_t *mk_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, C_BG, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s, C_FG, 0);
    lv_obj_set_style_text_font(s, &font_text_16, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s, gesture_cb, LV_EVENT_GESTURE, NULL);
    return s;
}

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *f, lv_color_t col, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_text(l, txt);
    bubble(l);
    return l;
}

static lv_obj_t *mk_container(lv_obj_t *p)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    bubble(c);
    return c;
}

static lv_obj_t *mk_button(lv_obj_t *p, const char *txt, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    bubble(b);
    lv_obj_t *l = mk_label(b, &font_text_16, C_FG, txt);
    lv_obj_center(l);
    return b;
}

/* « icône  texte traduit » (tampons tournants : le label copie le texte) */
static const char *with_sym(const char *sym, const char *txt)
{
    static char buf[4][96];
    static int k;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof(buf[k]), "%s  %s", sym, txt);
    return buf[k];
}

/* exports ui_kit.h (apps) */
lv_obj_t *ui_screen(void) { return mk_screen(); }
lv_obj_t *ui_label(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *t) { return mk_label(p, f, c, t); }
lv_obj_t *ui_button(lv_obj_t *p, const char *t, lv_color_t bg, lv_event_cb_t cb, void *ud) { return mk_button(p, t, bg, cb, ud); }
lv_obj_t *ui_container(lv_obj_t *p) { return mk_container(p); }
void      ui_bubble(lv_obj_t *o) { bubble(o); }

static const char *cat_icon(uint8_t cat)
{
    switch (cat) {
    case ANCS_CAT_INCOMING_CALL:
    case ANCS_CAT_MISSED_CALL:
    case ANCS_CAT_VOICEMAIL: return LV_SYMBOL_CALL;
    case ANCS_CAT_EMAIL:     return LV_SYMBOL_ENVELOPE;
    case ANCS_CAT_SOCIAL:    return SYM_COMMENTS;
    default:                 return LV_SYMBOL_BELL;
    }
}

static lv_color_t cat_color(uint8_t cat)
{
    switch (cat) {
    case ANCS_CAT_INCOMING_CALL:
    case ANCS_CAT_MISSED_CALL:
    case ANCS_CAT_VOICEMAIL: return lv_color_hex(0x15803D);
    case ANCS_CAT_SOCIAL:    return lv_color_hex(0x1D4ED8);
    case ANCS_CAT_EMAIL:     return lv_color_hex(0xB45309);
    case ANCS_CAT_SCHEDULE:  return lv_color_hex(0xB91C1C);
    case ANCS_CAT_NEWS:      return lv_color_hex(0x6D28D9);
    default:                 return lv_color_hex(0x374151);
    }
}

/* « yyyyMMddTHHmmSS » → « 14:05 » ou « 26/10 14:05 » si pas aujourd'hui */
static void fmt_when(const char *d, char *out, size_t osz)
{
    out[0] = 0;
    if (strlen(d) < 13 || d[8] != 'T') return;
    struct tm now;
    wclock_local_tm(&now);
    char today[40];
    snprintf(today, sizeof(today), "%04d%02d%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    if (strncmp(d, today, 8) == 0) {
        snprintf(out, osz, "%.2s:%.2s", d + 9, d + 11);
    } else {
        snprintf(out, osz, "%.2s/%.2s %.2s:%.2s", d + 6, d + 4, d + 9, d + 11);
    }
}

/* ------------------------------------------------------------------ */
/* Navigation                                                           */
/* ------------------------------------------------------------------ */
static void power_refresh(void);
static void about_refresh(void);

static void build_menu(void);
static void build_media(void);
static void menu_sync_from_settings(void);

/* Écrans construits à la demande et libérés écran éteint (ui_release_idle)
 * pour ménager la RAM. Avant d'en construire un, on libère les autres
 * groupes, sauf celui affiché (il peut être en train de s'animer). */
enum { G_NONE = 0, G_MENU, G_MEDIA, G_LAUNCHER, G_QUICK, G_APP };

static int group_of(scr_id_t id)
{
    switch (id) {
    case SCR_MENU: case SCR_SET_DISP: case SCR_SET_ALERT: case SCR_SET_BT: case SCR_OTA: return G_MENU;
    case SCR_MEDIA:    return G_MEDIA;
    case SCR_LAUNCHER: return G_LAUNCHER;
    case SCR_QUICK:    return G_QUICK;
    case SCR_APP:      return G_APP;
    default:           return G_NONE;
    }
}
static bool is_menu_group(scr_id_t id) { return group_of(id) == G_MENU; }

static void release_group(int g);
static void build_launcher(void);
static void watch_long_cb(lv_event_t *ev);
static void build_quick(void);
static void app_build(const app_t *a);

static void ensure_screen(scr_id_t id)
{
    int g = group_of(id);
    if (g == G_NONE || s_scr[id]) return;
    for (int o = G_MENU; o <= G_APP; o++) {
        if (o != g && o != group_of(s_cur)) release_group(o);
    }
    switch (g) {
    case G_MENU:     build_menu(); menu_sync_from_settings(); break;
    case G_MEDIA:    build_media(); break;
    case G_LAUNCHER: build_launcher(); break;
    case G_QUICK:    build_quick(); break;
    case G_APP:      if (s_app_cur) app_build(s_app_cur); break;
    }
}

static void quick_refresh(void);

static scr_id_t s_next = SCR_WATCH;                /* écran en cours d'ouverture (show) */

static void show(scr_id_t id, lv_screen_load_anim_t anim)
{
    ensure_screen(id);
    if (!s_scr[id]) id = SCR_WATCH;                    /* sécurité : rien à afficher */
    s_next = id;
    if (s_cur == SCR_APP && id != SCR_APP && s_app_cur && s_app_cur->on_hide) s_app_cur->on_hide();
    if (id == SCR_APP && s_cur != SCR_APP && s_app_cur && s_app_cur->on_show) s_app_cur->on_show();
    if (id == SCR_QUICK) quick_refresh();
    if (id == SCR_LIST && l_dirty) list_rebuild();
    if (id == SCR_POWER) { power_refresh(); lv_timer_resume(p_timer); }
    else lv_timer_pause(p_timer);
    if (id == SCR_MEDIA) { media_refresh(true); lv_timer_resume(mu_timer); }
    else if (mu_timer) lv_timer_pause(mu_timer);
    if (id == SCR_ABOUT) about_refresh();
    if (id == SCR_MENU) menu_refresh_summaries();
    if (id == SCR_SET_BT) { menu_bt_refresh(); lv_obj_add_flag(m_confirm, LV_OBJ_FLAG_HIDDEN); }
    if (id == SCR_WATCH) { watch_update_time(true); watch_refresh_status(); }
    s_cur = id;
    crashlog_mark(anim == LV_SCR_LOAD_ANIM_NONE ? 'S' : 'A', s_cur, id);
    scr_anim_settle();                                 /* jamais deux animations empilées */
    if (!display_is_on() || lv_screen_active() == s_scr[id]) anim = LV_SCR_LOAD_ANIM_NONE;
    if (anim == LV_SCR_LOAD_ANIM_NONE) lv_screen_load(s_scr[id]);
    else lv_screen_load_anim(s_scr[id], anim, ANIM_MS, 0, false);
}

static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    bool handled = true;
    switch (s_cur) {
    case SCR_WATCH:            /* ↑ notifications · ↓ contrôles rapides · ← apps */
        if (dir == LV_DIR_TOP) nav_open(SCR_LIST, LV_SCR_LOAD_ANIM_MOVE_TOP);
        else if (dir == LV_DIR_BOTTOM) nav_open(SCR_QUICK, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
        else if (dir == LV_DIR_LEFT) nav_open(SCR_LAUNCHER, LV_SCR_LOAD_ANIM_MOVE_LEFT);
        else handled = false;
        break;
    case SCR_LIST:             /* fermé par le geste inverse ou → */
        if (dir == LV_DIR_BOTTOM) show(SCR_WATCH, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
        else if (dir == LV_DIR_RIGHT) show(SCR_WATCH, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        else handled = false;
        break;
    case SCR_QUICK:
        if (dir == LV_DIR_TOP) show(SCR_WATCH, LV_SCR_LOAD_ANIM_MOVE_TOP);
        else if (dir == LV_DIR_RIGHT) show(SCR_WATCH, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        else handled = false;
        break;
    case SCR_DETAIL:
        if (dir == LV_DIR_RIGHT) show(d_return, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        else handled = false;
        break;
    case SCR_CALL:
        handled = false;       /* l'appel se traite par ses boutons */
        break;
    default:                   /* partout ailleurs : → = retour à l'origine */
        if (dir == LV_DIR_RIGHT) back_cb(NULL);
        else handled = false;
        break;
    }
    if (handled) lv_indev_wait_release(indev);
}

/* ------------------------------------------------------------------ */
/* Cadran                                                               */
/* ------------------------------------------------------------------ */
static void watch_update_time(bool force)
{
    static const char *const JOURS[] = { N_("dim."), N_("lun."), N_("mar."), N_("mer."), N_("jeu."),
                                         N_("ven."), N_("sam.") };
    static const char *const MOIS[] = { N_("janv."), N_("févr."), N_("mars"), N_("avr."), N_("mai"),
                                        N_("juin"), N_("juil."), N_("août"), N_("sept."), N_("oct."),
                                        N_("nov."), N_("déc.") };
    static int last_min = -1, last_day = -1;
    struct tm tm;
    wclock_local_tm(&tm);
    if (force || tm.tm_min != last_min) {
        lv_label_set_text_fmt(w_time, "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_obj_set_style_text_color(w_time, wclock_time_valid() ? C_FG : C_DIM, 0);
        last_min = tm.tm_min;
    }
    if (force || tm.tm_mday != last_day) {
        char b[32];
        snprintf(b, sizeof(b), "%s %d %s", _(JOURS[tm.tm_wday % 7]), tm.tm_mday, _(MOIS[tm.tm_mon % 12]));
        lv_label_set_text(w_date, b);
        last_day = tm.tm_mday;
    }
    if (w_classic) face_classic_set_time(&tm, force);
    if (w_pub) face_pub_set_time(&tm, force);
}

/* Un seul cadran analogique existe à la fois (mémoire LVGL) : créé à la
 * demande, l'autre est détruit. Le numérique, léger, reste en place. */
static void apply_face(void)
{
    uint8_t f = settings_get()->watchface;
    if (f != FACE_CLASSIC && w_classic) { lv_obj_delete(w_classic); w_classic = NULL; }
    if (f != FACE_PUB && w_pub)         { lv_obj_delete(w_pub); w_pub = NULL; }
    if (f == FACE_CLASSIC && !w_classic) w_classic = face_classic_create(w_scr);
    if (f == FACE_PUB && !w_pub)         w_pub = face_pub_create(w_scr);
    if (f == FACE_DIGITAL) lv_obj_remove_flag(w_digital, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(w_digital, LV_OBJ_FLAG_HIDDEN);
    /* lune « ne pas déranger » des cadrans analogiques, sous le guichet date */
    if (!w_dnd) {
        w_dnd = mk_label(w_scr, &font_text_16, C_DIM, SYM_MOON);
        lv_obj_set_pos(w_dnd, 170, 136);
    }
    lv_obj_set_style_text_color(w_dnd, lv_color_hex(f == FACE_PUB ? 0xC9A24A : 0x23407E), 0);
    lv_obj_move_foreground(w_dnd);
    if (f != FACE_DIGITAL && settings_get()->dnd) lv_obj_remove_flag(w_dnd, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(w_dnd, LV_OBJ_FLAG_HIDDEN);
}

static void watch_refresh_status(void)
{
    ble_info_t bi;
    ble_get_info(&bi);
    lv_color_t bc = C_OFF;
    switch (bi.state) {
    case BLE_ST_READY:       bc = C_ACC; break;
    case BLE_ST_CONNECTED:
    case BLE_ST_SECURED:     bc = C_WARN; break;
    case BLE_ST_ADVERTISING: bc = C_DIM; break;
    default:                 bc = C_OFF; break;
    }
    lv_obj_set_style_text_color(w_ble, bc, 0);

    int p = power_percent();
    const char *sym = p > 80 ? LV_SYMBOL_BATTERY_FULL : p > 55 ? LV_SYMBOL_BATTERY_3 :
                      p > 30 ? LV_SYMBOL_BATTERY_2 : p > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char b[32];
    if (power_charging()) snprintf(b, sizeof(b), LV_SYMBOL_CHARGE " %d%% %s", p, sym);
    else snprintf(b, sizeof(b), "%d%% %s", p, sym);
    lv_label_set_text(w_batt, b);
    lv_obj_set_style_text_color(w_batt, (p <= 15 && !power_vbus()) ? C_BAD : C_FG, 0);

    int n = ns_count();
    const char *dnd = settings_get()->dnd ? SYM_MOON " " : "";
    if (w_dnd) {
        if (settings_get()->dnd && settings_get()->watchface != FACE_DIGITAL) lv_obj_remove_flag(w_dnd, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(w_dnd, LV_OBJ_FLAG_HIDDEN);
    }
    if (n > 0) lv_label_set_text_fmt(w_notif, "%s" LV_SYMBOL_BELL " %d", dnd, n);
    else lv_label_set_text(w_notif, settings_get()->dnd ? SYM_MOON : "");

    if (w_classic) face_classic_set_status(p, power_charging(), bi.state == BLE_ST_READY, bi.state != BLE_ST_OFF, n);
    if (w_pub) {
        int link = bi.state == BLE_ST_READY ? PUB_LINK_READY :
                   (bi.state == BLE_ST_CONNECTED || bi.state == BLE_ST_SECURED) ? PUB_LINK_WAIT :
                   bi.state == BLE_ST_ADVERTISING ? PUB_LINK_ADV : PUB_LINK_OFF;
        face_pub_set_status(p, power_charging(), link, n);
    }
}

static void clock_timer_cb(lv_timer_t *t)
{
    if (s_cur == SCR_WATCH) watch_update_time(false);
}

static void build_watch(void)
{
    lv_obj_t *s = s_scr[SCR_WATCH] = mk_screen();

    /* cadran numérique */
    w_digital = mk_container(s);
    lv_obj_set_size(w_digital, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_remove_flag(w_digital, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *d = w_digital;
    w_ble = mk_label(d, &font_title_20, C_OFF, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(w_ble, LV_ALIGN_TOP_LEFT, 14, 10);
    w_notif = mk_label(d, &font_text_16, C_FG, "");
    lv_obj_align(w_notif, LV_ALIGN_TOP_MID, 0, 12);
    w_batt = mk_label(d, &font_text_16, C_FG, "");
    lv_obj_align(w_batt, LV_ALIGN_TOP_RIGHT, -12, 12);
    w_time = mk_label(d, &font_time_72, C_FG, "--:--");
    lv_obj_align(w_time, LV_ALIGN_CENTER, 0, -6);
    w_date = mk_label(d, &font_title_20, C_DIM, "");
    lv_obj_align(w_date, LV_ALIGN_CENTER, 0, 52);
    lv_obj_t *hint = mk_label(d, &font_text_16, C_OFF, LV_SYMBOL_UP);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* cadrans analogiques : créés par apply_face() selon le réglage */
    w_scr = s;
    lv_obj_add_event_cb(s, watch_long_cb, LV_EVENT_LONG_PRESSED, NULL);
    apply_face();
    w_clock_t = lv_timer_create(clock_timer_cb, 1000, NULL);
}

/* ------------------------------------------------------------------ */
/* Détail d'une notification                                            */
/* ------------------------------------------------------------------ */
static void detail_fill(const notif_entry_t *e)
{
    d_hash = e->hash;
    if (e->hash == s_pending_hash) s_pending_hash = 0;
    lv_obj_set_style_bg_color(d_header, cat_color(e->n.category), 0);
    char app[48];
    snprintf(app, sizeof(app), "%s %s", cat_icon(e->n.category), e->n.app_name);
    lv_label_set_text(d_app, app);
    char when[16];
    fmt_when(e->n.date, when, sizeof(when));
    lv_label_set_text(d_when, when);

    if (e->n.title[0]) {
        lv_label_set_text(d_title, e->n.title);
        lv_obj_remove_flag(d_title, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d_title, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(d_msg, e->n.message[0] ? e->n.message : "");

    bool pos = e->uid_valid && (e->n.flags & ANCS_FLAG_POSITIVE);
    bool neg_ios = e->uid_valid && (e->n.flags & ANCS_FLAG_NEGATIVE);
    if (pos) {
        lv_label_set_text(d_lbl_pos, e->n.pos_label[0] ? e->n.pos_label : "OK");
        lv_obj_remove_flag(d_btn_pos, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d_btn_pos, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(d_lbl_neg, (neg_ios && e->n.neg_label[0]) ? e->n.neg_label : _("Effacer"));
    lv_obj_set_user_data(d_btn_neg, (void *)(uintptr_t)(neg_ios ? 1 : 2));
    lv_obj_scroll_to_y(d_body, 0, LV_ANIM_OFF);
}

static void detail_btn_cb(lv_event_t *ev)
{
    lv_obj_t *btn = lv_event_get_target_obj(ev);
    int act = (int)(uintptr_t)lv_obj_get_user_data(btn);   /* 0 positive, 1 négative iOS, 2 local */
    notif_entry_t *e = ns_find_hash(d_hash);
    if (e) {
        if (act < 2 && e->uid_valid) ancs_request_action(e->n.uid, (uint8_t)act);
        if (act != 0) {
            ns_remove_entry(e);
            l_dirty = true;
        }
    }
    show(d_return, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

static void build_detail(void)
{
    lv_obj_t *s = s_scr[SCR_DETAIL] = mk_screen();

    d_header = mk_container(s);
    lv_obj_set_size(d_header, BOARD_LCD_W, 34);
    lv_obj_set_style_bg_opa(d_header, LV_OPA_COVER, 0);
    lv_obj_remove_flag(d_header, LV_OBJ_FLAG_SCROLLABLE);
    d_app = mk_label(d_header, &font_text_16, C_FG, "");
    lv_label_set_long_mode(d_app, LV_LABEL_LONG_DOT);
    lv_obj_set_width(d_app, 160);
    lv_obj_align(d_app, LV_ALIGN_LEFT_MID, 10, 0);
    d_when = mk_label(d_header, &font_text_16, C_FG, "");
    lv_obj_align(d_when, LV_ALIGN_RIGHT_MID, -10, 0);

    d_body = mk_container(s);
    lv_obj_set_size(d_body, BOARD_LCD_W, BOARD_LCD_H - 34 - 54);
    lv_obj_set_pos(d_body, 0, 34);
    lv_obj_set_style_pad_hor(d_body, 12, 0);
    lv_obj_set_style_pad_ver(d_body, 8, 0);
    lv_obj_set_style_pad_row(d_body, 6, 0);
    lv_obj_set_flex_flow(d_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(d_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(d_body, LV_SCROLLBAR_MODE_AUTO);

    d_title = mk_label(d_body, &font_title_20, C_FG, "");
    lv_label_set_long_mode(d_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d_title, lv_pct(100));
    d_msg = mk_label(d_body, &font_text_16, lv_color_hex(0xD0D3D8), "");
    lv_label_set_long_mode(d_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d_msg, lv_pct(100));

    d_btns = mk_container(s);
    lv_obj_set_size(d_btns, BOARD_LCD_W, 54);
    lv_obj_align(d_btns, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(d_btns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(d_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(d_btns, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    d_btn_pos = mk_button(d_btns, "", lv_color_hex(0x15803D), detail_btn_cb, NULL);
    lv_obj_set_size(d_btn_pos, 104, 42);
    lv_obj_set_user_data(d_btn_pos, (void *)(uintptr_t)0);
    d_lbl_pos = lv_obj_get_child(d_btn_pos, 0);
    lv_label_set_long_mode(d_lbl_pos, LV_LABEL_LONG_DOT);
    lv_obj_set_width(d_lbl_pos, 96);
    lv_obj_set_style_text_align(d_lbl_pos, LV_TEXT_ALIGN_CENTER, 0);

    d_btn_neg = mk_button(d_btns, "", lv_color_hex(0x3F3F46), detail_btn_cb, NULL);
    lv_obj_set_size(d_btn_neg, 104, 42);
    d_lbl_neg = lv_obj_get_child(d_btn_neg, 0);
    lv_label_set_long_mode(d_lbl_neg, LV_LABEL_LONG_DOT);
    lv_obj_set_width(d_lbl_neg, 96);
    lv_obj_set_style_text_align(d_lbl_neg, LV_TEXT_ALIGN_CENTER, 0);
}

/* ------------------------------------------------------------------ */
/* Appel entrant                                                        */
/* ------------------------------------------------------------------ */
static void call_btn_cb(lv_event_t *ev)
{
    int act = (int)(uintptr_t)lv_event_get_user_data(ev);   /* 0 décrocher, 1 refuser */
    if (c_uid_valid) ancs_request_action(c_uid, (uint8_t)act);
    show(c_return, LV_SCR_LOAD_ANIM_FADE_OUT);
}

static void build_call(void)
{
    lv_obj_t *s = s_scr[SCR_CALL] = mk_screen();
    lv_obj_t *t = mk_label(s, &font_text_16, C_DIM, with_sym(LV_SYMBOL_CALL, _("Appel entrant")));
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    c_who = mk_label(s, &font_title_20, C_FG, "");
    lv_label_set_long_mode(c_who, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c_who, 210);
    lv_obj_set_style_text_align(c_who, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(c_who, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t *ok = mk_button(s, LV_SYMBOL_CALL, C_OK, call_btn_cb, (void *)(uintptr_t)0);
    lv_obj_set_size(ok, 76, 76);
    lv_obj_set_style_radius(ok, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_LEFT, 28, -18);
    lv_obj_t *no = mk_button(s, LV_SYMBOL_CLOSE, C_BAD, call_btn_cb, (void *)(uintptr_t)1);
    lv_obj_set_size(no, 76, 76);
    lv_obj_set_style_radius(no, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(no, LV_ALIGN_BOTTOM_RIGHT, -28, -18);
}

/* ------------------------------------------------------------------ */
/* Liste                                                                */
/* ------------------------------------------------------------------ */
static void list_item_cb(lv_event_t *ev)
{
    uint32_t h = (uint32_t)(uintptr_t)lv_event_get_user_data(ev);
    notif_entry_t *e = ns_find_hash(h);
    if (!e) { list_rebuild(); return; }
    detail_fill(e);
    d_return = SCR_LIST;
    show(SCR_DETAIL, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void list_clear_cb(lv_event_t *ev)
{
    ns_clear();
    l_dirty = true;
    watch_refresh_status();
    show(SCR_WATCH, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
}

static void list_rebuild(void)
{
    lv_obj_clean(l_list);
    int n = ns_count();
    if (n == 0) {
        lv_obj_t *t = lv_list_add_text(l_list, _("Aucune notification"));
        lv_obj_set_style_text_color(t, C_DIM, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
        bubble(t);
    }
    for (int i = 0; i < n; i++) {
        notif_entry_t *e = ns_get(i);
        char b[ANCS_APPNAME_MAX + 72];
        snprintf(b, sizeof(b), "%.32s\n%.64s", e->n.app_name, e->n.title[0] ? e->n.title : e->n.message);
        lv_obj_t *btn = lv_list_add_button(l_list, cat_icon(e->n.category), b);
        lv_obj_set_style_bg_color(btn, C_CARD, 0);
        lv_obj_set_style_text_color(btn, C_FG, 0);
        lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)e->hash);
        bubble(btn);
        lv_obj_t *lbl = lv_obj_get_child(btn, -1);
        if (lbl) {
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_height(lbl, 2 * lv_font_get_line_height(&font_text_16));
            bubble(lbl);
        }
    }
    if (n > 0) {
        lv_obj_t *clr = lv_list_add_button(l_list, LV_SYMBOL_TRASH, _("Tout effacer"));
        lv_obj_set_style_bg_color(clr, lv_color_hex(0x3F1D1D), 0);
        lv_obj_set_style_text_color(clr, C_FG, 0);
        lv_obj_add_event_cb(clr, list_clear_cb, LV_EVENT_CLICKED, NULL);
        bubble(clr);
    }
    l_dirty = false;
}

static void build_list(void)
{
    lv_obj_t *s = s_scr[SCR_LIST] = mk_screen();
    lv_obj_t *h = mk_label(s, &font_title_20, C_FG, _("Notifications"));
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 8);
    l_list = lv_list_create(s);
    lv_obj_set_size(l_list, BOARD_LCD_W, BOARD_LCD_H - 38);
    lv_obj_align(l_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(l_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(l_list, 0, 0);
    lv_obj_set_style_pad_row(l_list, 4, 0);
    lv_obj_set_style_pad_hor(l_list, 6, 0);
    lv_obj_set_scroll_dir(l_list, LV_DIR_VER);
    bubble(l_list);
}

/* ------------------------------------------------------------------ */
/* Menu / réglages                                                      */
/* ------------------------------------------------------------------ */
static void toast_hide_cb(lv_timer_t *t) { lv_obj_add_flag(m_toast, LV_OBJ_FLAG_HIDDEN); }

static void toast(const char *txt)
{
    lv_label_set_text(m_toast, txt);
    lv_obj_remove_flag(m_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(m_toast);
    lv_timer_reset(m_toast_t);
    lv_timer_resume(m_toast_t);
}

void ui_toast(const char *txt) { toast(txt); }

static const uint8_t TMO_VALUES[] = { 5, 8, 10, 15, 30 };

static void menu_slider_cb(lv_event_t *ev)
{
    int v = lv_slider_get_value(m_slider);
    settings_get()->brightness = (uint8_t)v;
    display_set_brightness((uint8_t)v);
    if (lv_event_get_code(ev) == LV_EVENT_RELEASED) settings_save();
}

static void menu_vib_cb(lv_event_t *ev)
{
    settings_get()->vibrate = lv_obj_has_state(m_sw_vib, LV_STATE_CHECKED);
    settings_save();
    if (settings_get()->vibrate) power_vibrate(VIB_SHORT);
}

static void menu_nwake_cb(lv_event_t *ev)
{
    settings_get()->notif_wake = lv_obj_has_state(m_sw_nwake, LV_STATE_CHECKED);
    settings_save();
}

static void sound_test_done(lv_timer_t *t)
{
    int e = sound_last_error();
    if (e) {
        char b[48];
        snprintf(b, sizeof(b), _("Erreur audio 0x%x"), e);
        toast(b);
    }
}

static void sound_test_cb(lv_event_t *ev)
{
    sound_beeps(3, 2700, 120, 80);
    lv_timer_t *t = lv_timer_create(sound_test_done, 900, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void menu_tap_cb(lv_event_t *ev)
{
    bool on = lv_obj_has_state(m_sw_tap, LV_STATE_CHECKED);
    settings_get()->tap_wake = on;
    settings_save();
    display_tap_enable(on);
}

static void menu_bt_cb(lv_event_t *ev)
{
    bool on = lv_obj_has_state(m_sw_bt, LV_STATE_CHECKED);
    settings_get()->ble_enabled = on;
    settings_save();
    if (on) { ble_start(); toast(_("Bluetooth activé")); }
    else { ble_stop(); toast(_("Mode avion")); }
}

/* Faces dans l'ordre du sélecteur → valeur settings.watchface */
static const uint8_t FACE_ORDER[3] = { FACE_CLASSIC, FACE_PUB, FACE_DIGITAL };
static const char *const FACE_NAMES[3] = { N_("Rétro"), N_("Pub"), N_("Digital") };

static void menu_tmo_cb(lv_event_t *ev)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(m_bm_tmo);
    if (i < sizeof(TMO_VALUES)) {
        settings_get()->screen_timeout_s = TMO_VALUES[i];
        settings_save();
    }
}

static void menu_face_cb(lv_event_t *ev)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(m_bm_face);
    if (i >= 3) return;
    settings_get()->watchface = FACE_ORDER[i];
    settings_save();
    apply_face();
    watch_update_time(true);
    watch_refresh_status();
}

/* Changement de langue : tous les écrans sont reconstruits dans la nouvelle
 * langue (pas de redémarrage), puis on revient sur la page Affichage.
 * Différé : on ne détruit pas le bouton dont on traite l'événement. */
static void build_watch(void);
static void build_list(void);
static void build_detail(void);
static void build_call(void);
static void build_power(void);
static void build_pair(void);
static void build_about(void);
static void release_group(int g);

static void relang_async(void *arg)
{
    crashlog_mark('L', s_cur, 0);
    scr_id_t back = s_cur;
    lv_obj_t *tmp = lv_obj_create(NULL);
    scr_anim_settle();
    lv_screen_load(tmp);
    s_cur = SCR_WATCH;                       /* plus aucun groupe « affiché » */
    for (int g = G_MENU; g <= G_APP; g++) release_group(g);

    if (w_clock_t) { lv_timer_delete(w_clock_t); w_clock_t = NULL; }
    if (p_timer) { lv_timer_delete(p_timer); p_timer = NULL; }
    if (pa_timer) { lv_timer_delete(pa_timer); pa_timer = NULL; }
    static const scr_id_t perm[] = { SCR_WATCH, SCR_LIST, SCR_DETAIL, SCR_CALL, SCR_POWER, SCR_PAIR, SCR_ABOUT };
    for (size_t i = 0; i < sizeof(perm) / sizeof(perm[0]); i++) {
        if (s_scr[perm[i]]) { scr_delete(s_scr[perm[i]]); s_scr[perm[i]] = NULL; }
    }
    w_classic = w_pub = w_dnd = NULL;
    l_dirty = true;

    build_watch();
    build_list();
    build_detail();
    build_call();
    build_power();
    build_pair();
    build_about();
    show(back, LV_SCR_LOAD_ANIM_NONE);       /* reconstruit le groupe réglages */
    scr_delete(tmp);
}

static void menu_lang_cb(lv_event_t *ev)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(m_bm_lang);
    if (i > 1 || i == settings_get()->lang) return;
    settings_get()->lang = (uint8_t)i;
    settings_save();
    i18n_set((int)i);
    lv_async_call(relang_async, NULL);
}

/* ouvre un écran en mémorisant d'où l'on vient (pour « ‹ » et →) */
static void nav_open(scr_id_t id, lv_screen_load_anim_t anim)
{
    if (id != s_cur) s_origin[id] = s_cur;
    show(id, anim);
}

static void menu_nav_cb(lv_event_t *ev)
{
    scr_id_t id = (scr_id_t)(uintptr_t)lv_event_get_user_data(ev);
    if (id == SCR_PAIR) {
        if (!ble_running()) { toast(_("Activez d'abord le Bluetooth")); return; }
        lv_label_set_text(pa_code, "------");
        lv_obj_set_style_text_color(pa_code, C_DIM, 0);
        lv_label_set_text(pa_hint, _("iPhone : Réglages > Bluetooth\n> T-Watch, puis « Jumeler »"));
        ble_pairing_mode();
    }
    nav_open(id, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

/* écran de retour : celui d'où l'on est venu */
static scr_id_t menu_parent(scr_id_t id)
{
    scr_id_t o = s_origin[id];
    return (o == id || o >= SCR_COUNT || o == SCR_CALL) ? SCR_WATCH : o;
}

static void back_cb(lv_event_t *ev)
{
    if (s_cur == SCR_OTA && ota_active()) {
        toast(o_app >= 0 ? _("Annuler d'abord les réglages") : _("Annuler d'abord la mise à jour"));
        return;
    }
    if (s_cur == SCR_OTA && o_app >= 0) {            /* réglages non lancés : retour à l'app */
        int a = o_app;
        o_app = -1;
        ota_set_page(OTA_PAGE_UPDATE);
        if (a < g_app_count) { app_open(g_apps[a]); return; }
    }
    show(menu_parent(s_cur), LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

/* Confirmation « oublier les appareils » (boîte modale) */
static void confirm_cb(lv_event_t *ev)
{
    bool ok = (uintptr_t)lv_event_get_user_data(ev) != 0;
    lv_obj_add_flag(m_confirm, LV_OBJ_FLAG_HIDDEN);
    if (!ok) return;
    ble_forget_all();
    ns_invalidate_uids();
    power_vibrate(VIB_SHORT);
    toast(_("Appareils oubliés"));
    menu_bt_refresh();
}

static void forget_cb(lv_event_t *ev)
{
    lv_obj_remove_flag(m_confirm, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(m_confirm);
}

static void style_switch(lv_obj_t *sw)
{
    lv_obj_set_style_bg_color(sw, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, C_ACC, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, C_FG, LV_PART_KNOB);
}

/* Page de réglages : en-tête « ‹ Titre » cliquable + colonne défilante */
int ui_header(lv_obj_t *s, const char *title)
{
    lv_obj_t *hdr = lv_button_create(s);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, BOARD_LCD_W, 40);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_add_event_cb(hdr, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(hdr, C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_STATE_PRESSED);
    bubble(hdr);
    lv_obj_t *ar = mk_label(hdr, &font_title_20, C_ACC, LV_SYMBOL_LEFT);
    lv_obj_align(ar, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *t = mk_label(hdr, &font_title_20, C_FG, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 36, 0);
    return 40;
}

static lv_obj_t *mk_page(scr_id_t id, const char *title)
{
    lv_obj_t *s = s_scr[id] = mk_screen();
    ui_header(s, title);
    lv_obj_t *col = mk_container(s);
    lv_obj_set_size(col, BOARD_LCD_W, BOARD_LCD_H - 40);
    lv_obj_set_pos(col, 0, 40);
    lv_obj_set_style_pad_hor(col, 12, 0);
    lv_obj_set_style_pad_top(col, 2, 0);
    lv_obj_set_style_pad_bottom(col, 12, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_ACTIVE);
    return col;
}

/* Entrée de l'accueil des réglages : icône, titre, résumé, chevron */
static lv_obj_t *mk_entry(lv_obj_t *col, const char *icon, const char *title, scr_id_t target,
                          lv_obj_t **sum_out)
{
    lv_obj_t *b = lv_button_create(col);
    lv_obj_set_size(b, lv_pct(100), 50);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_set_style_bg_color(b, C_OFF, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, menu_nav_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)target);
    bubble(b);
    lv_obj_t *ic = mk_label(b, &font_title_20, C_ACC, icon);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *t = mk_label(b, &font_text_16, C_FG, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 44, 7);
    lv_obj_t *sm = mk_label(b, &font_small_11, C_DIM, "");
    lv_label_set_long_mode(sm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sm, 140);
    lv_obj_align(sm, LV_ALIGN_TOP_LEFT, 44, 28);
    lv_obj_t *ch = mk_label(b, &font_text_16, C_OFF, LV_SYMBOL_RIGHT);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -10, 0);
    if (sum_out) *sum_out = sm;
    return b;
}

/* Ligne interrupteur : titre + explication courte, interrupteur à droite */
static lv_obj_t *mk_switch_row(lv_obj_t *col, const char *title, const char *desc, lv_event_cb_t cb)
{
    lv_obj_t *r = mk_container(col);
    lv_obj_set_size(r, lv_pct(100), 46);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = mk_label(r, &font_text_16, C_FG, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 4);
    lv_obj_t *d = mk_label(r, &font_small_11, C_DIM, desc);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_t *sw = lv_switch_create(r);
    style_switch(sw);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_ext_click_area(sw, 8);
    bubble(sw);
    return sw;
}

static lv_obj_t *mk_section(lv_obj_t *col, const char *txt)
{
    lv_obj_t *l = mk_label(col, &font_small_11, C_DIM, txt);
    lv_obj_set_style_pad_top(l, 6, 0);
    return l;
}

/* Sélecteur segmenté (remplace les listes déroulantes, peu pratiques au doigt) */
static lv_obj_t *mk_segmented(lv_obj_t *col, const char *const *map, lv_event_cb_t cb)
{
    lv_obj_t *m = lv_buttonmatrix_create(col);
    lv_buttonmatrix_set_map(m, map);
    lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(m, true);
    lv_obj_set_size(m, lv_pct(100), 38);
    lv_obj_set_style_pad_all(m, 3, 0);
    lv_obj_set_style_pad_gap(m, 3, 0);
    lv_obj_set_style_bg_color(m, C_CARD, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_radius(m, 10, 0);
    lv_obj_set_style_bg_color(m, C_CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, C_ACC, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, C_FG, LV_PART_ITEMS);
    lv_obj_set_style_text_font(m, &font_text_16, LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 8, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, NULL);
    bubble(m);
    return m;
}

lv_obj_t *ui_segmented(lv_obj_t *p, const char *const *map, lv_event_cb_t cb) { return mk_segmented(p, map, cb); }

static void menu_bt_refresh(void)
{
    ble_info_t bi;
    ble_get_info(&bi);
    const char *st;
    if (!settings_get()->ble_enabled || bi.state == BLE_ST_OFF) st = _("Coupé (mode avion)");
    else if (bi.state == BLE_ST_READY) st = _("Connecté à l'iPhone");
    else if (bi.state == BLE_ST_ADVERTISING) st = _("En attente de l'iPhone");
    else st = _("Connexion en cours…");
    char n[48], b[96];
    if (bi.bonds == 0) snprintf(n, sizeof(n), "%s", _("Aucun appareil appairé"));
    else if (bi.bonds == 1) snprintf(n, sizeof(n), "%s", _("1 appareil appairé"));
    else snprintf(n, sizeof(n), _("%d appareils appairés"), bi.bonds);
    snprintf(b, sizeof(b), "%s\n%s", st, n);
    lv_label_set_text(m_bt_info, b);
}

/* résumés de l'accueil des réglages */
static void menu_refresh_summaries(void)
{
    settings_t *st = settings_get();
    char b[64];
    const char *face = FACE_NAMES[0];
    for (int i = 0; i < 3; i++) if (FACE_ORDER[i] == st->watchface) face = FACE_NAMES[i];
    snprintf(b, sizeof(b), _("Cadran %s · veille %u s"), _(face), st->screen_timeout_s);
    lv_label_set_text(m_sum[0], b);

    snprintf(b, sizeof(b), "%s%s%s", st->vibrate ? _("Vibreur") : _("Sans vibreur"),
             st->notif_wake ? "" : _(" · discret"), st->tap_wake ? _(" · double tap") : "");
    lv_label_set_text(m_sum[1], b);

    ble_info_t bi;
    ble_get_info(&bi);
    lv_label_set_text(m_sum[2], !st->ble_enabled ? _("Mode avion") :
                      bi.state == BLE_ST_READY ? _("Connecté") :
                      bi.state == BLE_ST_ADVERTISING ? _("En attente de l'iPhone") : _("Connexion…"));

    power_telemetry_t t;
    power_get_telemetry(&t);
    if (t.avg_ma > 0.1f) snprintf(b, sizeof(b), _("%d %% · moyenne %.1f mA"), t.percent, t.avg_ma);
    else snprintf(b, sizeof(b), "%d %%%s", t.percent, t.charging ? _(" · en charge") : "");
    lv_label_set_text(m_sum[3], b);

    snprintf(b, sizeof(b), _("Firmware %s"), esp_app_get_description()->version);
    lv_label_set_text(m_sum[4], b);
    lv_label_set_text(m_sum[5], _("Par Wi-Fi, sans câble"));
}

static void build_toast(void)
{
    /* toast sur la couche supérieure : visible quel que soit l'écran */
    m_toast = mk_label(lv_layer_top(), &font_text_16, C_FG, "");
    lv_obj_set_style_bg_color(m_toast, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_bg_opa(m_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(m_toast, 8, 0);
    lv_obj_set_style_radius(m_toast, 8, 0);
    lv_obj_set_style_max_width(m_toast, BOARD_LCD_W - 24, 0);   /* texte long : retour à la ligne */
    lv_label_set_long_mode(m_toast, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(m_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(m_toast, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(m_toast, LV_OBJ_FLAG_HIDDEN);
    m_toast_t = lv_timer_create(toast_hide_cb, 1800, NULL);
    lv_timer_set_repeat_count(m_toast_t, -1);
    lv_timer_pause(m_toast_t);
}

#define BANNER_H   68
#define BANNER_MS  5000

static void banner_anim_y(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void banner_hidden_cb(lv_anim_t *a) { lv_obj_add_flag(b_card, LV_OBJ_FLAG_HIDDEN); }

static void banner_hide(bool anim)
{
    if (!b_card || lv_obj_has_flag(b_card, LV_OBJ_FLAG_HIDDEN)) return;
    lv_timer_pause(b_timer);
    lv_anim_delete(b_card, banner_anim_y);
    if (!anim || !display_is_on()) {
        lv_obj_add_flag(b_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, b_card);
    lv_anim_set_exec_cb(&a, banner_anim_y);
    lv_anim_set_values(&a, lv_obj_get_style_translate_y(b_card, 0), -(BANNER_H + 12));
    lv_anim_set_duration(&a, 160);
    lv_anim_set_completed_cb(&a, banner_hidden_cb);
    lv_anim_start(&a);
}

static void banner_timer_cb(lv_timer_t *t) { banner_hide(true); }

static void banner_click_cb(lv_event_t *ev)
{
    notif_entry_t *e = ns_find_hash(b_hash);
    banner_hide(false);
    if (!e) return;
    /* détail, puis retour exactement là où l'on était (app, menu…) */
    if (s_cur != SCR_DETAIL && s_cur != SCR_CALL) d_return = s_cur;
    detail_fill(e);
    show(SCR_DETAIL, LV_SCR_LOAD_ANIM_OVER_TOP);
}

static void banner_gesture_cb(lv_event_t *ev)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_dir_t d = lv_indev_get_gesture_dir(in);
    if (d == LV_DIR_TOP || d == LV_DIR_LEFT || d == LV_DIR_RIGHT) {
        banner_hide(true);
        lv_indev_wait_release(in);
    }
}

static void banner_show(const notif_entry_t *e)
{
    b_hash = e->hash;
    lv_label_set_text(b_icon, cat_icon(e->n.category));
    lv_obj_set_style_bg_color(b_icon, cat_color(e->n.category), 0);
    char when[16];
    fmt_when(e->n.date, when, sizeof(when));
    char app[ANCS_APPNAME_MAX + 24];
    snprintf(app, sizeof(app), "%.32s · %s", e->n.app_name, when);
    lv_label_set_text(b_app, app);
    lv_label_set_text(b_title, e->n.title[0] ? e->n.title : e->n.app_name);
    lv_label_set_text(b_msg, e->n.message);
    lv_obj_move_foreground(b_card);
    bool was_hidden = lv_obj_has_flag(b_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(b_card, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(b_card, banner_anim_y);
    if (was_hidden && display_is_on()) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, b_card);
        lv_anim_set_exec_cb(&a, banner_anim_y);
        lv_anim_set_values(&a, -(BANNER_H + 12), 0);
        lv_anim_set_duration(&a, 200);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        lv_obj_set_style_translate_y(b_card, 0, 0);
    }
    lv_timer_set_period(b_timer, display_is_on() ? BANNER_MS : BANNER_MS + 2000);
    lv_timer_reset(b_timer);
    lv_timer_resume(b_timer);
}

static void build_banner(void)
{
    b_card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(b_card, BOARD_LCD_W - 12, BANNER_H);
    lv_obj_align(b_card, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(b_card, lv_color_hex(0x22252C), 0);
    lv_obj_set_style_bg_opa(b_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b_card, 18, 0);
    lv_obj_set_style_border_width(b_card, 1, 0);
    lv_obj_set_style_border_color(b_card, lv_color_hex(0x3A3F4A), 0);
    lv_obj_set_style_shadow_width(b_card, 18, 0);
    lv_obj_set_style_shadow_color(b_card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(b_card, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(b_card, 0, 0);
    lv_obj_remove_flag(b_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(b_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(b_card, banner_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(b_card, banner_gesture_cb, LV_EVENT_GESTURE, NULL);

    b_icon = lv_label_create(b_card);
    lv_obj_set_size(b_icon, 36, 36);
    lv_obj_set_style_radius(b_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(b_icon, &font_text_16, 0);
    lv_obj_set_style_text_color(b_icon, C_FG, 0);
    lv_obj_set_style_text_align(b_icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(b_icon, 9, 0);
    lv_obj_align(b_icon, LV_ALIGN_LEFT_MID, 10, 0);

    b_app = lv_label_create(b_card);
    lv_obj_set_style_text_font(b_app, &font_small_11, 0);
    lv_obj_set_style_text_color(b_app, C_DIM, 0);
    lv_label_set_long_mode(b_app, LV_LABEL_LONG_DOT);
    lv_obj_set_width(b_app, 166);
    lv_obj_set_pos(b_app, 56, 7);

    b_title = lv_label_create(b_card);
    lv_obj_set_style_text_font(b_title, &font_text_16, 0);
    lv_obj_set_style_text_color(b_title, C_FG, 0);
    lv_label_set_long_mode(b_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(b_title, 166);
    lv_obj_set_pos(b_title, 56, 22);

    b_msg = lv_label_create(b_card);
    lv_obj_set_style_text_font(b_msg, &font_small_11, 0);
    lv_obj_set_style_text_color(b_msg, lv_color_hex(0xC4C8D0), 0);
    lv_label_set_long_mode(b_msg, LV_LABEL_LONG_DOT);
    lv_obj_set_width(b_msg, 166);
    lv_obj_set_pos(b_msg, 56, 44);

    b_timer = lv_timer_create(banner_timer_cb, BANNER_MS, NULL);
    lv_timer_set_repeat_count(b_timer, -1);
    lv_timer_pause(b_timer);
}

static void build_menu(void)
{

    /* ---- accueil ---- */
    lv_obj_t *col = mk_page(SCR_MENU, _("Réglages"));
    mk_entry(col, SYM_EYE, _("Affichage"), SCR_SET_DISP, &m_sum[0]);
    mk_entry(col, LV_SYMBOL_BELL, _("Alertes et réveil"), SCR_SET_ALERT, &m_sum[1]);
    mk_entry(col, LV_SYMBOL_BLUETOOTH, _("Bluetooth"), SCR_SET_BT, &m_sum[2]);
    mk_entry(col, LV_SYMBOL_CHARGE, _("Énergie"), SCR_POWER, &m_sum[3]);
    mk_entry(col, LV_SYMBOL_LIST, _("À propos"), SCR_ABOUT, &m_sum[4]);
    mk_entry(col, LV_SYMBOL_REFRESH, _("Mise à jour"), SCR_OTA, &m_sum[5]);

    /* ---- affichage ---- */
    col = mk_page(SCR_SET_DISP, _("Affichage"));
    mk_section(col, _("CADRAN"));
    static const char *face_map[4];
    for (int i = 0; i < 3; i++) face_map[i] = _(FACE_NAMES[i]);
    face_map[3] = "";
    m_bm_face = mk_segmented(col, face_map, menu_face_cb);
    mk_section(col, _("LUMINOSITÉ"));
    m_slider = lv_slider_create(col);
    lv_obj_set_width(m_slider, lv_pct(90));
    lv_obj_set_style_margin_left(m_slider, 8, 0);
    lv_obj_set_style_margin_top(m_slider, 6, 0);
    lv_obj_set_style_margin_bottom(m_slider, 6, 0);
    lv_slider_set_range(m_slider, 10, 255);
    lv_obj_set_ext_click_area(m_slider, 10);
    lv_obj_add_event_cb(m_slider, menu_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(m_slider, menu_slider_cb, LV_EVENT_RELEASED, NULL);
    bubble(m_slider);
    mk_section(col, _("EXTINCTION DE L'ÉCRAN"));
    static const char *tmo_map[] = { "5 s", "8 s", "10 s", "15 s", "30 s", "" };
    m_bm_tmo = mk_segmented(col, tmo_map, menu_tmo_cb);
    mk_section(col, _("LANGUE"));
    static const char *lang_map[] = { "Français", "English", "" };
    m_bm_lang = mk_segmented(col, lang_map, menu_lang_cb);

    /* ---- alertes et réveil ---- */
    col = mk_page(SCR_SET_ALERT, _("Alertes"));
    m_sw_vib = mk_switch_row(col, _("Vibration"), _("Notifications et appels"), menu_vib_cb);
    m_sw_nwake = mk_switch_row(col, _("Allumer l'écran"), _("Sinon : vibreur seul"), menu_nwake_cb);
    m_sw_tap = mk_switch_row(col, _("Double tap"), _("2 tapes sur l'écran éteint"), menu_tap_cb);
    lv_obj_t *ts = mk_button(col, with_sym(LV_SYMBOL_VOLUME_MAX, _("Tester le haut-parleur")), C_CARD,
                             sound_test_cb, NULL);
    lv_obj_set_size(ts, lv_pct(100), 44);

    /* ---- bluetooth ---- */
    col = mk_page(SCR_SET_BT, _("Bluetooth"));
    m_sw_bt = mk_switch_row(col, _("Bluetooth"), _("Coupé = mode avion"), menu_bt_cb);
    m_bt_info = mk_label(col, &font_small_11, C_DIM, "");
    lv_obj_set_style_pad_bottom(m_bt_info, 4, 0);
    lv_obj_t *pb = mk_button(col, with_sym(LV_SYMBOL_BLUETOOTH, _("Appairer un iPhone")), C_CARD, menu_nav_cb,
                             (void *)(uintptr_t)SCR_PAIR);
    lv_obj_set_size(pb, lv_pct(100), 44);
    lv_obj_t *sp = mk_container(col);                 /* écart : action destructive à part */
    lv_obj_set_size(sp, 10, 14);
    lv_obj_t *fg = mk_button(col, with_sym(LV_SYMBOL_TRASH, _("Oublier les appareils")), lv_color_hex(0x3F1D1D),
                             forget_cb, NULL);
    lv_obj_set_size(fg, lv_pct(100), 44);
    lv_obj_set_style_text_color(lv_obj_get_child(fg, 0), lv_color_hex(0xFCA5A5), 0);

    /* boîte de confirmation */
    m_confirm = lv_obj_create(s_scr[SCR_SET_BT]);
    lv_obj_remove_style_all(m_confirm);
    lv_obj_set_size(m_confirm, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_set_style_bg_color(m_confirm, C_BG, 0);
    lv_obj_set_style_bg_opa(m_confirm, LV_OPA_80, 0);
    lv_obj_add_flag(m_confirm, LV_OBJ_FLAG_CLICKABLE);          /* bloque les touches dessous */
    lv_obj_t *card = lv_obj_create(m_confirm);
    lv_obj_set_size(card, 216, 150);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *q = mk_label(card, &font_text_16, C_FG, _("Oublier tous les\nappareils appairés ?"));
    lv_obj_set_style_text_align(q, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_t *q2 = mk_label(card, &font_small_11, C_DIM, _("Il faudra réappairer l'iPhone."));
    lv_obj_align(q2, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_t *no = mk_button(card, _("Annuler"), C_OFF, confirm_cb, (void *)0);
    lv_obj_set_size(no, 88, 40);
    lv_obj_align(no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *yes = mk_button(card, _("Oublier"), C_BAD, confirm_cb, (void *)1);
    lv_obj_set_size(yes, 88, 40);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(m_confirm, LV_OBJ_FLAG_HIDDEN);

    build_ota();
}

/* ------------------------------------------------------------------ */
/* Mise à jour par Wi-Fi                                                */
/* ------------------------------------------------------------------ */
static lv_timer_t *o_reboot_t;

static void ota_reboot_cb(lv_timer_t *t) { ota_reboot_now(); }

static void ota_title(void)
{
    if (o_title) lv_label_set_text(o_title, o_app >= 0 ? _("Réglages WebRX") : _("Mise à jour"));
}

static void ota_ui_running(void)
{
    ota_title();
    /* identifiants en gros, mot de passe groupé par 4 */
    const char *pw = ota_password();
    char g[40] = "";
    size_t o = 0;
    for (size_t i = 0; pw[i] && o + 2 < sizeof(g); i++) {
        if (i && i % 4 == 0) g[o++] = ' ';
        g[o++] = pw[i];
    }
    g[o] = 0;
    char b[96];
    snprintf(b, sizeof(b), _("Wi-Fi « %s », mot de passe :"), ota_ssid());
    lv_label_set_text(o_info, b);
    lv_obj_set_style_text_color(o_info, C_DIM, 0);
    lv_label_set_text(o_pw, g);
    lv_obj_remove_flag(o_pw, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(o_status, C_DIM, 0);
    lv_label_set_text(o_status, _("Puis ouvrir\nhttp://192.168.4.1"));
    lv_label_set_text(lv_obj_get_child(o_btn, 0), _("Annuler"));
    lv_obj_set_style_bg_color(o_btn, C_OFF, 0);
    if (o_prev) lv_obj_add_flag(o_prev, LV_OBJ_FLAG_HIDDEN);
}

static void ota_ui_idle(void)
{
    char b[220];
    ota_title();
    if (o_app >= 0) snprintf(b, sizeof(b), "%s", _("La montre crée un réseau Wi-Fi pour saisir les réglages ; "
                                                 "le Bluetooth est coupé pendant ce temps."));
    else snprintf(b, sizeof(b), _("Version installée : %s\n\nLa montre crée un réseau Wi-Fi ; le Bluetooth est coupé "
             "le temps de la mise à jour.%s"), esp_app_get_description()->version,
             ota_rolled_back() ? _("\n\nLa dernière mise à jour avait échoué : ancien firmware restauré.") : "");
    lv_label_set_text(o_info, b);
    lv_obj_set_style_text_color(o_info, C_FG, 0);
    lv_obj_scroll_to_y(lv_obj_get_parent(o_info), 0, LV_ANIM_OFF);
    if (o_pw) lv_obj_add_flag(o_pw, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(o_status, "");
    lv_obj_add_flag(o_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lv_obj_get_child(o_btn, 0), with_sym(LV_SYMBOL_REFRESH, _("Démarrer")));
    lv_obj_set_style_bg_color(o_btn, C_ACC, 0);
    const char *pv = o_app >= 0 ? NULL : ota_other_version();
    if (o_prev && pv) {
        char t[64];
        snprintf(t, sizeof(t), _("Revenir à la %s"), pv);
        lv_label_set_text(lv_obj_get_child(o_prev, 0), with_sym(LV_SYMBOL_LEFT, t));
        lv_obj_remove_flag(o_prev, LV_OBJ_FLAG_HIDDEN);
    } else if (o_prev) {
        lv_obj_add_flag(o_prev, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Retour à la version précédente : confirmation, puis vérification de
 * l'image et redémarrage dessus. */
static void prev_confirm_cb(lv_event_t *ev)
{
    bool ok = (uintptr_t)lv_event_get_user_data(ev) != 0;
    lv_obj_add_flag(o_prev_modal, LV_OBJ_FLAG_HIDDEN);
    if (!ok) return;
    lv_label_set_text(o_status, _("Vérification de l'image…"));
    lv_obj_set_style_text_color(o_status, C_DIM, 0);
    lv_refr_now(NULL);
    const char *err = ota_boot_other();          /* ne revient qu'en cas d'échec */
    lv_obj_set_style_text_color(o_status, C_BAD, 0);
    lv_label_set_text(o_status, _(err));
}

static void prev_cb(lv_event_t *ev)
{
    if (ota_active()) return;
    lv_obj_remove_flag(o_prev_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(o_prev_modal);
}

static lv_obj_t *mk_modal(lv_obj_t *parent, const char *q, const char *sub, const char *ok_txt,
                          lv_color_t ok_col, lv_event_cb_t cb)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_add_flag(m, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(m, 0, 0);
    lv_obj_set_style_bg_color(m, C_BG, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_80, 0);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);              /* bloque les touches dessous */
    lv_obj_t *card = lv_obj_create(m);
    lv_obj_set_size(card, 220, 176);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = mk_label(card, &font_text_16, C_FG, q);
    lv_obj_set_width(l, 190);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_t *l2 = mk_label(card, &font_small_11, C_DIM, sub);
    lv_obj_set_width(l2, 190);
    lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_t *no = mk_button(card, _("Annuler"), C_OFF, cb, (void *)0);
    lv_obj_set_size(no, 88, 40);
    lv_obj_align(no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *yes = mk_button(card, ok_txt, ok_col, cb, (void *)1);
    lv_obj_set_size(yes, 88, 40);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    return m;
}

static void ota_btn_cb(lv_event_t *ev)
{
    if (ota_active()) {
        if (o_app >= 0) sdr_mode_set_boot_app(o_app);   /* l'abandon redémarre : retour dans l'app */
        ota_cancel();
        return;
    }
    lv_label_set_text(o_status, _("Démarrage du Wi-Fi…"));
    lv_refr_now(NULL);
    const char *err = ota_start();
    if (err) {
        lv_obj_set_style_text_color(o_status, C_BAD, 0);
        lv_label_set_text(o_status, _(err));
        return;
    }
    ota_ui_running();
}

void ui_on_ota(int code)
{
    if (s_cur != SCR_OTA) show(SCR_OTA, LV_SCR_LOAD_ANIM_NONE);
    if (!o_status) return;
    char b[96];
    if (code >= 0 && code <= OTA_EV_PROGRESS_MAX) {
        lv_obj_remove_flag(o_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(o_bar, code, LV_ANIM_OFF);
        snprintf(b, sizeof(b), _("Réception… %d %%"), code);
        lv_obj_set_style_text_color(o_status, C_FG, 0);
        lv_label_set_text(o_status, b);
    } else if (code == OTA_EV_SAVED) {
        snprintf(b, sizeof(b), LV_SYMBOL_OK " %s", _("Réglages enregistrés\nRedémarrage…"));
        lv_obj_set_style_text_color(o_status, C_OK, 0);
        lv_label_set_text(o_status, b);
        lv_obj_add_flag(o_btn, LV_OBJ_FLAG_HIDDEN);
        if (o_app >= 0) sdr_mode_set_boot_app(o_app);
        if (!o_reboot_t) o_reboot_t = lv_timer_create(ota_reboot_cb, 2000, NULL);
    } else if (code == OTA_EV_CLIENT) {
        lv_label_set_text(o_status, _("Appareil connecté : ouvrir http://192.168.4.1"));
    } else if (code == OTA_EV_DONE) {
        lv_bar_set_value(o_bar, 100, LV_ANIM_OFF);
        {
            char v[64];
            snprintf(v, sizeof(v), _("Version %s installée\nRedémarrage…"), ota_new_version());
            snprintf(b, sizeof(b), LV_SYMBOL_OK " %s", v);
        }
        lv_obj_set_style_text_color(o_status, C_OK, 0);
        lv_label_set_text(o_status, b);
        lv_obj_add_flag(o_btn, LV_OBJ_FLAG_HIDDEN);
        if (!o_reboot_t) o_reboot_t = lv_timer_create(ota_reboot_cb, 2000, NULL);
    } else {
        const char *m = code == OTA_EV_ERR_IMAGE ? _("Fichier refusé (pas un firmware T-Watch ?)") :
                        code == OTA_EV_ERR_SIZE ? _("Fichier trop gros") :
                        code == OTA_EV_ERR_NET ? _("Transfert interrompu : réessayer") : _("Erreur d'écriture");
        lv_obj_set_style_text_color(o_status, C_BAD, 0);
        lv_label_set_text(o_status, m);
    }
}

static void build_ota(void)
{
    lv_obj_t *col = mk_page(SCR_OTA, _("Mise à jour"));
    o_title = lv_obj_get_child(lv_obj_get_child(s_scr[SCR_OTA], 0), 1);
    o_info = mk_label(col, &font_text_16, C_FG, "");
    o_pw = mk_label(col, &font_num_44, C_FG, "");          /* mot de passe, bien lisible */
    lv_obj_add_flag(o_pw, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_long_mode(o_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(o_info, lv_pct(100));
    o_bar = lv_bar_create(col);
    lv_obj_set_size(o_bar, lv_pct(100), 8);
    lv_bar_set_range(o_bar, 0, 100);
    lv_obj_set_style_bg_color(o_bar, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o_bar, C_ACC, LV_PART_INDICATOR);
    bubble(o_bar);
    o_status = mk_label(col, &font_text_16, C_DIM, "");
    lv_label_set_long_mode(o_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(o_status, lv_pct(100));
    o_btn = mk_button(col, "", C_ACC, ota_btn_cb, NULL);
    lv_obj_set_size(o_btn, lv_pct(100), 44);
    /* retour à la version précédente : à part, sous une confirmation */
    lv_obj_t *sp = mk_container(col);
    lv_obj_set_size(sp, 10, 10);
    o_prev = mk_button(col, "", C_CARD, prev_cb, NULL);
    lv_obj_set_size(o_prev, lv_pct(100), 44);
    lv_obj_add_flag(o_prev, LV_OBJ_FLAG_HIDDEN);
    o_prev_modal = mk_modal(s_scr[SCR_OTA], _("Revenir à la version précédente ?"),
                            _("La montre redémarre dessus. Réglages et appairage sont conservés."),
                            _("Revenir"), C_WARN, prev_confirm_cb);
    if (ota_active()) ota_ui_running();
    else ota_ui_idle();
}

static void menu_sync_from_settings(void)
{
    settings_t *st = settings_get();
    lv_slider_set_value(m_slider, st->brightness, LV_ANIM_OFF);
    lv_obj_set_state(m_sw_vib, LV_STATE_CHECKED, st->vibrate);
    lv_obj_set_state(m_sw_tap, LV_STATE_CHECKED, st->tap_wake);
    lv_obj_set_state(m_sw_nwake, LV_STATE_CHECKED, st->notif_wake);
    lv_obj_set_state(m_sw_bt, LV_STATE_CHECKED, st->ble_enabled);
    for (int i = 0; i < 3; i++) {
        if (FACE_ORDER[i] == st->watchface) lv_buttonmatrix_set_button_ctrl(m_bm_face, i, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
    lv_buttonmatrix_set_button_ctrl(m_bm_lang, st->lang ? 1 : 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    for (size_t i = 0; i < sizeof(TMO_VALUES); i++) {
        if (TMO_VALUES[i] >= st->screen_timeout_s) {
            lv_buttonmatrix_set_button_ctrl(m_bm_tmo, i, LV_BUTTONMATRIX_CTRL_CHECKED);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Énergie : télémétrie AXP202                                          */
/* ------------------------------------------------------------------ */
static void power_refresh(void)
{
    power_telemetry_t t;
    power_get_telemetry(&t);
    ble_info_t b;
    ble_get_info(&b);
    char s[112];

    snprintf(s, sizeof(s), _("Batterie  %d %%  ·  %.3f V"), t.percent, t.vbat_mv / 1000.0f);
    lv_label_set_text(p_lbl[0], s);

    if (t.vbus) {
        if (t.charging) snprintf(s, sizeof(s), _("Charge  +%.0f mA"), t.i_chg_ma);
        else snprintf(s, sizeof(s), _("Secteur · charge terminée"));
    } else {
        snprintf(s, sizeof(s), _("Décharge  %.1f mA"), t.i_dischg_ma);
    }
    lv_label_set_text(p_lbl[1], s);

    if (t.vbus) {   /* en charge, le compteur mesure ce qui entre dans la batterie */
        snprintf(s, sizeof(s), _("Chargé  %.0f mAh en %luh%02lu"), t.used_mah < 0 ? -t.used_mah : 0.0f,
                 (unsigned long)(t.since_s / 3600), (unsigned long)((t.since_s / 60) % 60));
    } else {
        snprintf(s, sizeof(s), _("Conso  %.2f mAh en %luh%02lu"), t.used_mah,
                 (unsigned long)(t.since_s / 3600), (unsigned long)((t.since_s / 60) % 60));
    }
    lv_label_set_text(p_lbl[2], s);

    if (t.vbus) {
        snprintf(s, sizeof(s), _("Débrancher pour mesurer"));
    } else if (t.avg_ma > 0.1f) {
        float left_h = power_capacity_mah() * t.percent / 100.0f / t.avg_ma;
        snprintf(s, sizeof(s), _("Moy. %.1f mA, reste ~%.0f h"), t.avg_ma, left_h);
    } else {
        snprintf(s, sizeof(s), _("Moyenne : mesure en cours…"));
    }
    lv_label_set_text(p_lbl[3], s);

    static const char *const ST[] = { N_("coupé"), N_("publicité"), N_("connecté"), N_("chiffré"), N_("prêt") };
    if (b.itvl_x125) {
        snprintf(s, sizeof(s), "BLE %s · %.0f ms · lat %u", _(ST[b.state]), b.itvl_x125 * 1.25f, b.latency);
    } else {
        snprintf(s, sizeof(s), "BLE %s%s", _(ST[b.state]),
                 (b.state == BLE_ST_ADVERTISING && !b.adv_active) ? " INACTIVE" : "");
    }
    lv_label_set_text(p_lbl[4], s);

    if (t.vbus) {
        snprintf(s, sizeof(s), _("Veille : sur batterie"));
    } else if (t.off_avg_ma > 0.05f) {
        snprintf(s, sizeof(s), _("Veille %.1f mA · écran %.0f%%"), t.off_avg_ma, t.screen_pct);
    } else {
        snprintf(s, sizeof(s), _("Écran allumé %.0f %% du temps"), t.screen_pct);
    }
    lv_label_set_text(p_lbl[5], s);
    /* ligne 1 : jauge (capacité apprise) ; ligne 2 : décharge précédente */
    int n = power_capacity_samples();
    char l1[64], l2[64] = "";
    if (n > 0) snprintf(l1, sizeof(l1), _("Jauge : capacité ~%u mAh (%d mes.)"), power_capacity_mah(), n);
    else if (power_soc_known()) snprintf(l1, sizeof(l1), _("Jauge : calibrage, %u mAh nominal"), power_capacity_mah());
    else snprintf(l1, sizeof(l1), _("Jauge : estimée (pas encore calibrée)"));
    power_hist_t h;
    if (power_last_discharge(&h)) {
        static const char *const END[] = { "", N_("rechargée"), N_("coupure"), N_("redémarrage") };
        snprintf(l2, sizeof(l2), _("Préc. %.0f mAh en %luh%02lu, %s"), h.used_mah,
                 (unsigned long)(h.since_s / 3600), (unsigned long)((h.since_s / 60) % 60), _(END[h.end & 3]));
    }
    snprintf(s, sizeof(s), "%s%s%s", l1, l2[0] ? "\n" : "", l2);
    lv_label_set_text(p_lbl[6], s);
}

static void power_timer_cb(lv_timer_t *t) { power_refresh(); }

static void build_power(void)
{
    lv_obj_t *s = s_scr[SCR_POWER] = mk_screen();
    lv_obj_t *h = mk_label(s, &font_title_20, C_FG, with_sym(LV_SYMBOL_CHARGE, _("Énergie")));
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 12, 10);
    for (int i = 0; i < 7; i++) {
        p_lbl[i] = mk_label(s, i == 6 ? &font_small_11 : &font_text_16, i == 3 ? C_ACC : (i == 6 ? C_DIM : C_FG), "");
        lv_label_set_long_mode(p_lbl[i], LV_LABEL_LONG_DOT);
        lv_obj_set_size(p_lbl[i], BOARD_LCD_W - 24, lv_font_get_line_height(&font_text_16));
        lv_obj_set_pos(p_lbl[i], 12, 42 + i * 27);
    }
    lv_label_set_long_mode(p_lbl[6], LV_LABEL_LONG_WRAP);        /* 2 lignes en petit */
    lv_obj_set_height(p_lbl[6], 2 * lv_font_get_line_height(&font_small_11));
    p_timer = lv_timer_create(power_timer_cb, 1000, NULL);
    lv_timer_pause(p_timer);
}

/* ------------------------------------------------------------------ */
/* Appairage                                                            */
/* ------------------------------------------------------------------ */
static void pair_done_timer_cb(lv_timer_t *t)
{
    lv_timer_pause(pa_timer);
    if (s_cur == SCR_PAIR) show(SCR_WATCH, LV_SCR_LOAD_ANIM_FADE_OUT);
}

static void build_pair(void)
{
    lv_obj_t *s = s_scr[SCR_PAIR] = mk_screen();
    lv_obj_t *h = mk_label(s, &font_title_20, C_FG, with_sym(LV_SYMBOL_BLUETOOTH, _("Appairage")));
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 12);
    pa_code = mk_label(s, &font_num_44, C_FG, "------");
    lv_obj_align(pa_code, LV_ALIGN_CENTER, 0, -16);
    pa_hint = mk_label(s, &font_text_16, C_DIM, "");
    lv_obj_set_style_text_align(pa_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(pa_hint, LV_ALIGN_BOTTOM_MID, 0, -14);
    pa_timer = lv_timer_create(pair_done_timer_cb, 2000, NULL);
    lv_timer_pause(pa_timer);
}

/* ------------------------------------------------------------------ */
/* À propos                                                             */
/* ------------------------------------------------------------------ */
static void about_refresh(void)
{
    ble_info_t b;
    ble_get_info(&b);
    const esp_app_desc_t *d = esp_app_get_description();
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    EXT_RAM_BSS_ATTR static char s[1700];
    int n = snprintf(s, sizeof(s),
             _("Firmware %s\nESP-IDF %s · LVGL %d.%d.%d\n"
               "BLE %02X:%02X:%02X:%02X:%02X:%02X\nAppareils appairés : %d\n"
               "En marche depuis %luj %02luh%02lu\nTas libre %u Ko (min %u Ko)\nPlus grand bloc %u Ko\nReset : %d\n"
               "Réveils écran éteint :\n tactile %lu · notif %lu\n média %lu · AXP %lu · BLE %lu"),
             d->version, esp_get_idf_version(), LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             b.addr[5], b.addr[4], b.addr[3], b.addr[2], b.addr[1], b.addr[0], b.bonds,
             (unsigned long)(up / 86400), (unsigned long)((up / 3600) % 24), (unsigned long)((up / 60) % 60),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (int)esp_reset_reason(),
             (unsigned long)g_wake_cnt[EV_TOUCH_INT], (unsigned long)g_wake_cnt[EV_NOTIF],
             (unsigned long)g_wake_cnt[EV_MEDIA], (unsigned long)g_wake_cnt[EV_AXP_IRQ],
             (unsigned long)g_wake_cnt[EV_BLE_STATE]);
    if (n > 0 && n < (int)sizeof(s)) {
        /* rails AXP (0x12 sorties, 0x29 LDO3 : bit 7 mode, 6:0 tension) et son */
        n += snprintf(s + n, sizeof(s) - n, "\nPSRAM %u/%u Ko\nAXP 12=%02X 28=%02X 29=%02X · audio %d",
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                      (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
                      axp_reg(0x12), axp_reg(0x28), axp_reg(0x29), sound_last_error());
    }
    if (n > 0 && n < (int)sizeof(s) && crashlog_text()[0]) {
        n += snprintf(s + n, sizeof(s) - n, "\n\n%s", crashlog_text());
    }
    if (n > 0 && n < (int)sizeof(s)) {
        n += snprintf(s + n, sizeof(s) - n, _("\n\nJournal BLE (pub. %s) :\n"),
                      b.adv_active ? "active" : "INACTIVE");
        if (n < (int)sizeof(s)) ble_diag_dump(s + n, sizeof(s) - n);
    }
    lv_label_set_text(a_lbl, s);
}

static lv_obj_t *a_modal;

static void reboot_confirm_cb(lv_event_t *ev)
{
    bool ok = (uintptr_t)lv_event_get_user_data(ev) != 0;
    lv_obj_add_flag(a_modal, LV_OBJ_FLAG_HIDDEN);
    if (!ok) return;
    /* redémarrage voulu : une mise à jour encore « à l'essai » est validée,
     * sinon le bootloader reviendrait à l'ancienne version */
    esp_ota_mark_app_valid_cancel_rollback();
    esp_restart();
}

static void reboot_cb(lv_event_t *ev)
{
    lv_obj_scroll_to_y(s_scr[SCR_ABOUT], 0, LV_ANIM_OFF);
    lv_obj_remove_flag(a_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(a_modal);
}

static void build_about(void)
{
    lv_obj_t *s = s_scr[SCR_ABOUT] = mk_screen();
    lv_obj_t *h = mk_label(s, &font_title_20, C_FG, _("À propos"));
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 12, 10);
    a_lbl = mk_label(s, &font_text_16, C_DIM, "");
    lv_obj_set_width(a_lbl, BOARD_LCD_W - 24);
    lv_label_set_long_mode(a_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(a_lbl, 12, 92);
    lv_obj_set_style_pad_bottom(a_lbl, 16, 0);
    lv_obj_t *rb = mk_button(s, with_sym(LV_SYMBOL_REFRESH, _("Redémarrer la montre")), C_CARD, reboot_cb, NULL);
    lv_obj_set_size(rb, BOARD_LCD_W - 24, 40);
    lv_obj_set_pos(rb, 12, 42);
    a_modal = mk_modal(s, _("Redémarrer ?"), _("Réglages et appairage sont conservés."),
                       _("Oui"), C_WARN, reboot_confirm_cb);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);       /* journal BLE en dessous */
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_ACTIVE);
}

/* ------------------------------------------------------------------ */
/* Lecture en cours (AMS)                                               */
/* ------------------------------------------------------------------ */
static lv_obj_t *mu_box, *mu_empty, *mu_player, *mu_title, *mu_artist, *mu_album;
static lv_obj_t *mu_bar, *mu_pos, *mu_dur, *mu_play_lbl, *mu_vol;
static lv_obj_t *mu_btn[5];          /* prev, play, next, vol-, vol+ */
static uint32_t mu_seq = UINT32_MAX;

static void fmt_mmss(char *b, size_t n, float sec)
{
    int t = sec > 0 ? (int)sec : 0;
    if (t >= 3600) snprintf(b, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(b, n, "%d:%02d", t / 60, t % 60);
}

static void set_clean_text(lv_obj_t *l, const char *src)
{
    char b[MEDIA_TXT_MAX];
    strlcpy(b, src, sizeof(b));
    text_sanitize(b);              /* UTF-8 tronqué, émojis : comme les notifications */
    lv_label_set_text(l, b);
}

static void media_btn_cb(lv_event_t *ev)
{
    media_command((uint8_t)(uintptr_t)lv_event_get_user_data(ev));
}

static void media_refresh(bool full)
{
    media_info_t m;
    media_get(&m);
    bool has = m.available && (m.title[0] || m.player[0]);
    if (!has) {
        lv_obj_add_flag(mu_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(mu_empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(mu_empty, LV_SYMBOL_AUDIO "\n\n%s",
                              m.available ? _("Aucune lecture en cours") : _("iPhone non connecté"));
        mu_seq = UINT32_MAX;
        return;
    }
    lv_obj_remove_flag(mu_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mu_empty, LV_OBJ_FLAG_HIDDEN);

    char b[24];
    if (full || m.seq != mu_seq) {
        char pl[sizeof(m.player)], pl2[sizeof(m.player) + 8];
        strlcpy(pl, m.player[0] ? m.player : _("Lecture"), sizeof(pl));
        text_sanitize(pl);             /* avant l'icône : le nettoyage retire les glyphes privés */
        snprintf(pl2, sizeof(pl2), LV_SYMBOL_AUDIO "  %s", pl);
        lv_label_set_text(mu_player, pl2);
        set_clean_text(mu_title, m.title[0] ? m.title : "—");
        set_clean_text(mu_artist, m.artist);
        set_clean_text(mu_album, m.album);
        bool playing = m.state == MEDIA_PLAYING || m.state == MEDIA_FFWD || m.state == MEDIA_REWINDING;
        lv_label_set_text(mu_play_lbl, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        if (m.duration_s > 0) fmt_mmss(b, sizeof(b), m.duration_s);
        else b[0] = 0;
        lv_label_set_text(mu_dur, b);
        if (m.volume >= 0) {
            lv_obj_remove_flag(mu_vol, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(mu_vol, (int32_t)(m.volume * 100 + 0.5f), LV_ANIM_OFF);
        } else {
            lv_obj_add_flag(mu_vol, LV_OBJ_FLAG_HIDDEN);
        }
        /* commandes annoncées par l'app (liste vide = inconnue : tout permis) */
        static const uint8_t ids[5] = { MEDIA_CMD_PREV, MEDIA_CMD_TOGGLE, MEDIA_CMD_NEXT,
                                        MEDIA_CMD_VOL_DOWN, MEDIA_CMD_VOL_UP };
        for (int i = 0; i < 5; i++) {
            bool ok = !m.cmds || (m.cmds & (1u << ids[i])) ||
                      (ids[i] == MEDIA_CMD_TOGGLE && (m.cmds & ((1u << MEDIA_CMD_PLAY) | (1u << MEDIA_CMD_PAUSE))));
            if (ok) lv_obj_remove_state(mu_btn[i], LV_STATE_DISABLED);
            else lv_obj_add_state(mu_btn[i], LV_STATE_DISABLED);
        }
        mu_seq = m.seq;
    }
    float pos = media_position_s(&m);
    fmt_mmss(b, sizeof(b), pos);
    lv_label_set_text(mu_pos, b);
    lv_bar_set_value(mu_bar, m.duration_s > 0 ? (int32_t)(pos * 1000 / m.duration_s) : 0, LV_ANIM_OFF);
}

static void media_timer_cb(lv_timer_t *t) { media_refresh(false); }

static lv_obj_t *media_button(lv_obj_t *p, const char *sym, int size, lv_color_t bg, uint8_t cmd,
                              const lv_font_t *font, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, media_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)cmd);
    lv_obj_set_ext_click_area(b, 6);
    bubble(b);
    lv_obj_t *l = mk_label(b, font, C_FG, sym);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

static void build_media(void)
{
    lv_obj_t *s = s_scr[SCR_MEDIA] = mk_screen();

    mu_empty = mk_label(s, &font_text_16, C_DIM, "");
    lv_label_set_long_mode(mu_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(mu_empty, 210);
    lv_obj_set_style_text_align(mu_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(mu_empty);

    mu_box = mk_container(s);
    lv_obj_set_size(mu_box, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_remove_flag(mu_box, LV_OBJ_FLAG_SCROLLABLE);

    mu_player = mk_label(mu_box, &font_text_16, C_DIM, "");
    lv_label_set_long_mode(mu_player, LV_LABEL_LONG_DOT);
    lv_obj_set_width(mu_player, 200);
    lv_obj_set_style_text_align(mu_player, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mu_player, LV_ALIGN_TOP_MID, 0, 6);

    mu_title = mk_label(mu_box, &font_title_20, C_FG, "");
    lv_label_set_long_mode(mu_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(mu_title, 216, 2 * lv_font_get_line_height(&font_title_20));
    lv_obj_set_style_text_align(mu_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mu_title, LV_ALIGN_TOP_MID, 0, 28);

    mu_artist = mk_label(mu_box, &font_text_16, C_ACC, "");
    lv_label_set_long_mode(mu_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(mu_artist, 216);
    lv_obj_set_style_text_align(mu_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mu_artist, LV_ALIGN_TOP_MID, 0, 80);

    mu_album = mk_label(mu_box, &font_small_11, C_DIM, "");
    lv_label_set_long_mode(mu_album, LV_LABEL_LONG_DOT);
    lv_obj_set_width(mu_album, 216);
    lv_obj_set_style_text_align(mu_album, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mu_album, LV_ALIGN_TOP_MID, 0, 101);

    mu_bar = lv_bar_create(mu_box);
    lv_obj_set_size(mu_bar, 200, 5);
    lv_obj_align(mu_bar, LV_ALIGN_TOP_MID, 0, 122);
    lv_bar_set_range(mu_bar, 0, 1000);
    lv_obj_set_style_bg_color(mu_bar, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mu_bar, C_FG, LV_PART_INDICATOR);
    bubble(mu_bar);
    mu_pos = mk_label(mu_box, &font_small_11, C_DIM, "");
    lv_obj_align(mu_pos, LV_ALIGN_TOP_LEFT, 20, 130);
    mu_dur = mk_label(mu_box, &font_small_11, C_DIM, "");
    lv_obj_align(mu_dur, LV_ALIGN_TOP_RIGHT, -20, 130);

    mu_btn[1] = media_button(mu_box, LV_SYMBOL_PLAY, 62, C_ACC, MEDIA_CMD_TOGGLE, &font_title_20, &mu_play_lbl);
    lv_obj_align(mu_btn[1], LV_ALIGN_TOP_MID, 0, 148);
    mu_btn[0] = media_button(mu_box, LV_SYMBOL_PREV, 48, C_CARD, MEDIA_CMD_PREV, &font_text_16, NULL);
    lv_obj_align(mu_btn[0], LV_ALIGN_TOP_MID, -70, 155);
    mu_btn[2] = media_button(mu_box, LV_SYMBOL_NEXT, 48, C_CARD, MEDIA_CMD_NEXT, &font_text_16, NULL);
    lv_obj_align(mu_btn[2], LV_ALIGN_TOP_MID, 70, 155);

    mu_btn[3] = media_button(mu_box, LV_SYMBOL_VOLUME_MID, 30, C_CARD, MEDIA_CMD_VOL_DOWN, &font_small_11, NULL);
    lv_obj_align(mu_btn[3], LV_ALIGN_BOTTOM_LEFT, 22, -4);
    lv_obj_t *l3 = lv_obj_get_child(mu_btn[3], 0);
    lv_obj_set_style_text_font(l3, &font_text_16, 0);
    lv_label_set_text(l3, "-");
    mu_btn[4] = media_button(mu_box, LV_SYMBOL_VOLUME_MAX, 30, C_CARD, MEDIA_CMD_VOL_UP, &font_text_16, NULL);
    lv_obj_align(mu_btn[4], LV_ALIGN_BOTTOM_RIGHT, -22, -4);
    lv_label_set_text(lv_obj_get_child(mu_btn[4], 0), "+");
    mu_vol = lv_bar_create(mu_box);
    lv_obj_set_size(mu_vol, 110, 4);
    lv_obj_align(mu_vol, LV_ALIGN_BOTTOM_MID, 0, -17);
    lv_bar_set_range(mu_vol, 0, 100);
    lv_obj_set_style_bg_color(mu_vol, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mu_vol, C_DIM, LV_PART_INDICATOR);
    bubble(mu_vol);

    mu_timer = lv_timer_create(media_timer_cb, 1000, NULL);
    lv_timer_pause(mu_timer);
}

void ui_on_media(void)
{
    if (s_cur == SCR_MEDIA) media_refresh(false);
}

/* ------------------------------------------------------------------ */
/* Lanceur d'apps (← depuis le cadran)                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *name, *icon;
    uint32_t color;
    scr_id_t scr;            /* écran système, ou SCR_APP */
    const app_t *app;
} launch_t;

#define LAUNCH_MAX 16
static launch_t s_launch[LAUNCH_MAX];
static int s_launch_n;

static void launch_cb(lv_event_t *ev)
{
    int i = (int)(uintptr_t)lv_event_get_user_data(ev);
    if (i < 0 || i >= s_launch_n) return;
    if (s_launch[i].app) app_open(s_launch[i].app);
    else nav_open(s_launch[i].scr, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void launch_add(const char *name, const char *icon, uint32_t color, scr_id_t scr, const app_t *app)
{
    if (s_launch_n >= LAUNCH_MAX) return;
    s_launch[s_launch_n++] = (launch_t){ name, icon, color, scr, app };
}

static void build_launcher(void)
{
    s_launch_n = 0;
    launch_add(N_("Lecture"), LV_SYMBOL_AUDIO, 0x1DB954, SCR_MEDIA, NULL);
    for (int i = 0; i < g_app_count; i++)
        launch_add(g_apps[i]->name, g_apps[i]->icon, g_apps[i]->color, SCR_APP, g_apps[i]);
    launch_add(N_("Énergie"), LV_SYMBOL_BATTERY_FULL, 0x22C55E, SCR_POWER, NULL);
    launch_add(N_("Réglages"), LV_SYMBOL_SETTINGS, 0x8A8F98, SCR_MENU, NULL);

    lv_obj_t *s = s_scr[SCR_LAUNCHER] = mk_screen();
    lv_obj_t *t = mk_label(s, &font_text_16, C_DIM, _("Apps"));
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *grid = mk_container(s);
    lv_obj_set_size(grid, BOARD_LCD_W, BOARD_LCD_H - 30);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(grid, 4, 0);
    lv_obj_set_style_pad_row(grid, 4, 0);
    lv_obj_set_style_pad_column(grid, 4, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < s_launch_n; i++) {
        lv_obj_t *cell = mk_container(grid);
        lv_obj_set_size(cell, 74, 98);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(cell, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(cell, C_FG, LV_STATE_PRESSED);
        lv_obj_set_style_radius(cell, 14, 0);
        lv_obj_add_event_cb(cell, launch_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *disc = lv_obj_create(cell);
        lv_obj_set_size(disc, 58, 58);
        lv_obj_align(disc, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(disc, 0, 0);
        lv_obj_set_style_bg_color(disc, lv_color_hex(s_launch[i].color), 0);
        lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
        lv_obj_remove_flag(disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        bubble(disc);
        lv_obj_t *ic = mk_label(disc, &font_title_20, lv_color_hex(0x000000), s_launch[i].icon);
        lv_obj_center(ic);

        lv_obj_t *nm = mk_label(cell, &font_small_11, C_FG, _(s_launch[i].name));
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
}

static void release_launcher(void)
{
    if (!s_scr[SCR_LAUNCHER] || s_cur == SCR_LAUNCHER) return;
    scr_delete(s_scr[SCR_LAUNCHER]);
    s_scr[SCR_LAUNCHER] = NULL;
}

/* ------------------------------------------------------------------ */
/* Contrôles rapides (↓ depuis le cadran)                               */
/* ------------------------------------------------------------------ */
enum { Q_BT, Q_DND, Q_LAMP, Q_MEDIA, Q_N };
static lv_obj_t *q_tile[Q_N];

static const app_t *find_app(const char *name)
{
    for (int i = 0; i < g_app_count; i++)
        if (!strcmp(g_apps[i]->name, name)) return g_apps[i];
    return NULL;
}

static void q_set_tile(int i, bool on)
{
    if (!q_tile[i]) return;
    lv_obj_set_style_bg_color(q_tile[i], on ? C_ACC : C_CARD, 0);
}

static void quick_refresh(void)
{
    if (!s_scr[SCR_QUICK]) return;
    ble_info_t bi;
    ble_get_info(&bi);
    const char *lk = bi.state == BLE_ST_READY ? _("iPhone connecté") :
                     (bi.state == BLE_ST_CONNECTED || bi.state == BLE_ST_SECURED) ? _("Connexion…") :
                     bi.state == BLE_ST_OFF ? _("Bluetooth coupé") : _("iPhone absent");
    lv_label_set_text_fmt(q_info, "%s%d%%  ·  %s", power_charging() ? LV_SYMBOL_CHARGE " " : "",
                          power_percent(), lk);
    q_set_tile(Q_BT, ble_running());
    q_set_tile(Q_DND, settings_get()->dnd);
    lv_bar_set_value(q_bri, settings_get()->brightness, LV_ANIM_OFF);
}

static void quick_tile_cb(lv_event_t *ev)
{
    int i = (int)(uintptr_t)lv_event_get_user_data(ev);
    settings_t *st = settings_get();
    switch (i) {
    case Q_BT:
        st->ble_enabled = !ble_running();
        settings_save();
        if (st->ble_enabled) { ble_start(); toast(_("Bluetooth activé")); }
        else { ble_stop(); toast(_("Mode avion")); }
        break;
    case Q_DND:
        st->dnd = !st->dnd;
        settings_save();
        toast(st->dnd ? _("Ne pas déranger") : _("Notifications actives"));
        watch_refresh_status();
        break;
    case Q_LAMP: {
        const app_t *a = find_app("Lampe");
        if (a) app_open(a);
        return;
    }
    case Q_MEDIA:
        nav_open(SCR_MEDIA, LV_SCR_LOAD_ANIM_MOVE_LEFT);
        return;
    }
    quick_refresh();
}

static void quick_bri_cb(lv_event_t *ev)
{
    int d = (int)(intptr_t)lv_event_get_user_data(ev);
    int v = settings_get()->brightness + d;
    if (v < 10) v = 10;
    if (v > 255) v = 255;
    settings_get()->brightness = (uint8_t)v;
    display_set_brightness((uint8_t)v);
    settings_save();
    quick_refresh();
}

static lv_obj_t *q_mk_tile(lv_obj_t *p, int i, const char *icon, const char *txt, int x, int y)
{
    lv_obj_t *t = lv_obj_create(p);
    lv_obj_set_size(t, 104, 62);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_radius(t, 16, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_bg_color(t, C_CARD, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, C_OFF, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    bubble(t);
    lv_obj_add_event_cb(t, quick_tile_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    lv_obj_t *ic = mk_label(t, &font_title_20, C_FG, icon);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *l = mk_label(t, &font_small_11, C_FG, txt);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -7);
    q_tile[i] = t;
    return t;
}

static void build_quick(void)
{
    lv_obj_t *s = s_scr[SCR_QUICK] = mk_screen();
    q_info = mk_label(s, &font_text_16, C_DIM, "");
    lv_obj_align(q_info, LV_ALIGN_TOP_MID, 0, 8);

    q_mk_tile(s, Q_BT, LV_SYMBOL_BLUETOOTH, _("Bluetooth"), 12, 36);
    q_mk_tile(s, Q_DND, SYM_MOON, _("Ne pas déranger"), 124, 36);
    q_mk_tile(s, Q_LAMP, SYM_BULB, _("Lampe"), 12, 106);
    q_mk_tile(s, Q_MEDIA, LV_SYMBOL_AUDIO, _("Lecture"), 124, 106);

    /* luminosité : − [barre] + (gros boutons, faciles au doigt) */
    lv_obj_t *mn = mk_button(s, "-", C_CARD, quick_bri_cb, (void *)(intptr_t)-49);
    lv_obj_set_style_text_font(lv_obj_get_child(mn, 0), &font_title_20, 0);
    lv_obj_set_size(mn, 48, 44);
    lv_obj_set_pos(mn, 12, 180);
    lv_obj_t *pl = mk_button(s, "+", C_CARD, quick_bri_cb, (void *)(intptr_t)49);
    lv_obj_set_style_text_font(lv_obj_get_child(pl, 0), &font_title_20, 0);
    lv_obj_set_size(pl, 48, 44);
    lv_obj_set_pos(pl, 180, 180);
    q_bri = lv_bar_create(s);
    lv_obj_set_size(q_bri, 100, 10);
    lv_obj_set_pos(q_bri, 70, 197);
    lv_bar_set_range(q_bri, 10, 255);
    lv_obj_set_style_bg_color(q_bri, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(q_bri, C_FG, LV_PART_INDICATOR);
    bubble(q_bri);
    lv_obj_t *sun = mk_label(s, &font_small_11, C_DIM, _("Luminosité"));
    lv_obj_align(sun, LV_ALIGN_TOP_MID, 0, 178);
}

static void release_quick(void)
{
    if (!s_scr[SCR_QUICK] || s_cur == SCR_QUICK) return;
    scr_delete(s_scr[SCR_QUICK]);
    s_scr[SCR_QUICK] = NULL;
    q_info = q_bri = NULL;
    memset(q_tile, 0, sizeof(q_tile));
}

/* ------------------------------------------------------------------ */
/* Hébergement des apps                                                 */
/* ------------------------------------------------------------------ */
static int app_index(const app_t *a)
{
    for (int i = 0; i < g_app_count; i++) if (g_apps[i] == a) return i;
    return -1;
}

static void app_build(const app_t *a)
{
    lv_obj_t *s = s_scr[SCR_APP] = mk_screen();
    a->create(s);
}

static void app_drop_screen(void)
{
    if (!s_scr[SCR_APP]) return;
    if (s_app_cur && s_app_cur->destroy) s_app_cur->destroy();
    scr_delete(s_scr[SCR_APP]);
    s_scr[SCR_APP] = NULL;
}

static void release_app(void)
{
    if (s_cur == SCR_APP) return;
    app_drop_screen();
}

void app_open(const app_t *a)
{
    crashlog_mark('P', app_index(a), s_cur);
    if (!a || app_index(a) < 0) return;
    if (s_cur == SCR_APP) {
        if (s_app_cur == a) return;
        /* remplacer l'app affichée sans repasser par l'écran d'origine */
        if (s_app_cur && s_app_cur->on_hide) s_app_cur->on_hide();
        if (s_app_cur && s_app_cur->destroy) s_app_cur->destroy();
        lv_obj_t *old = s_scr[SCR_APP];
        s_app_cur = a;
        app_build(a);
        if (a->on_show) a->on_show();
        scr_anim_settle();
        lv_screen_load(s_scr[SCR_APP]);
        scr_delete(old);
        return;
    }
    if (s_app_cur != a) app_drop_screen();   /* vue d'une autre app, en réserve */
    s_app_cur = a;
    nav_open(SCR_APP, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

/* Réglages d'une app par la page web de la montre (point d'accès de la mise
 * à jour). La session se termine toujours par un redémarrage, qui rouvre l'app. */
void app_open_settings_portal(const app_t *a)
{
    int idx = app_index(a);
    if (idx < 0 || ota_active()) return;
    o_app = idx;
    ota_set_page(OTA_PAGE_WEBRX);
    /* sans animation : la vue de l'app est libérée tout de suite, sa RAM
     * interne revient au serveur web et au Wi-Fi */
    show(SCR_OTA, LV_SCR_LOAD_ANIM_NONE);
    release_group(G_APP);
    ota_ui_idle();
    ota_btn_cb(NULL);
}

bool app_leaving_for_good(void)
{
    if (!display_is_on()) return false;
    if (s_next == SCR_DETAIL && d_return == SCR_APP) return false;
    if (s_next == SCR_CALL && c_return == SCR_APP) return false;
    return true;
}

void app_close(void)
{
    if (s_cur == SCR_APP) show(menu_parent(SCR_APP), LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

bool app_is_open(const app_t *a) { return s_cur == SCR_APP && s_app_cur == a; }

void app_post_event(const app_t *a, int code)
{
    int i = app_index(a);
    if (i < 0) return;
    app_post_simple(EV_APP, (int32_t)((i << 16) | (code & 0xFFFF)));
}

bool ui_on_app_event(int32_t ev)
{
    int i = (int)(ev >> 16), code = (int)(ev & 0xFFFF);
    if (i < 0 || i >= g_app_count || !g_apps[i]->on_event) return false;
    const app_t *a = g_apps[i];
    if (!a->on_event(code)) return false;
    /* l'app demande à être montrée (alarme) : écran d'origine = cadran
     * si on vient d'ailleurs que de l'app elle-même */
    if (!app_is_open(a)) {
        if (!display_is_on()) show(SCR_WATCH, LV_SCR_LOAD_ANIM_NONE);
        app_open(a);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Appui long sur le cadran : choix du cadran (Affichage)               */
/* ------------------------------------------------------------------ */
static void watch_long_cb(lv_event_t *ev)
{
    if (s_cur != SCR_WATCH) return;
    power_vibrate(VIB_SHORT);
    nav_open(SCR_SET_DISP, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */
void ui_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, C_ACC, lv_color_hex(0x6B7280), true, &font_text_16);
    lv_display_set_theme(disp, th);
    i18n_set(settings_get()->lang);

    build_watch();
    build_list();
    build_detail();
    build_call();
    build_toast();
    build_banner();
    build_power();
    build_pair();
    build_about();
    show(SCR_WATCH, LV_SCR_LOAD_ANIM_NONE);
}

static void release_menu(void)
{
    if (!s_scr[SCR_MENU] || is_menu_group(s_cur)) return;
    static const scr_id_t grp[] = { SCR_MENU, SCR_SET_DISP, SCR_SET_ALERT, SCR_SET_BT, SCR_OTA };
    for (size_t i = 0; i < sizeof(grp) / sizeof(grp[0]); i++) {
        if (s_scr[grp[i]]) { scr_delete(s_scr[grp[i]]); s_scr[grp[i]] = NULL; }
    }
    /* plus aucun pointeur vers des objets détruits */
    m_slider = m_sw_vib = m_sw_bt = m_sw_tap = m_sw_nwake = m_bm_tmo = m_bm_face = m_bm_lang = NULL;
    m_bt_info = m_confirm = NULL;
    o_info = o_bar = o_status = o_btn = o_pw = o_prev = o_prev_modal = NULL;
    memset(m_sum, 0, sizeof(m_sum));
}

static void release_media(void)
{
    if (!s_scr[SCR_MEDIA] || s_cur == SCR_MEDIA) return;
    if (mu_timer) { lv_timer_delete(mu_timer); mu_timer = NULL; }
    scr_delete(s_scr[SCR_MEDIA]);
    s_scr[SCR_MEDIA] = NULL;
    mu_box = mu_empty = mu_player = mu_title = mu_artist = mu_album = NULL;
    mu_bar = mu_pos = mu_dur = mu_play_lbl = mu_vol = NULL;
    memset(mu_btn, 0, sizeof(mu_btn));
    mu_seq = UINT32_MAX;
}

static void release_group(int g)
{
    crashlog_mark('R', g, s_cur);
    switch (g) {
    case G_MENU:     release_menu(); break;
    case G_MEDIA:    release_media(); break;
    case G_LAUNCHER: release_launcher(); break;
    case G_QUICK:    release_quick(); break;
    case G_APP:      release_app(); break;
    }
}

void ui_release_idle(void)
{
    banner_hide(false);
    /* l'app reste construite écran éteint : on lui dit qu'elle n'est plus
     * visible (Wi-Fi, timers d'affichage…) ; ui_on_wake() la réveille */
    if (s_cur == SCR_APP && s_app_cur && s_app_cur->on_hide) s_app_cur->on_hide();
    /* une animation de changement d'écran en cours garde l'ancien écran
     * affiché : on la termine avant de libérer quoi que ce soit */
    scr_anim_settle();
    if (s_scr[s_cur] && lv_screen_active() != s_scr[s_cur]) lv_screen_load(s_scr[s_cur]);
    for (int g = G_MENU; g <= G_APP; g++) release_group(g);
}

/* extinction et réveil : on reste sur l'app ouverte pour la retrouver au
 * réveil (y compris si l'on regardait une notif ou un appel ouvert depuis
 * elle), sinon retour au cadran */
void ui_go_home_or_app(void)
{
    if (s_cur == SCR_APP) return;
    if (s_scr[SCR_APP] && s_app_cur &&
        ((s_cur == SCR_DETAIL && d_return == SCR_APP) || (s_cur == SCR_CALL && c_return == SCR_APP))) {
        show(SCR_APP, LV_SCR_LOAD_ANIM_NONE);
        return;
    }
    ui_go_home();
}

void ui_go_home(void)
{
    if (s_cur != SCR_WATCH) show(SCR_WATCH, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

bool ui_is_home(void) { return s_cur == SCR_WATCH; }

uint32_t ui_timeout_ms(void)
{
    uint32_t base = settings_get()->screen_timeout_s * 1000u;
    if (ota_active()) return 10u * 60 * 1000;   /* écran allumé pendant la mise à jour */
    switch (s_cur) {
    case SCR_APP:
        if (s_app_cur && s_app_cur->keep_screen_on && s_app_cur->keep_screen_on()) return UINT32_MAX;
        if (s_app_cur && s_app_cur->screen_timeout_ms) return s_app_cur->screen_timeout_ms;
        return base;
    case SCR_LAUNCHER:
    case SCR_QUICK:  return base > 10000 ? base : 10000;
    case SCR_DETAIL: return base > 15000 ? base : 15000;
    case SCR_CALL:   return 45000;
    case SCR_MENU:   return base > 20000 ? base : 20000;
    case SCR_POWER:  return 120000;
    case SCR_PAIR:   return 90000;
    case SCR_ABOUT:  return 30000;
    default:         return base;
    }
}

void ui_on_wake(void)
{
    if (s_cur == SCR_APP && s_app_cur && s_app_cur->on_show) s_app_cur->on_show();
    if (s_cur == SCR_WATCH) { watch_update_time(true); watch_refresh_status(); }
    if (s_cur == SCR_POWER) power_refresh();
}

bool ui_on_notif(notif_entry_t *e, bool alert)
{
    crashlog_mark('N', alert, s_cur);
    l_dirty = true;
    if (s_cur == SCR_LIST) list_rebuild();
    watch_refresh_status();
    if (s_cur == SCR_DETAIL && e->hash == d_hash) detail_fill(e);
    if (!alert) return false;

    /* Ne pas déranger : rien (ni vibration ni écran), la notif attend dans
     * la liste et le double tap la montre. Les appels passent toujours. */
    if (settings_get()->dnd && e->n.category != ANCS_CAT_INCOMING_CALL) {
        if (!display_is_on()) s_pending_hash = e->hash;
        return false;
    }

    /* Mode discret : écran éteint → on vibre seulement ; le bouton affichera
     * cette notif. Les appels allument toujours (décrocher/refuser). */
    if (!display_is_on() && !settings_get()->notif_wake
        && e->n.category != ANCS_CAT_INCOMING_CALL) {
        s_pending_hash = e->hash;
        if (settings_get()->vibrate && !(e->n.flags & ANCS_FLAG_SILENT)) power_vibrate(VIB_NOTIF);
        return false;
    }

    bool vib = settings_get()->vibrate && !(e->n.flags & ANCS_FLAG_SILENT);
    if (e->n.category == ANCS_CAT_INCOMING_CALL) {
        banner_hide(false);
        if (s_cur != SCR_CALL) c_return = (s_cur == SCR_APP || s_cur == SCR_LIST || s_cur == SCR_MEDIA) ? s_cur : SCR_WATCH;
        c_uid = e->n.uid;
        c_uid_valid = e->uid_valid;
        lv_label_set_text(c_who, e->n.title[0] ? e->n.title : _("Inconnu"));
        show(SCR_CALL, LV_SCR_LOAD_ANIM_OVER_TOP);
        if (vib) power_vibrate(VIB_CALL);
    } else if (s_cur != SCR_WATCH) {
        /* dans une app, un menu, la liste… : bannière par-dessus, on ne
         * quitte pas l'écran en cours ; un toucher ouvre le détail */
        banner_show(e);
        if (vib) power_vibrate(VIB_NOTIF);
    } else {
        d_return = SCR_WATCH;
        detail_fill(e);
        show(SCR_DETAIL, LV_SCR_LOAD_ANIM_OVER_TOP);
        if (vib) power_vibrate(VIB_NOTIF);
    }
    return true;
}

bool ui_open_pending(void)
{
    if (!s_pending_hash) return false;
    notif_entry_t *e = ns_find_hash(s_pending_hash);
    s_pending_hash = 0;
    if (!e) return false;              /* supprimée entre-temps sur l'iPhone */
    d_return = s_cur == SCR_APP ? SCR_APP : SCR_WATCH;   /* puis retour à l'app en cours */
    detail_fill(e);
    show(SCR_DETAIL, LV_SCR_LOAD_ANIM_NONE);
    return true;
}

void ui_on_notif_removed(uint32_t uid)
{
    crashlog_mark('X', 0, s_cur);
    notif_entry_t *e = ns_find_uid(uid);
    uint32_t h = e ? e->hash : 0;
    bool was_call = (s_cur == SCR_CALL && c_uid_valid && c_uid == uid);
    if (e) {
        if (e->hash == s_pending_hash) s_pending_hash = 0;
        ns_remove_entry(e);
        l_dirty = true;
        if (s_cur == SCR_LIST) list_rebuild();
        watch_refresh_status();
    }
    if (was_call) {
        c_uid_valid = false;
        show(c_return, LV_SCR_LOAD_ANIM_FADE_OUT);
    } else if (s_cur == SCR_DETAIL && h && h == d_hash) {
        show(d_return, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    }
}

void ui_on_ble_state(int st)
{
    crashlog_mark('B', st, s_cur);
    if (s_cur == SCR_SET_BT) menu_bt_refresh();
    else if (s_cur == SCR_MENU) menu_refresh_summaries();
    if (st != BLE_ST_READY && st != BLE_ST_SECURED && st != BLE_ST_CONNECTED) {
        ns_invalidate_uids();
        c_uid_valid = false;
        if (s_cur == SCR_CALL) show(c_return, LV_SCR_LOAD_ANIM_NONE);
    }
    watch_refresh_status();
    if (s_cur == SCR_QUICK) quick_refresh();
    /* l'interrupteur suit l'état réel de la pile (page construite à la demande) */
    if (m_sw_bt) lv_obj_set_state(m_sw_bt, LV_STATE_CHECKED, ble_running());
}

void ui_on_power(void)
{
    watch_refresh_status();
    if (s_cur == SCR_QUICK) quick_refresh();
}

void ui_on_time_changed(void) { watch_update_time(true); }

void ui_show_passkey(uint32_t pk)
{
    lv_label_set_text_fmt(pa_code, "%06lu", (unsigned long)pk);
    lv_obj_set_style_text_color(pa_code, C_FG, 0);
    lv_label_set_text(pa_hint, _("Saisir ce code sur l'iPhone"));
    lv_timer_pause(pa_timer);
    if (s_cur != SCR_PAIR) s_origin[SCR_PAIR] = s_cur;
    show(SCR_PAIR, LV_SCR_LOAD_ANIM_NONE);
}

void ui_pairing_done(int status)
{
    if (s_cur != SCR_PAIR) return;
    if (status == 0) {
        lv_obj_set_style_text_color(pa_code, C_OK, 0);
        lv_label_set_text(pa_hint, with_sym(LV_SYMBOL_OK, _("Appairage réussi")));
        power_vibrate(VIB_SHORT);
    } else {
        lv_obj_set_style_text_color(pa_code, C_BAD, 0);
        lv_label_set_text(pa_hint, with_sym(LV_SYMBOL_CLOSE, _("Échec de l'appairage")));
    }
    lv_timer_reset(pa_timer);
    lv_timer_resume(pa_timer);
}
