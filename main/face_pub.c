#include "face_pub.h"
#include <math.h>
#include <stdio.h>
#include "fonts/fonts.h"

/* ---------- palette « pub irlandais » ---------- */
#define C_GREEN_TOP  lv_color_hex(0x1E5A3C)   /* vert bouteille */
#define C_GREEN_BOT  lv_color_hex(0x0F3522)
#define C_SUB        lv_color_hex(0x0C2B1B)   /* compteurs creusés */
#define C_BRASS      lv_color_hex(0xC9A24A)   /* laiton */
#define C_BRASS_D    lv_color_hex(0x8C6A2A)
#define C_CREAM      lv_color_hex(0xEFE2C2)   /* impressions crème */
#define C_DARK       lv_color_hex(0x08170F)
#define C_RED        lv_color_hex(0xB8352F)
#define C_AMBER      lv_color_hex(0xE0A030)
#define C_LAMP_ON    lv_color_hex(0x7CE08A)
#define C_DIM        lv_color_hex(0x5E7A63)

#define CX 120
#define CY 120
#define TRACK_H   110         /* demi-côté de la piste des minutes */
#define TRACK_P   6.0f        /* exposant du « squircle » de la piste */
#define IDX_LEN   18

/* complications */
#define FU_X 120              /* jauge FUEL */
#define FU_Y 62
#define FU_D 66
#define SS_X 120              /* petite seconde */
#define SS_Y 176
#define SS_D 58
#define DT_X 180              /* guichet date */
#define DT_Y 120
#define SG_X 62               /* enseigne liaison */
#define SG_Y 120
#define HP_X 60               /* harpe (centre de la boîte) */
#define HP_Y 180
#define SH_X 178              /* trèfle, symétrique de la harpe */
#define SH_Y 174

static lv_obj_t *s_root;
static lv_obj_t *s_hour, *s_hour_in, *s_min, *s_min_in, *s_sec, *s_fuel_hand;
static lv_obj_t *s_date, *s_fuel_lbl, *s_sign, *s_sign_txt, *s_lamp, *s_badge, *s_badge_txt;
static lv_point_precise_t s_pt_hour[2], s_pt_hour_in[2], s_pt_min[2], s_pt_min_in[2], s_pt_sec[2], s_pt_fuel[2];
static int s_last_min = -1, s_last_sec = -1, s_last_pct = -1, s_last_link = -1;

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

/* point de la piste « squircle » à l'angle deg (horaire depuis 12 h) */
static void track_pt(float deg, float h, float *x, float *y, float *ux, float *uy)
{
    float r = deg * (float)M_PI / 180.0f;
    float sx = sinf(r), sy = -cosf(r);
    float n = powf(powf(fabsf(sx), TRACK_P) + powf(fabsf(sy), TRACK_P), 1.0f / TRACK_P);
    float rad = h / n;
    *x = CX + rad * sx; *y = CY + rad * sy;
    *ux = sx; *uy = sy;
}

/* ------------------------------------------------------------------ */
/* Harpe celtique (dessin original, courbes de Bézier) : colonne galbée */
/* à gauche, console en S, caisse de résonance bombée qui s'affine vers  */
/* le haut, volute au sommet de la colonne, cordes qui suivent la console */
/* ------------------------------------------------------------------ */
typedef struct { float x, y; } fpt_t;

static fpt_t bez(fpt_t a, fpt_t b, fpt_t c, fpt_t d, float t)
{
    float u = 1 - t;
    float k0 = u * u * u, k1 = 3 * u * u * t, k2 = 3 * u * t * t, k3 = t * t * t;
    return (fpt_t){ k0 * a.x + k1 * b.x + k2 * c.x + k3 * d.x, k0 * a.y + k1 * b.y + k2 * c.y + k3 * d.y };
}

/* trait courbe, largeur interpolée w0 → w1, segments courts à bouts ronds */
static void draw_bez(lv_layer_t *layer, float ox, float oy, fpt_t a, fpt_t b, fpt_t c, fpt_t d,
                     float w0, float w1, lv_color_t col, int n)
{
    fpt_t p = bez(a, b, c, d, 0);
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n;
        fpt_t q = bez(a, b, c, d, t);
        int w = (int)(w0 + (w1 - w0) * (t - 0.5f / n) + 0.5f);
        draw_line(layer, ox + p.x, oy + p.y, ox + q.x, oy + q.y, w < 1 ? 1 : w, col, LV_OPA_COVER, true);
        p = q;
    }
}

static void draw_disc(lv_layer_t *layer, float cx, float cy, int r, lv_color_t c);

