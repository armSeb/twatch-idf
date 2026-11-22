/*
 * app_mouse.c — « Souris » : la montre pilote un ordinateur en Bluetooth.
 *
 * En ouvrant l'app, la montre quitte l'iPhone et devient « T-Watch Souris »
 * (ble_hid.c) ; en la quittant, elle redevient la montre et l'iPhone se
 * reconnecte. Trois pages, en touchant 1/3 en haut à droite :
 *
 *   (pages : 1/3 en haut à droite)
 *   Pavé    glisser = pointeur (accélération selon la vitesse), toucher =
 *           clic, appui long = clic droit, toucher puis aussitôt glisser =
 *           glisser-déposer, bande de droite = défilement
 *   Diapos  ◀ ▶ en grand (flèches du clavier), Début (F5), Noir (B), Fin (Échap)
 *   Média   piste précédente / lecture / suivante, volume (répété si on garde
 *           le doigt), muet
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "power.h"
#include "ble.h"
#include "ble_hid.h"
#include "esp_timer.h"

extern const app_t app_mouse;

typedef enum { PG_PAD, PG_SLIDES, PG_MEDIA, PG_N } page_t;
static page_t s_page;
static lv_obj_t *v_title, *v_body, *v_status, *v_hint, *v_pagebtn;
static lv_timer_t *v_tick;
static bool s_ble_was, s_active;

/* ------------------------------------------------------------------ pavé */
#define SCROLL_X 200                     /* bande de défilement à droite */
#define TAP_MOVE 10
static struct {
    bool down, scroll, drag, moved_far, long_done;
    int64_t t_down, t_last_tap, t_sent;
    float fx, fy, fw;
    int ax, ay, aw;                      /* à envoyer */
    int dist;
} P;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void flush(bool force)
{
    if (!P.ax && !P.ay && !P.aw) return;
    if (!force && now_ms() - P.t_sent < 12) return;   /* ~80 rapports/s au plus */
    if (hid_mouse(P.drag ? HID_BTN_LEFT : 0, P.ax, P.ay, P.aw)) { P.ax = P.ay = P.aw = 0; P.t_sent = now_ms(); }
    else P.ax = P.ay = P.aw = 0;                       /* pas connecté : on n'accumule pas */
}

static void pad_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (c == LV_EVENT_PRESSED) {
        memset(&P.fx, 0, sizeof P.fx * 3);
        P.down = true;
        P.scroll = p.x >= SCROLL_X;
        P.moved_far = P.long_done = false;
        P.dist = 0;
        P.t_down = now_ms();
        /* toucher puis aussitôt appuyer : glisser-déposer */
        P.drag = !P.scroll && P.t_down - P.t_last_tap < 280;
        if (P.drag) hid_mouse(HID_BTN_LEFT, 0, 0, 0);
    } else if (c == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(in, &v);
        if (!v.x && !v.y) return;
        P.dist += abs(v.x) + abs(v.y);
        if (P.dist > TAP_MOVE) P.moved_far = true;
        if (P.scroll) {
            P.fw += v.y / 9.0f;                            /* doigt vers le bas : on remonte */
            int w = (int)P.fw;
            P.fw -= w;
            P.aw += w;
        } else {
            float sp = sqrtf((float)(v.x * v.x + v.y * v.y));
            float g = 1.4f + sp * 0.18f;                   /* accélération selon la vitesse */
            if (g > 7) g = 7;
            P.fx += v.x * g; P.fy += v.y * g;
            int dx = (int)P.fx, dy = (int)P.fy;
            P.fx -= dx; P.fy -= dy;
            P.ax += dx; P.ay += dy;
        }
        flush(false);
    } else if (c == LV_EVENT_LONG_PRESSED) {
        if (!P.moved_far && !P.scroll && !P.drag) {        /* clic droit */
            hid_mouse(HID_BTN_RIGHT, 0, 0, 0);
            hid_mouse(0, 0, 0, 0);
            power_vibrate(VIB_SHORT);
            P.long_done = true;
        }
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        flush(true);
        if (P.drag) hid_mouse(0, 0, 0, 0);
        else if (!P.scroll && !P.moved_far && !P.long_done && now_ms() - P.t_down < 350) {
            hid_mouse(HID_BTN_LEFT, 0, 0, 0);              /* clic */
            hid_mouse(0, 0, 0, 0);
            P.t_last_tap = now_ms();
            return;
        }
        P.drag = false;
        P.down = false;
        P.t_last_tap = 0;
    }
}

