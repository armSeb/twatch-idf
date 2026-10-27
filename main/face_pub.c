#include "face_pub.h"
#include <math.h>
#include <stdio.h>
#include "fonts/fonts.h"

/* ---------- palette « pub irlandais » ---------- */
#define C_WOOD_TOP   lv_color_hex(0x5E3721)   /* acajou */
#define C_WOOD_BOT   lv_color_hex(0x3A2013)
#define C_GRAIN_D    lv_color_hex(0x2A170D)   /* veines sombres */
#define C_GRAIN_L    lv_color_hex(0x7A4A2C)   /* veines claires */
#define C_GREEN_TOP  lv_color_hex(0x1E5A3C)   /* vert bouteille */
#define C_GREEN_BOT  lv_color_hex(0x0F3522)
#define C_SUB        lv_color_hex(0x0C2B1B)   /* compteurs creusés */
#define C_BRASS      lv_color_hex(0xC9A24A)   /* laiton */
#define C_BRASS_D    lv_color_hex(0x8C6A2A)
#define C_CREAM      lv_color_hex(0xEFE2C2)   /* impressions crème */
#define C_DARK       lv_color_hex(0x08170F)
#define C_RED        lv_color_hex(0xA8322D)
#define C_DIM        lv_color_hex(0x5E7A63)

#define CX 120
#define CY 120
#define DIAL_IN   12          /* marge du cadran vert dans le cadre bois */
#define DIAL_R    30          /* rayon des angles du cadran */
#define TRACK_H   98          /* demi-côté de la piste des minutes */
#define TRACK_P   6.0f        /* exposant du « squircle » de la piste */

/* complications */
#define PR_X 120
#define PR_Y 66
#define SS_X 120
#define SS_Y 174
#define SS_D 54
#define DT_X 176
#define DT_Y 120
#define ST_X 62
#define ST_Y 120

static lv_obj_t *s_root;
static lv_obj_t *s_hour, *s_hour_in, *s_min, *s_min_in, *s_sec, *s_pr_hand;
static lv_obj_t *s_date, *s_pr_label, *s_ble, *s_badge, *s_badge_txt;
static lv_point_precise_t s_pt_hour[2], s_pt_hour_in[2], s_pt_min[2], s_pt_min_in[2], s_pt_sec[2], s_pt_pr[2];
static int s_last_min = -1, s_last_sec = -1, s_last_pct = -1;

/* ------------------------------------------------------------------ */
static void bubble(lv_obj_t *o)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    bubble(o);
    return o;
}

static lv_obj_t *disc(lv_obj_t *p, int cx, int cy, int d, lv_color_t bg, int border, lv_color_t bc)
{
    lv_obj_t *o = box(p, cx - d / 2, cy - d / 2, d, d, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (border) {
        lv_obj_set_style_border_width(o, border, 0);
        lv_obj_set_style_border_color(o, bc, 0);
    }
    return o;
}

static lv_obj_t *scale(lv_obj_t *p, int cx, int cy, int d, int rot, int range_deg, int ticks, int major_every,
                       int major_len, int major_w, int minor_len, lv_color_t major_c, lv_color_t minor_c)
{
    lv_obj_t *s = lv_scale_create(p);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, d, d);
    lv_obj_set_pos(s, cx - d / 2, cy - d / 2);
    lv_scale_set_mode(s, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_rotation(s, rot);
    lv_scale_set_angle_range(s, range_deg);
    lv_scale_set_range(s, 0, ticks - 1);
    lv_scale_set_total_tick_count(s, ticks);
    lv_scale_set_major_tick_every(s, major_every);
    lv_scale_set_label_show(s, false);
    lv_obj_set_style_length(s, major_len, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s, major_w, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s, major_c, LV_PART_INDICATOR);
    lv_obj_set_style_length(s, minor_len, LV_PART_ITEMS);
    lv_obj_set_style_line_width(s, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s, minor_c, LV_PART_ITEMS);
    lv_obj_set_style_arc_width(s, 0, LV_PART_MAIN);
    bubble(s);
    return s;
}

static lv_obj_t *hand(lv_obj_t *p, int area, int w, lv_color_t c, lv_point_precise_t *pts)
{
    lv_obj_t *l = lv_line_create(p);
    lv_obj_remove_style_all(l);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_size(l, area, area);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    pts[0].x = pts[1].x = pts[0].y = pts[1].y = 0;
    lv_line_set_points(l, pts, 2);
    bubble(l);
    return l;
}

static void place(lv_obj_t *l, lv_point_precise_t *pts, float cx, float cy, float deg, float from, float to)
{
    float r = deg * (float)M_PI / 180.0f;
    float sx = sinf(r), sy = -cosf(r);
    pts[0].x = (lv_value_precise_t)(cx + from * sx);
    pts[0].y = (lv_value_precise_t)(cy + from * sy);
    pts[1].x = (lv_value_precise_t)(cx + to * sx);
    pts[1].y = (lv_value_precise_t)(cy + to * sy);
    lv_line_set_points(l, pts, 2);
}

static lv_obj_t *text(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *t)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    bubble(l);
    return l;
}

