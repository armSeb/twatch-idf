#include "face_classic.h"
#include <math.h>
#include <stdio.h>
#include "fonts/fonts.h"

/* ---------- palette « montre ancienne » ---------- */
#define C_DIAL_TOP   lv_color_hex(0xF2EADA)
#define C_DIAL_BOT   lv_color_hex(0xE2D5B8)
#define C_BEZEL      lv_color_hex(0xA58F62)   /* or vieilli */
#define C_SUBDIAL    lv_color_hex(0xE7DCC3)
#define C_PRINT      lv_color_hex(0x4E4130)   /* impressions du cadran */
#define C_INDEX      lv_color_hex(0x1C1C20)   /* index appliqués */
#define C_BLUED      lv_color_hex(0x23407E)   /* acier bleui */
#define C_LUME       lv_color_hex(0xF4ECD6)
#define C_WINDOW     lv_color_hex(0xFBF8F1)
#define C_BADGE      lv_color_hex(0xA3372A)
#define C_DIM        lv_color_hex(0x9A8E76)

#define CX 120
#define CY 120
#define DIAL_D 236

/* positions des complications (coordonnées écran) */
#define PR_X 120   /* réserve de marche */
#define PR_Y 70
#define SS_X 120   /* petite seconde */
#define SS_Y 170
#define SS_D 56
#define DT_X 176   /* guichet date */
#define DT_Y 120
#define ST_X 64    /* état */
#define ST_Y 120

static lv_obj_t *s_root;
static lv_obj_t *s_hour, *s_hour_lume, *s_min, *s_min_lume, *s_sec, *s_pr_hand;
static lv_obj_t *s_date, *s_pr_label, *s_ble, *s_badge, *s_badge_txt;
static lv_point_precise_t s_pt_hour[2], s_pt_hour_l[2], s_pt_min[2], s_pt_min_l[2], s_pt_sec[2], s_pt_pr[2];
static int s_last_min = -1, s_last_sec = -1, s_last_pct = -1;

/* ------------------------------------------------------------------ */
static void bubble(lv_obj_t *o)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *disc(lv_obj_t *p, int cx, int cy, int d, lv_color_t bg, int border, lv_color_t bc)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    if (border) {
        lv_obj_set_style_border_width(o, border, 0);
        lv_obj_set_style_border_color(o, bc, 0);
    }
    bubble(o);
    return o;
}

static lv_obj_t *ring(lv_obj_t *p, int cx, int cy, int d, int w, lv_color_t c)
{
    lv_obj_t *o = disc(p, cx, cy, d, c, w, c);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    return o;
}

/* Graduation ronde (lv_scale), sans libellés */
static lv_obj_t *scale(lv_obj_t *p, int cx, int cy, int d, int rot, int range_deg, int ticks, int major_every,
                       int major_len, int major_w, int minor_len, lv_color_t major_c, lv_color_t minor_c,
                       int arc_w)
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
    lv_obj_set_style_arc_width(s, arc_w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s, minor_c, LV_PART_MAIN);
    bubble(s);
    return s;
}

static lv_obj_t *hand(lv_obj_t *p, int area, int w, lv_color_t c, lv_point_precise_t *pts)
{
    lv_obj_t *l = lv_line_create(p);
    lv_obj_remove_style_all(l);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_size(l, area, area);   /* zone fixe (taille connue : pas de layout encore) */
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    pts[0].x = pts[1].x = 0;
    pts[0].y = pts[1].y = 0;
    lv_line_set_points(l, pts, 2);
    bubble(l);
    return l;
}