/* ------------------------------------------------------------------ touches */
typedef struct { uint8_t kind, mod; uint16_t code; } act_t;   /* kind 1 clavier, 2 multimédia */

static void act_event(lv_event_t *e)
{
    const act_t *a = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    bool vol = a->kind == 2 && (a->code == HID_CC_VOL_UP || a->code == HID_CC_VOL_DOWN);
    if (c == LV_EVENT_LONG_PRESSED_REPEAT && !vol) return;
    bool ok = a->kind == 1 ? hid_key(a->mod, (uint8_t)a->code) : hid_consumer(a->code);
    if (ok && c == LV_EVENT_PRESSED) power_vibrate(VIB_SHORT);
}

static lv_obj_t *act_btn(const char *t, lv_color_t col, const act_t *a, int x, int y, int w, int h, const lv_font_t *f)
{
    lv_obj_t *b = ui_button(v_body, t, col, NULL, NULL);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l && f) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_add_event_cb(b, act_event, LV_EVENT_PRESSED, (void *)a);
    lv_obj_add_event_cb(b, act_event, LV_EVENT_LONG_PRESSED_REPEAT, (void *)a);
    return b;
}

/* ------------------------------------------------------------------ vue */
static void build(void);
static void build_async(void *p) { build(); }

static void page_cb(lv_event_t *e)
{
    s_page = (page_t)((s_page + 1) % PG_N);
    lv_async_call(build_async, NULL);
}