static void draw_harp(lv_layer_t *layer, int ox, int oy)
{
    float x0 = ox + HP_X - 21, y0 = oy + HP_Y - 29;          /* boîte 42 × 58 */
    /* caisse : du pied (bas) jusque sous l'extrémité de la console (haut droite) */
    const fpt_t sb0 = { 14, 55 }, sb1 = { 22, 40 }, sb2 = { 31, 22 }, sb3 = { 35, 6 };
    /* colonne : du pied, galbe vers la gauche, jusqu'à la volute */
    const fpt_t pl0 = { 13, 55 }, pl1 = { -2, 41 }, pl2 = { -1, 17 }, pl3 = { 7, 8 };
    /* console : S élégant de la volute à la tête de la caisse */
    const fpt_t nk0 = { 7, 8 }, nk1 = { 15, -5 }, nk2 = { 24, 18 }, nk3 = { 36, 4 };

    /* cordes d'abord (sous la structure) : de la console à la caisse */
    for (int i = 0; i < 8; i++) {
        float t = 0.22f + i * 0.085f;
        fpt_t top = bez(nk0, nk1, nk2, nk3, t);
        /* point de la caisse à la même abscisse (recherche sur la courbe) */
        float best = 1e9f, yb = top.y;
        for (int k = 0; k <= 40; k++) {
            fpt_t q = bez(sb0, sb1, sb2, sb3, k / 40.0f);
            float dx = fabsf(q.x - top.x);
            if (dx < best) { best = dx; yb = q.y; }
        }
        if (yb - top.y > 6) {
            draw_line(layer, x0 + top.x, y0 + top.y + 2, x0 + top.x, y0 + yb - 2, 1, C_CREAM, LV_OPA_60, false);
        }
    }
    draw_bez(layer, x0, y0, sb0, sb1, sb2, sb3, 8.0f, 3.5f, C_BRASS, 14);   /* caisse */
    draw_bez(layer, x0, y0, sb0, sb1, sb2, sb3, 1.0f, 1.0f, C_BRASS_D, 14); /* filet central */
    draw_bez(layer, x0, y0, pl0, pl1, pl2, pl3, 4.0f, 3.0f, C_BRASS, 16);   /* colonne */
    draw_bez(layer, x0, y0, nk0, nk1, nk2, nk3, 3.5f, 3.0f, C_BRASS, 16);   /* console */
    draw_disc(layer, x0 + 6, y0 + 7, 3, C_BRASS);                           /* volute */
    draw_disc(layer, x0 + 6, y0 + 7, 1, C_BRASS_D);
    draw_disc(layer, x0 + 36, y0 + 4, 2, C_BRASS);                          /* tête de caisse */
    draw_disc(layer, x0 + 13, y0 + 55, 3, C_BRASS);                         /* pied */
}

static void draw_disc(lv_layer_t *layer, float cx, float cy, int r, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    lv_area_t a = { (int32_t)(cx - r), (int32_t)(cy - r), (int32_t)(cx + r), (int32_t)(cy + r) };
    lv_draw_rect(layer, &d, &a);
}

/* Trèfle à trois feuilles en cœur + tige, pendant de la harpe */
static void draw_shamrock(lv_layer_t *layer, float cx, float cy)
{
    static const float ang[3] = { -90.0f, 30.0f, 150.0f };     /* haut, bas-droite, bas-gauche */
    for (int i = 0; i < 3; i++) {
        float a = ang[i] * (float)M_PI / 180.0f;
        float ux = cosf(a), uy = sinf(a);           /* direction de la feuille */
        float px = -uy, py = ux;                    /* perpendiculaire */
        float lx = cx + ux * 9, ly = cy + uy * 9;   /* centre de la feuille */
        /* cœur : deux lobes + pointe vers le centre */
        draw_disc(layer, lx + px * 4 + ux * 2, ly + py * 4 + uy * 2, 6, C_BRASS);
        draw_disc(layer, lx - px * 4 + ux * 2, ly - py * 4 + uy * 2, 6, C_BRASS);
        draw_line(layer, lx, ly, cx + ux * 2, cy + uy * 2, 7, C_BRASS, LV_OPA_COVER, true);
        draw_line(layer, lx + ux * 6, ly + uy * 6, cx + ux * 3, cy + uy * 3, 1, C_BRASS_D, LV_OPA_COVER, false);
    }
    draw_line(layer, cx, cy + 2, cx + 5, cy + 18, 3, C_BRASS, LV_OPA_COVER, true);   /* tige */
    draw_line(layer, cx + 5, cy + 18, cx + 9, cy + 22, 2, C_BRASS, LV_OPA_COVER, true);
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
            draw_line(layer, x, y, x - 6 * ux, y - 6 * uy, 1, C_CREAM, LV_OPA_70, false);
            continue;
        }
        if (m == 0) {   /* double index à 12 h */
            for (int k = -1; k <= 1; k += 2) {
                draw_line(layer, x + k * 6, y + 1, x + k * 6, y + IDX_LEN, 6, C_BRASS, LV_OPA_COVER, false);
                draw_line(layer, x + k * 6, y + 3, x + k * 6, y + IDX_LEN - 2, 1, C_BRASS_D, LV_OPA_COVER, false);
            }
            continue;
        }
        draw_line(layer, x, y, x - IDX_LEN * ux, y - IDX_LEN * uy, 7, C_BRASS, LV_OPA_COVER, false);
        draw_line(layer, x - 2 * ux, y - 2 * uy, x - (IDX_LEN - 2) * ux, y - (IDX_LEN - 2) * uy, 1,
                  C_BRASS_D, LV_OPA_COVER, false);
    }

    /* zone rouge de la jauge FUEL (0–15 %) */
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = ox + FU_X;
    a.center.y = oy + FU_Y;
    a.radius = FU_D / 2 - 1;
    a.width = 3;
    a.color = C_RED;
    a.start_angle = 210;            /* LVGL : 0° = 3 h, sens horaire ; E = −60° depuis 12 h */
    a.end_angle = 228;              /* 15 % de 120° = 18° */
    lv_draw_arc(layer, &a);

    draw_harp(layer, ox, oy);
    draw_shamrock(layer, ox + SH_X, oy + SH_Y);
}