/* ------------------------------------------------------------------ */
/* Décor dessiné                                                        */
/* ------------------------------------------------------------------ */
static void draw_line(lv_layer_t *layer, float x1, float y1, float x2, float y2, int w, lv_color_t c,
                      lv_opa_t opa, bool round)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = (lv_value_precise_t)x1; d.p1.y = (lv_value_precise_t)y1;
    d.p2.x = (lv_value_precise_t)x2; d.p2.y = (lv_value_precise_t)y2;
    d.width = w;
    d.color = c;
    d.opa = opa;
    d.round_start = d.round_end = round;
    lv_draw_line(layer, &d);
}

static void draw_disc(lv_layer_t *layer, float cx, float cy, int r, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    lv_area_t a = { (int32_t)cx - r, (int32_t)cy - r, (int32_t)cx + r, (int32_t)cy + r };
    lv_draw_rect(layer, &d, &a);
}

/* Veinage du bois : bandes légèrement ondulées (segments), visibles dans le
 * cadre autour du cadran. Pseudo-aléatoire déterministe. */
static void wood_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_area_t co;
    lv_obj_get_coords(o, &co);
    int ox = co.x1, oy = co.y1;
    uint32_t seed = 0x2B7E1516;
    for (int i = 0; i < 22; i++) {
        seed = seed * 1103515245u + 12345u;
        float y = 4 + i * 10.7f + (float)((seed >> 16) % 5);
        float amp = 1.0f + (float)((seed >> 8) % 3);
        float ph = (float)(seed % 628) / 100.0f;
        bool dark = (i % 3) != 1;
        lv_color_t c = dark ? C_GRAIN_D : C_GRAIN_L;
        lv_opa_t opa = dark ? LV_OPA_40 : LV_OPA_30;
        int w = (seed >> 4) % 3 == 0 ? 2 : 1;
        float px = 0, py = y + amp * sinf(ph);
        for (int x = 24; x <= 240; x += 24) {
            float ny = y + amp * sinf(ph + x / 38.0f);
            draw_line(layer, ox + px, oy + py, ox + x, oy + ny, w, c, opa, false);
            px = (float)x; py = ny;
        }
    }
}

/* point de la piste « squircle » à l'angle deg (horaire depuis 12 h), rayon k·h */
static void track_pt(float deg, float h, float *x, float *y, float *ux, float *uy)
{
    float r = deg * (float)M_PI / 180.0f;
    float sx = sinf(r), sy = -cosf(r);
    float n = powf(powf(fabsf(sx), TRACK_P) + powf(fabsf(sy), TRACK_P), 1.0f / TRACK_P);
    float rad = h / n;
    *x = CX + rad * sx; *y = CY + rad * sy;
    *ux = sx; *uy = sy;
}