static void status_refresh(void)
{
    if (!v_status) return;
    bool rdy = hid_ready(), con = hid_connected();
    lv_obj_set_style_bg_color(v_status, rdy ? lv_color_hex(0x22C55E) : con ? lv_color_hex(0xF59E0B) : lv_color_hex(0x6B7280), 0);
    if (v_hint) {
        if (rdy) lv_obj_add_flag(v_hint, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_remove_flag(v_hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(v_hint, con ? _("Connexion…") :
                              _("Sur l'ordinateur : Bluetooth > Ajouter un appareil > « T-Watch Souris ». Ensuite il se reconnecte tout seul."));
        }
    }
}

static void build(void)
{
    if (!v_body) return;
    lv_obj_clean(v_body);
    v_hint = NULL;
    static const char *const TITLE[PG_N] = { N_("Pavé"), N_("Diapos"), N_("Média") };
    lv_label_set_text(v_title, _(TITLE[s_page]));
    if (v_pagebtn) lv_label_set_text_fmt(lv_obj_get_child(v_pagebtn, 0), "%d/%d", s_page + 1, PG_N);
    lv_color_t blue = lv_color_hex(0x2563EB), grey = lv_color_hex(0x374151), red = lv_color_hex(0xB91C1C);
    switch (s_page) {
    case PG_PAD: {
        lv_obj_t *pad = lv_obj_create(v_body);
        lv_obj_remove_style_all(pad);
        lv_obj_set_size(pad, 240, 200);
        lv_obj_set_style_bg_color(pad, lv_color_hex(0x0B0F17), 0);
        lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
        lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(pad, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_remove_flag(pad, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(pad, pad_event, LV_EVENT_ALL, NULL);
        lv_obj_t *strip = lv_obj_create(pad);                  /* bande de défilement */
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, 240 - SCROLL_X, 200);
        lv_obj_set_pos(strip, SCROLL_X, 0);
        lv_obj_set_style_bg_color(strip, lv_color_hex(0x1A2233), 0);
        lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
        lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *u = ui_label(strip, &font_title_20, C_DIM, LV_SYMBOL_UP);
        lv_obj_align(u, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_t *d = ui_label(strip, &font_title_20, C_DIM, LV_SYMBOL_DOWN);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, 0, -12);
        v_hint = ui_label(pad, &font_text_16, C_DIM, "");
        lv_obj_set_width(v_hint, 186);
        lv_label_set_long_mode(v_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(v_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(v_hint, LV_ALIGN_CENTER, -18, 0);
        break;
    }
    case PG_SLIDES: {
        static const act_t PREV = { 1, 0, HID_KEY_LEFT }, NEXT = { 1, 0, HID_KEY_RIGHT },
                           START = { 1, 0, HID_KEY_F5 }, BLACK = { 1, 0, HID_KEY_B }, END = { 1, 0, HID_KEY_ESC };
        act_btn(LV_SYMBOL_LEFT, grey, &PREV, 6, 4, 82, 128, &font_title_20);
        act_btn(LV_SYMBOL_RIGHT, blue, &NEXT, 94, 4, 140, 128, &font_title_20);
        act_btn(_("Début"), grey, &START, 6, 140, 72, 52, NULL);
        act_btn(_("Noir"), grey, &BLACK, 84, 140, 72, 52, NULL);
        act_btn(_("Fin"), red, &END, 162, 140, 72, 52, NULL);
        break;
    }
    case PG_MEDIA: {
        static const act_t A[6] = { { 2, 0, HID_CC_PREV }, { 2, 0, HID_CC_PLAY }, { 2, 0, HID_CC_NEXT },
                                    { 2, 0, HID_CC_VOL_DOWN }, { 2, 0, HID_CC_MUTE }, { 2, 0, HID_CC_VOL_UP } };
        static const char *const S[6] = { LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT,
                                          LV_SYMBOL_VOLUME_MID, LV_SYMBOL_MUTE, LV_SYMBOL_VOLUME_MAX };
        for (int i = 0; i < 6; i++)
            act_btn(S[i], i == 1 ? blue : grey, &A[i], 6 + (i % 3) * 76, 6 + (i / 3) * 96, 72, 90, &font_title_20);
        break;
    }
    default: break;
    }
    status_refresh();
}

static void tick_cb(lv_timer_t *t) { status_refresh(); }

static lv_obj_t *hdr_btn(lv_obj_t *scr, const char *sym, int x, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 44, 34);
    lv_obj_set_pos(b, x, 3);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C_CARD, 0);
    lv_obj_t *l = ui_label(b, &font_title_20, C_FG, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void create(lv_obj_t *scr)
{
    int y = ui_header(scr, "");
    lv_obj_t *hdr = lv_obj_get_child(scr, -1);
    v_title = lv_obj_get_child(hdr, 1);
    v_status = lv_obj_create(scr);                         /* pastille : gris, orange, vert */
    lv_obj_remove_style_all(v_status);
    lv_obj_set_size(v_status, 12, 12);
    lv_obj_set_pos(v_status, 160, 14);
    lv_obj_set_style_radius(v_status, 6, 0);
    lv_obj_set_style_bg_opa(v_status, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v_status, LV_OBJ_FLAG_CLICKABLE);
    v_pagebtn = hdr_btn(scr, "1/3", 184, page_cb);       /* page suivante */
    lv_obj_set_width(v_pagebtn, 52);
    v_body = ui_container(scr);
    lv_obj_set_size(v_body, 240, 240 - y);
    lv_obj_align(v_body, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_remove_flag(v_body, LV_OBJ_FLAG_SCROLLABLE);
    s_page = PG_PAD;
    build();
    v_tick = lv_timer_create(tick_cb, 300, NULL);
}

static void destroy(void)
{
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    v_title = v_body = v_status = v_hint = v_pagebtn = NULL;
}

static void on_show(void)
{
    if (v_tick) lv_timer_resume(v_tick);
    if (!s_active) {                                       /* la montre devient une souris */
        s_ble_was = ble_running();
        ble_stop();
        ble_set_hid_mode(true);
        ble_start();
        s_active = true;
    }
    lv_indev_t *in = lv_indev_get_next(NULL);              /* toucher lu toutes les 10 ms */
    while (in && lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) in = lv_indev_get_next(in);
    if (in && lv_indev_get_read_timer(in)) lv_timer_set_period(lv_indev_get_read_timer(in), 10);
}

static void on_hide(void)
{
    if (v_tick) lv_timer_pause(v_tick);
    lv_indev_t *in = lv_indev_get_next(NULL);
    while (in && lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) in = lv_indev_get_next(in);
    if (in && lv_indev_get_read_timer(in)) lv_timer_set_period(lv_indev_get_read_timer(in), LV_DEF_REFR_PERIOD);   /* valeur par défaut de LVGL */
    if (s_active) {                                        /* retour à la montre et à l'iPhone */
        ble_stop();
        ble_set_hid_mode(false);
        if (s_ble_was) ble_start();
        s_active = false;
    }
}

static bool keep_on(void) { return true; }
static bool no_interrupt(void) { return true; }

const app_t app_mouse = {
    .name = N_("Souris"),
    .icon = SYM_POINTER,
    .color = 0x7C3AED,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .keep_screen_on = keep_on,
    .no_interrupt = no_interrupt,
};