/* ------------------------------------------------------------------ */
lv_obj_t *face_pub_create(lv_obj_t *parent)
{
    /* cadran vert bouteille plein écran, filet laiton au bord, liseré crème */
    s_root = box(parent, 0, 0, 240, 240, 0);
    lv_obj_set_style_bg_color(s_root, C_GREEN_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_root, C_GREEN_BOT, 0);
    lv_obj_set_style_bg_grad_dir(s_root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_t *rim = box(s_root, 1, 1, 238, 238, 18);
    lv_obj_set_style_border_width(rim, 3, 0);
    lv_obj_set_style_border_color(rim, C_BRASS, 0);
    lv_obj_t *pin = box(s_root, 7, 7, 226, 226, 13);
    lv_obj_set_style_border_width(pin, 1, 0);
    lv_obj_set_style_border_color(pin, C_CREAM, 0);
    lv_obj_set_style_border_opa(pin, LV_OPA_40, 0);

    lv_obj_t *track = box(s_root, 0, 0, 240, 240, 0);
    lv_obj_add_event_cb(track, track_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    /* 12 h : jauge FUEL */
    scale(s_root, FU_X, FU_Y, FU_D, 210, 120, 9, 4, 7, 2, 4, C_BRASS, C_CREAM);
    s_fuel_lbl = text(s_root, &font_small_11, C_CREAM, "FUEL");
    lv_obj_set_style_text_letter_space(s_fuel_lbl, 1, 0);
    lv_obj_align(s_fuel_lbl, LV_ALIGN_TOP_MID, 0, FU_Y + 6);
    lv_obj_t *le = text(s_root, &font_text_16, C_RED, "E");
    lv_obj_set_pos(le, FU_X - 35, FU_Y - 14);
    lv_obj_t *lf = text(s_root, &font_text_16, C_CREAM, "F");
    lv_obj_set_pos(lf, FU_X + 27, FU_Y - 14);

    lv_obj_t *brand = text(s_root, &font_small_11, C_BRASS, "SLÁINTE");
    lv_obj_set_style_text_letter_space(brand, 2, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, FU_Y + 34);

    /* 6 h : petite seconde */
    lv_obj_t *ssd = disc(s_root, SS_X, SS_Y, SS_D, C_SUB, 1, C_BRASS);
    scale(ssd, SS_D / 2, SS_D / 2, SS_D - 6, 270, 360, 61, 15, 5, 2, 2, C_BRASS, C_CREAM);
    s_sec = hand(ssd, SS_D, 2, C_RED, s_pt_sec);

    /* 3 h : guichet jour + date */
    lv_obj_t *win = box(s_root, DT_X - 28, DT_Y - 11, 56, 22, 3);
    lv_obj_set_style_bg_color(win, C_CREAM, 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(win, 2, 0);
    lv_obj_set_style_border_color(win, C_BRASS, 0);
    s_date = text(win, &font_small_11, C_DARK, "");
    lv_obj_center(s_date);

    /* 9 h : enseigne de pub = état de la liaison iPhone */
    s_sign = box(s_root, SG_X - 32, SG_Y - 11, 64, 22, 3);
    lv_obj_set_style_bg_color(s_sign, C_DARK, 0);
    lv_obj_set_style_bg_opa(s_sign, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sign, 2, 0);
    lv_obj_set_style_border_color(s_sign, C_BRASS, 0);
    s_lamp = disc(s_sign, 8, 9, 7, C_DIM, 0, C_DIM);
    s_sign_txt = text(s_sign, &font_small_11, C_DIM, "");
    lv_obj_align(s_sign_txt, LV_ALIGN_LEFT_MID, 13, 0);
    s_badge = disc(s_root, SG_X + 31, SG_Y - 12, 18, C_RED, 1, C_CREAM);
    s_badge_txt = text(s_badge, &font_small_11, C_CREAM, "");
    lv_obj_center(s_badge_txt);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    /* aiguilles laiton à filet sombre */
    s_fuel_hand = hand(s_root, 240, 2, C_CREAM, s_pt_fuel);
    disc(s_root, FU_X, FU_Y, 7, C_BRASS, 0, C_BRASS);
    s_hour = hand(s_root, 240, 9, C_BRASS, s_pt_hour);
    s_hour_in = hand(s_root, 240, 2, C_DARK, s_pt_hour_in);
    s_min = hand(s_root, 240, 7, C_BRASS, s_pt_min);
    s_min_in = hand(s_root, 240, 2, C_DARK, s_pt_min_in);
    disc(s_root, CX, CY, 15, C_BRASS, 1, C_BRASS_D);
    disc(s_root, CX, CY, 4, C_DARK, 0, C_DARK);
    disc(ssd, SS_D / 2, SS_D / 2, 5, C_RED, 0, C_RED);

    s_last_min = s_last_sec = s_last_pct = s_last_link = -1;
    return s_root;
}

void face_pub_set_time(const struct tm *tm, bool force)
{
    static const char *const DAYS[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    if (force || tm->tm_min != s_last_min) {
        float m = tm->tm_min + tm->tm_sec / 60.0f;
        float h = (tm->tm_hour % 12) + m / 60.0f;
        place(s_hour, s_pt_hour, CX, CY, h * 30.0f, -14, 62);
        place(s_hour_in, s_pt_hour_in, CX, CY, h * 30.0f, 16, 54);
        place(s_min, s_pt_min, CX, CY, m * 6.0f, -18, 100);
        place(s_min_in, s_pt_min_in, CX, CY, m * 6.0f, 18, 92);
        char b[16];
        snprintf(b, sizeof(b), "%s %d", DAYS[tm->tm_wday % 7], tm->tm_mday);
        lv_label_set_text(s_date, b);
        s_last_min = tm->tm_min;
    }
    if (force || tm->tm_sec != s_last_sec) {
        place(s_sec, s_pt_sec, SS_D / 2, SS_D / 2, tm->tm_sec * 6.0f, -6, 23);
        s_last_sec = tm->tm_sec;
    }
}

void face_pub_set_status(int pct, bool charging, int link, int notifs)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct != s_last_pct) {
        place(s_fuel_hand, s_pt_fuel, FU_X, FU_Y, -60.0f + pct * 1.2f, -5, 27);
        s_last_pct = pct;
    }
    lv_label_set_text(s_fuel_lbl, charging ? "CHARGING" : "FUEL");
    lv_obj_set_style_text_color(s_fuel_lbl, (pct <= 15 && !charging) ? C_RED : C_CREAM, 0);

    if (link != s_last_link) {
        static const char *const TXT[] = { "OFF", "CLOSED", "WAIT", "OPEN", "LATER" };
        lv_color_t lamp, txt;
        switch (link) {
        case PUB_LINK_READY: lamp = C_LAMP_ON; txt = C_CREAM; break;
        case PUB_LINK_WAIT:  lamp = C_AMBER;   txt = C_AMBER; break;
        case PUB_LINK_ADV:   lamp = C_RED;     txt = C_RED;   break;
        case PUB_LINK_ECO:   lamp = C_DIM;     txt = C_CREAM; break;   /* économie : relève plus tard */
        default:             lamp = C_DIM;     txt = C_DIM;   link = PUB_LINK_OFF; break;
        }
        lv_label_set_text(s_sign_txt, TXT[link]);
        lv_obj_set_style_text_color(s_sign_txt, txt, 0);
        lv_obj_set_style_bg_color(s_lamp, lamp, 0);
        s_last_link = link;
    }
    if (notifs > 0) {
        lv_label_set_text_fmt(s_badge_txt, "%d", notifs > 9 ? 9 : notifs);
        lv_obj_remove_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);
    }
}