static void track_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_area_t co;
    lv_obj_get_coords(o, &co);
    int ox = co.x1, oy = co.y1;

    for (int m = 0; m < 60; m++) {
        float x, y, ux, uy;
        track_pt(m * 6.0f, TRACK_H, &x, &y, &ux, &uy);
        x += ox; y += oy;
        if (m % 5) {
            draw_line(layer, x, y, x - 5 * ux, y - 5 * uy, 1, C_CREAM, LV_OPA_70, false);
            continue;
        }
        /* index laiton : barre épaisse + filet sombre central */
        float len = 15;
        if (m == 0) {   /* double index à 12 h */
            for (int k = -1; k <= 1; k += 2) {
                draw_line(layer, x + k * 5, y + 1, x + k * 5, y + len, 5, C_BRASS, LV_OPA_COVER, false);
                draw_line(layer, x + k * 5, y + 3, x + k * 5, y + len - 2, 1, C_BRASS_D, LV_OPA_COVER, false);
            }
            continue;
        }
        draw_line(layer, x, y, x - len * ux, y - len * uy, 6, C_BRASS, LV_OPA_COVER, false);
        draw_line(layer, x - 2 * ux, y - 2 * uy, x - (len - 2) * ux, y - (len - 2) * uy, 1, C_BRASS_D,
                  LV_OPA_COVER, false);
    }

    /* trèfle laiton entre le centre et la petite seconde */
    float tx = ox + CX, ty = oy + CY + 17;
    draw_line(layer, tx, ty, tx + 3, ty + 9, 2, C_BRASS, LV_OPA_COVER, true);
    draw_disc(layer, tx, ty - 4, 3, C_BRASS);
    draw_disc(layer, tx - 4, ty + 1, 3, C_BRASS);
    draw_disc(layer, tx + 4, ty + 1, 3, C_BRASS);
}