/* angle en degrés, sens horaire depuis 12 h ; aiguille de « from » à « to » px du pivot */
static void place(lv_obj_t *l, lv_point_precise_t *pts, float cx, float cy, float deg, float from, float to)
{
    float r = deg * (float)M_PI / 180.0f;
    float sx = sinf(r), cy_ = -cosf(r);
    pts[0].x = (lv_value_precise_t)(cx + from * sx);
    pts[0].y = (lv_value_precise_t)(cy + from * cy_);
    pts[1].x = (lv_value_precise_t)(cx + to * sx);
    pts[1].y = (lv_value_precise_t)(cy + to * cy_);
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
lv_obj_t *face_classic_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 240, 240);
    lv_obj_set_pos(s_root, 0, 0);
    bubble(s_root);

    /* boîtier + cadran ivoire légèrement patiné */
    lv_obj_t *dial = disc(s_root, CX, CY, DIAL_D, C_DIAL_TOP, 3, C_BEZEL);
    lv_obj_set_style_bg_grad_color(dial, C_DIAL_BOT, 0);
    lv_obj_set_style_bg_grad_dir(dial, LV_GRAD_DIR_VER, 0);

    /* chemin de fer des minutes + index appliqués */
    scale(s_root, CX, CY, 222, 270, 360, 61, 5, 14, 3, 5, C_INDEX, C_PRINT, 1);
    ring(s_root, CX, CY, 212, 1, C_PRINT);
    lv_obj_t *twelve = lv_obj_create(s_root);            /* index double à 12 h */
    lv_obj_remove_style_all(twelve);
    lv_obj_set_size(twelve, 9, 16);
    lv_obj_set_pos(twelve, CX - 4, CY - 111);
    lv_obj_set_style_bg_color(twelve, C_INDEX, 0);
    lv_obj_set_style_bg_opa(twelve, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(twelve, 1, 0);
    lv_obj_set_style_border_color(twelve, C_LUME, 0);
    lv_obj_set_style_border_side(twelve, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT, 0);
    bubble(twelve);

    /* 12 h : réserve de marche (batterie), arc de 120° */
    scale(s_root, PR_X, PR_Y, 62, 210, 120, 11, 5, 6, 2, 3, C_INDEX, C_PRINT, 1);
    s_pr_label = text(s_root, &font_small_11, C_PRINT, "RÉSERVE");
    lv_obj_align(s_pr_label, LV_ALIGN_TOP_MID, 0, PR_Y + 6);
    lv_obj_t *pr0 = text(s_root, &font_small_11, C_PRINT, "0");
    lv_obj_set_pos(pr0, PR_X - 30, PR_Y - 10);
    lv_obj_t *pr1 = text(s_root, &font_small_11, C_PRINT, "100");
    lv_obj_set_pos(pr1, PR_X + 22, PR_Y - 10);

    /* 6 h : petite seconde dans un compteur légèrement creusé */
    lv_obj_t *ssd = disc(s_root, SS_X, SS_Y, SS_D, C_SUBDIAL, 1, C_PRINT);
    lv_obj_t *sss = scale(ssd, SS_D / 2, SS_D / 2, SS_D - 6, 270, 360, 61, 15, 5, 2, 2, C_INDEX, C_PRINT, 0);
    (void)sss;
    s_sec = hand(ssd, SS_D, 2, C_INDEX, s_pt_sec);

    /* 3 h : guichet jour + date */
    lv_obj_t *win = lv_obj_create(s_root);
    lv_obj_remove_style_all(win);
    lv_obj_set_size(win, 56, 20);
    lv_obj_set_pos(win, DT_X - 28, DT_Y - 10);
    lv_obj_set_style_bg_color(win, C_WINDOW, 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(win, 1, 0);
    lv_obj_set_style_border_color(win, C_PRINT, 0);
    lv_obj_set_style_radius(win, 2, 0);
    bubble(win);
    s_date = text(win, &font_small_11, C_INDEX, "");
    lv_obj_center(s_date);

    /* 9 h : état Bluetooth + pastille de notifications */
    lv_obj_t *st = disc(s_root, ST_X, ST_Y, 38, C_SUBDIAL, 1, C_PRINT);
    s_ble = text(st, &font_text_16, C_DIM, LV_SYMBOL_BLUETOOTH);
    lv_obj_center(s_ble);
    s_badge = disc(s_root, ST_X + 14, ST_Y - 15, 17, C_BADGE, 0, C_BADGE);
    s_badge_txt = text(s_badge, &font_small_11, C_LUME, "");
    lv_obj_center(s_badge_txt);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    /* aiguilles (créées en dernier : au-dessus de tout) */
    s_pr_hand = hand(s_root, 240, 2, C_BLUED, s_pt_pr);
    disc(s_root, PR_X, PR_Y, 6, C_BLUED, 0, C_BLUED);
    s_hour = hand(s_root, 240, 7, C_BLUED, s_pt_hour);
    s_hour_lume = hand(s_root, 240, 2, C_LUME, s_pt_hour_l);
    s_min = hand(s_root, 240, 5, C_BLUED, s_pt_min);
    s_min_lume = hand(s_root, 240, 1, C_LUME, s_pt_min_l);
    disc(s_root, CX, CY, 13, C_BLUED, 0, C_BLUED);
    disc(s_root, CX, CY, 4, C_LUME, 0, C_LUME);
    disc(ssd, SS_D / 2, SS_D / 2, 5, C_INDEX, 0, C_INDEX);

    s_last_min = s_last_sec = s_last_pct = -1;
    return s_root;
}

void face_classic_set_time(const struct tm *tm, bool force)
{
    static const char *const JOURS[] = { "DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM" };
    if (force || tm->tm_min != s_last_min) {
        float m = tm->tm_min + tm->tm_sec / 60.0f;
        float h = (tm->tm_hour % 12) + m / 60.0f;
        place(s_hour, s_pt_hour, CX, CY, h * 30.0f, -12, 58);
        place(s_hour_lume, s_pt_hour_l, CX, CY, h * 30.0f, 20, 52);
        place(s_min, s_pt_min, CX, CY, m * 6.0f, -16, 94);
        place(s_min_lume, s_pt_min_l, CX, CY, m * 6.0f, 24, 88);
        char b[16];
        snprintf(b, sizeof(b), "%s %d", JOURS[tm->tm_wday % 7], tm->tm_mday);
        lv_label_set_text(s_date, b);
        s_last_min = tm->tm_min;
    }
    if (force || tm->tm_sec != s_last_sec) {
        place(s_sec, s_pt_sec, SS_D / 2, SS_D / 2, tm->tm_sec * 6.0f, -6, 22);
        s_last_sec = tm->tm_sec;
    }
}

void face_classic_set_status(int pct, bool charging, bool ble_ok, bool ble_on, int notifs)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct != s_last_pct) {
        place(s_pr_hand, s_pt_pr, PR_X, PR_Y, -60.0f + pct * 1.2f, -4, 24);
        s_last_pct = pct;
    }
    lv_label_set_text(s_pr_label, charging ? "CHARGE" : "RÉSERVE");
    lv_obj_set_style_text_color(s_pr_label, (pct <= 15 && !charging) ? C_BADGE : C_PRINT, 0);

    lv_obj_set_style_text_color(s_ble, ble_ok ? C_BLUED : (ble_on ? C_DIM : lv_color_hex(0xCFC4AA)), 0);
    if (notifs > 0) {
        lv_label_set_text_fmt(s_badge_txt, "%d", notifs > 9 ? 9 : notifs);
        lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    }
}