/* ------------------------------------------------------------------ */
lv_obj_t *face_pub_create(lv_obj_t *parent)
{
    s_root = box(parent, 0, 0, 240, 240, 0);
    lv_obj_set_style_bg_color(s_root, C_WOOD_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_root, C_WOOD_BOT, 0);
    lv_obj_set_style_bg_grad_dir(s_root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_root, wood_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    /* cadran vert bouteille, filet laiton, liseré crème intérieur */
    int ds = 240 - 2 * DIAL_IN;
    lv_obj_t *dial = box(s_root, DIAL_IN, DIAL_IN, ds, ds, DIAL_R);
    lv_obj_set_style_bg_color(dial, C_GREEN_TOP, 0);
    lv_obj_set_style_bg_grad_color(dial, C_GREEN_BOT, 0);
    lv_obj_set_style_bg_grad_dir(dial, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dial, 3, 0);
    lv_obj_set_style_border_color(dial, C_BRASS, 0);
    lv_obj_t *pin = box(s_root, DIAL_IN + 6, DIAL_IN + 6, ds - 12, ds - 12, DIAL_R - 6);
    lv_obj_set_style_border_width(pin, 1, 0);
    lv_obj_set_style_border_color(pin, C_CREAM, 0);
    lv_obj_set_style_border_opa(pin, LV_OPA_40, 0);

    /* piste des minutes, index et trèfle (dessinés) */
    lv_obj_t *track = box(s_root, 0, 0, 240, 240, 0);
    lv_obj_add_event_cb(track, track_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    /* 12 h : réserve de marche */
    scale(s_root, PR_X, PR_Y, 60, 210, 120, 11, 5, 6, 2, 3, C_BRASS, C_CREAM);
    s_pr_label = text(s_root, &font_small_11, C_CREAM, "RÉSERVE");
    lv_obj_align(s_pr_label, LV_ALIGN_TOP_MID, 0, PR_Y + 5);
    lv_obj_t *pr0 = text(s_root, &font_small_11, C_CREAM, "0");
    lv_obj_set_pos(pr0, PR_X - 30, PR_Y - 10);
    lv_obj_t *pr1 = text(s_root, &font_small_11, C_CREAM, "100");
    lv_obj_set_pos(pr1, PR_X + 21, PR_Y - 10);

    /* inscription */
    lv_obj_t *brand = text(s_root, &font_small_11, C_BRASS, "SLÁINTE");
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, PR_Y + 31);

    /* 6 h : petite seconde */
    lv_obj_t *ssd = disc(s_root, SS_X, SS_Y, SS_D, C_SUB, 1, C_BRASS);
    scale(ssd, SS_D / 2, SS_D / 2, SS_D - 6, 270, 360, 61, 15, 5, 2, 2, C_BRASS, C_CREAM);
    s_sec = hand(ssd, SS_D, 2, C_RED, s_pt_sec);

    /* 3 h : guichet jour + date */
    lv_obj_t *win = box(s_root, DT_X - 27, DT_Y - 10, 54, 20, 3);
    lv_obj_set_style_bg_color(win, C_CREAM, 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(win, 2, 0);
    lv_obj_set_style_border_color(win, C_BRASS, 0);
    s_date = text(win, &font_small_11, C_DARK, "");
    lv_obj_center(s_date);

    /* 9 h : Bluetooth + notifications */
    lv_obj_t *st = disc(s_root, ST_X, ST_Y, 38, C_SUB, 1, C_BRASS);
    s_ble = text(st, &font_text_16, C_DIM, LV_SYMBOL_BLUETOOTH);
    lv_obj_center(s_ble);
    s_badge = disc(s_root, ST_X + 14, ST_Y - 15, 17, C_RED, 1, C_CREAM);
    s_badge_txt = text(s_badge, &font_small_11, C_CREAM, "");
    lv_obj_center(s_badge_txt);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    /* aiguilles laiton à filet sombre */
    s_pr_hand = hand(s_root, 240, 2, C_BRASS, s_pt_pr);
    disc(s_root, PR_X, PR_Y, 6, C_BRASS, 0, C_BRASS);
    s_hour = hand(s_root, 240, 8, C_BRASS, s_pt_hour);
    s_hour_in = hand(s_root, 240, 2, C_DARK, s_pt_hour_in);
    s_min = hand(s_root, 240, 6, C_BRASS, s_pt_min);
    s_min_in = hand(s_root, 240, 2, C_DARK, s_pt_min_in);
    disc(s_root, CX, CY, 14, C_BRASS, 1, C_BRASS_D);
    disc(s_root, CX, CY, 4, C_DARK, 0, C_DARK);
    disc(ssd, SS_D / 2, SS_D / 2, 5, C_RED, 0, C_RED);

    s_last_min = s_last_sec = s_last_pct = -1;
    return s_root;
}

void face_pub_set_time(const struct tm *tm, bool force)
{
    static const char *const JOURS[] = { "DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM" };
    if (force || tm->tm_min != s_last_min) {
        float m = tm->tm_min + tm->tm_sec / 60.0f;
        float h = (tm->tm_hour % 12) + m / 60.0f;
        place(s_hour, s_pt_hour, CX, CY, h * 30.0f, -12, 56);
        place(s_hour_in, s_pt_hour_in, CX, CY, h * 30.0f, 14, 48);
        place(s_min, s_pt_min, CX, CY, m * 6.0f, -16, 90);
        place(s_min_in, s_pt_min_in, CX, CY, m * 6.0f, 16, 82);
        char b[16];
        snprintf(b, sizeof(b), "%s %d", JOURS[tm->tm_wday % 7], tm->tm_mday);
        lv_label_set_text(s_date, b);
        s_last_min = tm->tm_min;
    }
    if (force || tm->tm_sec != s_last_sec) {
        place(s_sec, s_pt_sec, SS_D / 2, SS_D / 2, tm->tm_sec * 6.0f, -6, 21);
        s_last_sec = tm->tm_sec;
    }
}

void face_pub_set_status(int pct, bool charging, bool ble_ok, bool ble_on, int notifs)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct != s_last_pct) {
        place(s_pr_hand, s_pt_pr, PR_X, PR_Y, -60.0f + pct * 1.2f, -4, 23);
        s_last_pct = pct;
    }
    lv_label_set_text(s_pr_label, charging ? "CHARGE" : "RÉSERVE");
    lv_obj_set_style_text_color(s_pr_label, (pct <= 15 && !charging) ? C_RED : C_CREAM, 0);

    lv_obj_set_style_text_color(s_ble, ble_ok ? C_BRASS : (ble_on ? C_DIM : lv_color_hex(0x2E4A38)), 0);
    if (notifs > 0) {
        lv_label_set_text_fmt(s_badge_txt, "%d", notifs > 9 ? 9 : notifs);
        lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    }
}
