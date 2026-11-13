/*
 * app_webrx.c — écoute d'un serveur OpenWebRX sur le haut-parleur.
 *
 *   ‹  ● Profil ………………………  🔊        barre : retour, profil (liste), son
 *        145.5000                       fréquence : toucher un chiffre = pas de −/+
 *   ≡ Relais F1ZXX            [NFM]     signets (liste) · mode (liste)
 *   ▮▮▮▮▮▮▮▮▯▯▯▯▯▯▯  -87 dB             S-mètre
 *   ░░▒▒▓▓░░░│░░░░░░░░░ ×4              waterfall : toucher = écouter là, ×N = zoom
 *      [ − ]   [ ▶ / ■ ]   [ + ]
 *
 * Écran éteint, l'écoute continue (waterfall suspendu) ; quitter l'app
 * l'arrête et rend le Bluetooth s'il avait fallu le suspendre.
 */
#include <stdio.h>
#include "esp_attr.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "webrx.h"
#include "webrx_cfg.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

extern const app_t app_webrx;

#define TOP_H  32
#define FREQ_Y 34
#define INFO_Y 88
#define MET_Y  116
#define WF_Y   128
#define WF_H   54
#define NDIG   8                        /* 4 chiffres de MHz + 4 décimales (pas de 100 Hz) */
#define SPAN_DB 50.0f

static const int64_t W[NDIG] = { 1000000000LL, 100000000LL, 10000000LL, 1000000LL, 100000, 10000, 1000, 100 };

/* modèle */
static int   m_dig = 6;                 /* chiffre réglé par −/+ (1 kHz) */
static int   m_zoom = 1;
static bool  m_want = true;             /* l'utilisateur veut écouter (lancement auto) */
static float m_floor = -100.0f;
static bool  m_have_floor;
static int64_t m_lo; static int32_t m_span;   /* plage affichée par la waterfall */
static wrx_info_t m_i;
static uint32_t m_bm_ver = ~0u;
static wrx_bm_t *m_bm; static int m_nbm;
static wrx_mode_t *m_modes; static int m_nmodes;
static uint32_t m_mode_ver = ~0u;

/* vue */
static lv_obj_t *v_scr, *v_dot, *v_prof, *v_dig[NDIG], *v_dot_sep, *v_name, *v_mod, *v_mod_lbl, *v_meter, *v_meter_lbl;
static lv_obj_t *v_wf, *v_over, *v_zoom, *v_status, *v_st1, *v_st2, *v_play, *v_play_lbl, *v_minus, *v_plus, *v_setup;
static lv_obj_t *v_sheet, *v_sheet_title, *v_sheet_list, *v_spk;
static lv_obj_t *v_vol, *v_vol_lbl, *v_sq, *v_sq_lbl, *v_sq_sig;
static uint16_t *v_wf_buf;
static lv_timer_t *v_tick;
EXT_RAM_BSS_ATTR static uint16_t s_pal[256];
static int v_sheet_kind;
enum { SH_NONE, SH_PROF, SH_BM, SH_MODE, SH_SOUND };

static void sheet_close(void);
static lv_obj_t *v_tone_lbl;
static void refresh(bool force);

/* ------------------------------------------------------------------ */
/* utilitaires                                                          */
/* ------------------------------------------------------------------ */
static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return lv_color_to_u16(lv_color_make(r, g, b)); }

static void palette_init(void)
{
    static const struct { float t; uint8_t r, g, b; } K[] = {
        { 0.00f, 0, 0, 8 }, { 0.20f, 10, 20, 110 }, { 0.42f, 0, 150, 210 },
        { 0.62f, 240, 220, 40 }, { 0.82f, 240, 60, 20 }, { 1.00f, 255, 255, 255 },
    };
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        int k = 0;
        while (k < 4 && t > K[k + 1].t) k++;
        float u = (t - K[k].t) / (K[k + 1].t - K[k].t);
        s_pal[i] = rgb565((uint8_t)(K[k].r + u * (K[k + 1].r - K[k].r)),
                          (uint8_t)(K[k].g + u * (K[k + 1].g - K[k].g)),
                          (uint8_t)(K[k].b + u * (K[k + 1].b - K[k].b)));
    }
}

static void mod_upper(const char *m, char *out, size_t cap)
{
    size_t i = 0;
    for (; m[i] && i + 1 < cap; i++) out[i] = (char)(m[i] >= 'a' && m[i] <= 'z' ? m[i] - 32 : m[i]);
    out[i] = 0;
    if (!strcmp(out, "NFM")) strlcpy(out, "FM", cap);
}

/* nom d'un mode (liste du serveur), sinon la modulation en capitales */
static void mode_name(const char *mod, char *out, size_t cap)
{
    for (int i = 0; i < m_nmodes; i++) if (!strcmp(m_modes[i].mod, mod)) { strlcpy(out, m_modes[i].name, cap); return; }
    mod_upper(mod, out, cap);
}

static const wrx_mode_t *mode_of(const char *mod)
{
    for (int i = 0; i < m_nmodes; i++) if (!strcmp(m_modes[i].mod, mod)) return &m_modes[i];
    return NULL;
}

static void fmt_mhz(int64_t hz, char *b, size_t cap)
{
    snprintf(b, cap, "%lld.%04lld", (long long)(hz / 1000000), (long long)((hz % 1000000) / 100));
}

static const wrx_bm_t *bm_at(int64_t f)
{
    for (int i = 0; i < m_nbm; i++) if (llabs(m_bm[i].freq - f) < 50) return &m_bm[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* waterfall                                                            */
/* ------------------------------------------------------------------ */
static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void wf_push(const wrx_row_t *r)
{
    if (!v_wf_buf) return;
    static float *tmp;                                  /* PSRAM */
    if (!tmp) tmp = heap_caps_malloc(WRX_FFT_W * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) return;
    memcpy(tmp, r->db, WRX_FFT_W * sizeof(float));
    qsort(tmp, WRX_FFT_W, sizeof(float), cmpf);
    float p = tmp[WRX_FFT_W / 5];                       /* 20e centile ≈ bruit */
    m_floor = m_have_floor ? m_floor + 0.1f * (p - m_floor) : p;
    m_have_floor = true;
    memmove(v_wf_buf + WRX_FFT_W, v_wf_buf, WRX_FFT_W * (WF_H - 1) * sizeof(uint16_t));
    for (int x = 0; x < WRX_FFT_W; x++) {
        float t = (r->db[x] - (m_floor - 3.0f)) / SPAN_DB;
        int v = (int)(t * 255.0f);
        v_wf_buf[x] = s_pal[v < 0 ? 0 : v > 255 ? 255 : v];
    }
    bool moved = r->lo != m_lo || r->span != m_span;
    m_lo = r->lo;
    m_span = r->span;
    lv_obj_invalidate(v_wf);
    if (moved) lv_obj_invalidate(v_over);
}

static float x_of(int64_t f) { return m_span ? (float)(f - m_lo) * WRX_FFT_W / (float)m_span : -1.0f; }

/* repère d'écoute, bande passante et signets, par-dessus la waterfall */
static void over_draw_cb(lv_event_t *e)
{
    if (!m_span || m_i.state != WRX_LIVE) return;
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t A;
    lv_obj_get_coords(o, &A);
    /* signets : petits traits jaunes en haut */
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(0xFACC15);
    rd.bg_opa = LV_OPA_COVER;
    for (int i = 0; i < m_nbm; i++) {
        float x = x_of(m_bm[i].freq);
        if (x < 0 || x >= WRX_FFT_W) continue;
        lv_area_t a = { A.x1 + (int)x, A.y1, A.x1 + (int)x, A.y1 + 5 };
        lv_draw_rect(layer, &rd, &a);
    }
    /* bande passante (approximative) et fréquence écoutée */
    int lo = -4000, hi = 4000;
    const wrx_mode_t *md = mode_of(m_i.mod);
    if (md && md->bp) { lo = md->lo; hi = md->hi; }
    float x0 = x_of(m_i.freq + lo), x1 = x_of(m_i.freq + hi), xc = x_of(m_i.freq);
    if (x1 - x0 >= 2) {
        rd.bg_color = C_FG;
        rd.bg_opa = LV_OPA_20;
        lv_area_t a = { A.x1 + (int)x0, A.y1, A.x1 + (int)x1, A.y2 };
        lv_draw_rect(layer, &rd, &a);
    }
    if (xc >= 0 && xc < WRX_FFT_W) {
        rd.bg_color = lv_color_hex(0xEF4444);
        rd.bg_opa = LV_OPA_COVER;
        lv_area_t a = { A.x1 + (int)xc, A.y1, A.x1 + (int)xc, A.y2 };
        lv_draw_rect(layer, &rd, &a);
    }
}

/* toucher la waterfall : écouter là (signet proche, sinon arrondi au pas choisi) */
static void wf_click_cb(lv_event_t *e)
{
    if (m_i.state != WRX_LIVE || !m_span) return;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t A;
    lv_obj_get_coords(v_over, &A);
    int x = p.x - A.x1;
    for (int i = 0; i < m_nbm; i++) {
        float bx = x_of(m_bm[i].freq);
        if (fabsf(bx - x) <= 4.0f) { webrx_select(&m_bm[i]); refresh(true); return; }
    }
    int64_t f = m_lo + (int64_t)x * m_span / WRX_FFT_W;
    int64_t step = W[m_dig] > 10000 ? 10000 : W[m_dig];
    f = (f + step / 2) / step * step;
    webrx_tune(f);
    refresh(true);
}

static void zoom_cb(lv_event_t *e)
{
    m_zoom = m_zoom == 1 ? 4 : m_zoom == 4 ? 16 : 1;
    char b[8];
    snprintf(b, sizeof b, "\xC3\x97%d", m_zoom);
    set_text(lv_obj_get_child(v_zoom, 0), b);
    webrx_fft_enable(true, m_zoom);
}

/* ------------------------------------------------------------------ */
/* affichage                                                            */
/* ------------------------------------------------------------------ */
static void freq_draw(void)
{
    int64_t f = m_i.freq;
    int64_t top = f;
    if (m_i.center && m_i.samp_rate) top = m_i.center + m_i.samp_rate / 2;
    int nint = 1;                                       /* chiffres de MHz affichés */
    for (int64_t t = (top > f ? top : f) / 10000000; t > 0 && nint < 4; t /= 10) nint++;
    char d[24];
    snprintf(d, sizeof d, "%04lld%04lld", (long long)((f / 1000000) % 10000), (long long)((f % 1000000) / 100));
    bool live = m_i.state == WRX_LIVE;
    for (int i = 0; i < NDIG; i++) {
        bool shown = i >= 4 - nint;
        if (shown) lv_obj_remove_flag(v_dig[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(v_dig[i], LV_OBJ_FLAG_HIDDEN);
        char c[2] = { f ? d[i] : '-', 0 };
        set_text(v_dig[i], c);
        lv_color_t col = !live ? C_DIM : i == m_dig ? C_ACC : C_FG;
        lv_obj_set_style_text_color(v_dig[i], col, 0);
        lv_obj_set_style_border_width(v_dig[i], live && i == m_dig ? 3 : 0, 0);
    }
    if (m_dig < 4 - nint) m_dig = 4 - nint;
}

static void status_draw(void)
{
    char t[96] = "", d[96] = "";
    const char *msg = m_i.msg ? _(m_i.msg) : NULL;
    lv_color_t col = C_FG;
    if (!webrx_running()) {
        snprintf(t, sizeof t, "%s", _("Arrêté"));
        col = C_DIM;
    } else if (m_i.state == WRX_FAIL) {
        col = C_BAD;
        snprintf(t, sizeof t, "%s", msg ? msg : _("Connexion impossible"));
        int n = snprintf(d, sizeof d, "%s%s", m_i.detail, m_i.detail[0] ? " · " : "");
        if (m_i.retry_s > 0) snprintf(d + n, sizeof d - n, _("nouvel essai dans %d s"), m_i.retry_s);
        else snprintf(d + n, sizeof d - n, "%s", _("toucher pour réessayer"));
    } else if (m_i.state != WRX_LIVE) {
        snprintf(t, sizeof t, "%s", msg ? msg : "");
        int n = snprintf(d, sizeof d, "%s%s", m_i.detail, m_i.detail[0] && m_i.retry_s > 0 ? " · " : "");
        if (m_i.retry_s > 0) snprintf(d + n, sizeof d - n, _("nouvel essai dans %d s"), m_i.retry_s);
    } else if (m_i.msg) {                               /* erreur du récepteur, en direct */
        col = C_WARN;
        snprintf(t, sizeof t, "%s", msg);
        snprintf(d, sizeof d, "%s", m_i.detail);
    }
    if (t[0]) {
        set_text(v_st1, t);
        set_text(v_st2, d);
        lv_obj_set_style_text_color(v_st1, col, 0);
        lv_obj_remove_flag(v_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v_status, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh(bool force)
{
    if (!v_scr) return;
    webrx_info(&m_i);
    if (m_i.mode_ver != m_mode_ver) {
        m_mode_ver = m_i.mode_ver;
        m_nmodes = m_modes ? webrx_modes(m_modes, WRX_MODE_MAX) : 0;
        if (v_sheet_kind == SH_MODE) sheet_close();
        force = true;
    }
    if (m_i.bm_ver != m_bm_ver) {
        m_bm_ver = m_i.bm_ver;
        m_nbm = m_bm ? webrx_bookmarks(m_bm, WRX_BM_MAX) : 0;
        lv_obj_invalidate(v_over);
        if (v_sheet_kind == SH_BM) sheet_close();
    }
    bool run = webrx_running(), live = run && m_i.state == WRX_LIVE;
    lv_color_t dc = !run ? C_OFF : live ? C_OK : m_i.state == WRX_FAIL ? C_BAD : C_WARN;
    lv_obj_set_style_bg_color(v_dot, dc, 0);
    /* profil */
    static wrx_prof_t *pl;                               /* PSRAM */
    if (!pl) pl = heap_caps_malloc(WRX_PROF_MAX * sizeof *pl, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static uint32_t pver = ~0u;
    static char pname[48];
    if (pver != m_i.prof_ver || force) {
        pver = m_i.prof_ver;
        pname[0] = 0;
        int n = pl ? webrx_profiles(pl, WRX_PROF_MAX) : 0;
        for (int i = 0; i < n; i++) if (!strcmp(pl[i].id, m_i.profile)) strlcpy(pname, pl[i].name, sizeof pname);
    }
    set_text(v_prof, pname[0] ? pname : m_i.server_name[0] ? m_i.server_name : "WebRX");
    freq_draw();
    /* nom : station RDS, signet écouté, sinon invitation */
    const wrx_bm_t *bm = bm_at(m_i.freq);
    char nb[64];
    const char *nm = m_i.station[0] ? m_i.station : bm ? bm->name : NULL;
    snprintf(nb, sizeof nb, LV_SYMBOL_LIST " %s", nm ? nm : (m_nbm ? _("Signets") : ""));
    set_text(v_name, nb);
    lv_obj_set_style_text_color(v_name, nm ? C_FG : C_DIM, 0);
    char mu[24];
    mode_name(m_i.mod[0] ? m_i.mod : "nfm", mu, sizeof mu);
    set_text(v_mod_lbl, mu);
    /* S-mètre */
    float lv = m_i.level_db;
    int pct = live && lv > -199 ? (int)((lv + 110.0f) * 100.0f / 80.0f) : 0;
    lv_bar_set_value(v_meter, pct < 0 ? 0 : pct > 100 ? 100 : pct, LV_ANIM_OFF);
    bool open = m_i.squelch <= -150 || lv >= m_i.squelch;
    lv_obj_set_style_bg_color(v_meter, open ? C_OK : C_DIM, LV_PART_INDICATOR);
    char mb[16] = "";
    if (live && lv > -199) snprintf(mb, sizeof mb, "%d dB", (int)lroundf(lv));
    set_text(v_meter_lbl, mb);
    /* volume bas : icône du haut-parleur « petit volume », à toucher */
    set_text(v_spk, m_i.volume > 30 ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(v_spk, m_i.volume ? C_FG : C_WARN, 0);
    /* boutons */
    set_text(v_play_lbl, run ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_color(v_play, run ? lv_color_hex(0xDC2626) : lv_color_hex(0x16A34A), 0);
    lv_obj_set_state(v_minus, LV_STATE_DISABLED, !live);
    lv_obj_set_state(v_plus, LV_STATE_DISABLED, !live);
    status_draw();
    if (v_sheet_kind == SH_SOUND && v_sq_sig) {
        char sb[32] = "";
        if (live && lv > -199) snprintf(sb, sizeof sb, _("Signal : %d dB"), (int)lroundf(lv));
        set_text(v_sq_sig, sb);
    }
    if (force) lv_obj_invalidate(v_over);
}

static void tick_cb(lv_timer_t *t)
{
    static int64_t last_freq;
    static char last_mod[16];
    wrx_row_t *r = NULL;
    static wrx_row_t *row;                               /* PSRAM */
    if (!row) row = heap_caps_malloc(sizeof *row, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (row && webrx_fft_pop(row)) r = row;
    refresh(false);
    if (r) wf_push(r);
    if (m_i.freq != last_freq || strcmp(m_i.mod, last_mod)) {
        last_freq = m_i.freq;
        strlcpy(last_mod, m_i.mod, sizeof last_mod);
        lv_obj_invalidate(v_over);
    }
}

/* ------------------------------------------------------------------ */
/* marche / arrêt                                                       */
/* ------------------------------------------------------------------ */
static void start(void)
{
    if (!wrx_cfg_ready()) {
        lv_obj_remove_flag(v_setup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(v_setup);
        return;
    }
    lv_obj_add_flag(v_setup, LV_OBJ_FLAG_HIDDEN);
    esp_err_t e = webrx_start();
    if (e != ESP_OK) {
        ui_toast(_("Wi-Fi indisponible (mémoire ?)"));
        return;
    }
    if (webrx_ble_paused()) ui_toast(_("Bluetooth en pause pendant l'écoute"));
    m_have_floor = false;
    if (v_wf_buf) memset(v_wf_buf, 0, WRX_FFT_W * WF_H * sizeof(uint16_t));
    m_span = 0;
}

static void play_cb(lv_event_t *e)
{
    if (webrx_running()) {
        m_want = false;
        webrx_stop();
    } else {
        m_want = true;
        start();
    }
    refresh(true);
}

static void status_cb(lv_event_t *e)
{
    if (m_i.state == WRX_FAIL) webrx_retry();
}

static void step(int dir)
{
    if (m_i.state != WRX_LIVE) return;
    webrx_tune(m_i.freq + dir * W[m_dig]);
    refresh(true);
}
static void minus_cb(lv_event_t *e) { step(-1); }
static void plus_cb(lv_event_t *e)  { step(+1); }

static void dig_cb(lv_event_t *e)
{
    m_dig = (int)(intptr_t)lv_event_get_user_data(e);
    refresh(true);
}

static void setup_cb(lv_event_t *e)
{
    sheet_close();
    app_open_settings_portal(&app_webrx);
}

/* ------------------------------------------------------------------ */
/* feuilles : profils, signets, modes, son                              */
/* ------------------------------------------------------------------ */
static void sheet_close(void)
{
    v_tone_lbl = NULL;
    v_sheet_kind = SH_NONE;
    v_vol = v_vol_lbl = v_sq = v_sq_lbl = v_sq_sig = NULL;
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

static lv_obj_t *sheet_row(const char *txt, const char *sub, bool cur, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_button_create(v_sheet_list);
    lv_obj_set_size(b, lv_pct(100), sub ? 50 : 42);
    lv_obj_set_style_bg_color(b, cur ? lv_color_hex(0x1E3A5F) : C_CARD, 0);
    lv_obj_set_style_bg_color(b, C_OFF, LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_pad_ver(b, 4, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    ui_bubble(b);
    lv_obj_t *l = ui_label(b, &font_text_16, cur ? C_ACC : C_FG, txt);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, lv_pct(100), 20);
    lv_obj_align(l, sub ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 0, 0);
    if (sub) {
        lv_obj_t *s = ui_label(b, &font_small_11, C_DIM, sub);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    return b;
}

static void sheet_open(int kind, const char *title)
{
    if (!v_sheet) return;
    lv_obj_clean(v_sheet_list);
    v_vol = v_vol_lbl = v_sq = v_sq_lbl = v_sq_sig = NULL;
    v_sheet_kind = kind;
    set_text(v_sheet_title, title);
    lv_obj_scroll_to_y(v_sheet_list, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(v_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v_sheet);
}

/* profils */
static wrx_prof_t *s_pl;

static void prof_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    sheet_close();
    if (!s_pl) return;
    if (strcmp(s_pl[i].id, m_i.profile)) {
        webrx_profile(s_pl[i].id);
        m_span = 0;
        m_have_floor = false;
        if (v_wf_buf) memset(v_wf_buf, 0, WRX_FFT_W * WF_H * sizeof(uint16_t));
        lv_obj_invalidate(v_wf);
    }
    refresh(true);
}

static void srv_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    sheet_close();
    if (i != m_i.server) {
        webrx_server(i);
        m_span = 0;
        m_have_floor = false;
        if (v_wf_buf) memset(v_wf_buf, 0, WRX_FFT_W * WF_H * sizeof(uint16_t));
        lv_obj_invalidate(v_wf);
    }
    refresh(true);
}

static lv_obj_t *section(const char *t)
{
    lv_obj_t *l = ui_label(v_sheet_list, &font_small_11, C_DIM, t);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_pad_top(l, 4, 0);
    return l;
}

static void prof_cb(lv_event_t *e)
{
    sheet_open(SH_PROF, _("Récepteur"));
    /* serveurs (s'il y en a plusieurs) */
    wrx_cfg_t *c = heap_caps_malloc(sizeof *c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) {
        wrx_cfg_load(c);
        int ns = wrx_cfg_servers(c);
        if (ns > 1) {
            section(_("SERVEUR"));
            for (int i = 0; i < ns; i++) {
                char name[40];
                wrx_srv_label(c, i, name, sizeof name);
                sheet_row(name, NULL, i == m_i.server, srv_pick_cb, i);
            }
            section(_("PROFIL"));
        }
        free(c);
    }
    if (!s_pl) s_pl = heap_caps_malloc(WRX_PROF_MAX * sizeof *s_pl, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int n = s_pl ? webrx_profiles(s_pl, WRX_PROF_MAX) : 0;
    for (int i = 0; i < n; i++) sheet_row(s_pl[i].name, NULL, !strcmp(s_pl[i].id, m_i.profile), prof_pick_cb, i);
    if (!n) {
        lv_obj_t *l = ui_label(v_sheet_list, &font_text_16, C_DIM,
                               webrx_running() ? _("Liste reçue à la connexion") : _("Démarrer l'écoute pour voir les profils"));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lv_pct(100));
    }
    char t[48];
    snprintf(t, sizeof t, LV_SYMBOL_SETTINGS "  %s", _("Wi-Fi et serveur"));
    sheet_row(t, NULL, false, setup_cb, 0);
}

/* signets */
static void bm_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    sheet_close();
    if (i < m_nbm) webrx_select(&m_bm[i]);
    refresh(true);
}

static void bm_cb(lv_event_t *e)
{
    if (m_i.state != WRX_LIVE) return;
    sheet_open(SH_BM, _("Signets"));
    int cur = -1;
    for (int i = 0; i < m_nbm; i++) {
        char f[24], m[24], sub[64];
        fmt_mhz(m_bm[i].freq, f, sizeof f);
        mode_name(m_bm[i].mod, m, sizeof m);
        snprintf(sub, sizeof sub, "%s MHz · %s", f, m);
        bool c = llabs(m_bm[i].freq - m_i.freq) < 50;
        lv_obj_t *r = sheet_row(m_bm[i].name[0] ? m_bm[i].name : f, sub, c, bm_pick_cb, i);
        if (c && cur < 0) cur = i;
        if (c) lv_obj_scroll_to_view(r, LV_ANIM_OFF);
    }
    if (!m_nbm) {
        lv_obj_t *l = ui_label(v_sheet_list, &font_text_16, C_DIM, _("Aucun signet dans cette bande"));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lv_pct(100));
    }
}

/* modes : liste du serveur (modes ajoutés par l'administrateur compris) */
static void mode_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    sheet_close();
    if (i < m_nmodes) webrx_set_mod(m_modes[i].mod);
    refresh(true);
}

static void bp_text(const wrx_mode_t *m, char *b, size_t cap)
{
    if (!m->bp) { snprintf(b, cap, "%s", m->mod); return; }
    if (m->lo == -m->hi) {
        if (m->hi >= 10000) snprintf(b, cap, "%s · \xC2\xB1 %d kHz", m->mod, m->hi / 1000);
        else snprintf(b, cap, "%s · \xC2\xB1 %.1f kHz", m->mod, m->hi / 1000.0);
    } else {
        snprintf(b, cap, "%s · %d … %d Hz", m->mod, m->lo, m->hi);
    }
}

static void mode_cb(lv_event_t *e)
{
    if (m_i.state != WRX_LIVE) return;
    sheet_open(SH_MODE, _("Mode"));
    for (int i = 0; i < m_nmodes; i++) {
        if (!m_modes[i].bp) continue;                   /* DAB… : pas écoutable ainsi */
        char sub[48];
        bp_text(&m_modes[i], sub, sizeof sub);
        sheet_row(m_modes[i].name, sub, !strcmp(m_modes[i].mod, m_i.mod), mode_pick_cb, i);
    }
    if (!m_nmodes) {
        lv_obj_t *l = ui_label(v_sheet_list, &font_text_16, C_DIM, _("Liste reçue à la connexion"));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lv_pct(100));
    }
}

/* son : volume et squelch */
static void sq_text(int v)
{
    char b[24];
    if (v <= -130) snprintf(b, sizeof b, "%s", _("Coupé"));
    else snprintf(b, sizeof b, "%d dB", v);
    set_text(v_sq_lbl, b);
}

static void vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(v_vol);
    webrx_volume(v);
    char b[8];
    snprintf(b, sizeof b, "%d %%", v);
    set_text(v_vol_lbl, b);
}

static void sq_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(v_sq);
    sq_text(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) webrx_squelch(v <= -130 ? -150 : v);
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
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return s;
}

static lv_obj_t *kv(const char *k, lv_obj_t **val)
{
    lv_obj_t *r = ui_container(v_sheet_list);
    lv_obj_set_size(r, lv_pct(100), 22);
    ui_bubble(r);
    ui_label(r, &font_text_16, C_FG, k);
    *val = ui_label(r, &font_text_16, C_ACC, "");
    lv_obj_align(*val, LV_ALIGN_RIGHT_MID, 0, 0);
    return r;
}

static const char *const TONE[WRX_TONES] = { N_("Neutre"), N_("Claire"), N_("Voix"), N_("Douce"), N_("Sourde") };

static void tone_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    if (v_tone_lbl) set_text(v_tone_lbl, _(TONE[v]));
    webrx_tone(v);
}

static void sound_cb(lv_event_t *e)
{
    sheet_open(SH_SOUND, _("Son"));
    kv(_("Volume"), &v_vol_lbl);
    v_vol = mk_slider(0, 100, m_i.volume, vol_cb);
    char b[8];
    snprintf(b, sizeof b, "%d %%", m_i.volume);
    set_text(v_vol_lbl, b);
    kv(_("Squelch"), &v_sq_lbl);
    int sq = m_i.squelch <= -150 ? -130 : m_i.squelch;
    v_sq = mk_slider(-130, -20, sq, sq_cb);
    sq_text(sq);
    v_sq_sig = ui_label(v_sheet_list, &font_small_11, C_DIM, "");
    kv(_("Tonalité"), &v_tone_lbl);
    mk_slider(0, WRX_TONES - 1, m_i.tone, tone_cb);
    set_text(v_tone_lbl, _(TONE[m_i.tone < WRX_TONES ? m_i.tone : WRX_TONE_DEF]));
    lv_obj_t *fh = ui_label(v_sheet_list, &font_small_11, C_DIM,
                            _("Tonalité : vers la droite, moins d'aigus et de souffle, plus de corps aux voix."));
    lv_label_set_long_mode(fh, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(fh, lv_pct(100));
    lv_obj_t *h = ui_label(v_sheet_list, &font_small_11, C_DIM,
                           _("Squelch : coupe le son tant que le signal reste sous le seuil. Tout à gauche : coupé."));
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, lv_pct(100));
    refresh(false);
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

static void back_cb(lv_event_t *e) { app_close(); }

static lv_obj_t *round_btn(lv_obj_t *scr, const char *txt, lv_event_cb_t cb, lv_color_t bg, int x, bool rep)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_set_size(b, 64, 48);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -6);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x22252B), LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (rep) lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    ui_bubble(b);
    lv_obj_t *l = ui_label(b, txt[0] == '+' || txt[0] == '-' ? &font_num_44 : &font_title_20, C_FG, txt);
    lv_obj_center(l);
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
    lv_obj_t *ar = ui_label(hdr, &font_title_20, C_ACC, LV_SYMBOL_LEFT);
    lv_obj_align(ar, LV_ALIGN_LEFT_MID, 12, 0);
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
    lv_obj_t *ic = ui_label(v_setup, &font_title_20, lv_color_hex(0x10B981), SYM_HEADPHONES);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *t = ui_label(v_setup, &font_text_16, C_FG,
                           _("Indiquer les réseaux Wi-Fi et l'adresse du serveur OpenWebRX, depuis le téléphone."));
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
    palette_init();
    if (!m_bm) m_bm = heap_caps_malloc(WRX_BM_MAX * sizeof *m_bm, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m_modes) m_modes = heap_caps_malloc(WRX_MODE_MAX * sizeof *m_modes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    m_bm_ver = m_mode_ver = ~0u;

    /* barre du haut */
    lv_obj_t *bk = flat_btn(scr, back_cb);
    lv_obj_set_size(bk, 34, TOP_H);
    lv_obj_set_pos(bk, 0, 0);
    lv_obj_center(ui_label(bk, &font_title_20, C_ACC, LV_SYMBOL_LEFT));
    lv_obj_t *pb = flat_btn(scr, prof_cb);
    lv_obj_set_size(pb, 168, TOP_H);
    lv_obj_set_pos(pb, 34, 0);
    v_dot = lv_obj_create(pb);
    lv_obj_remove_style_all(v_dot);
    lv_obj_set_size(v_dot, 8, 8);
    lv_obj_set_style_radius(v_dot, 4, 0);
    lv_obj_set_style_bg_opa(v_dot, LV_OPA_COVER, 0);
    lv_obj_align(v_dot, LV_ALIGN_LEFT_MID, 2, 0);
    v_prof = ui_label(pb, &font_text_16, C_FG, "WebRX");
    lv_label_set_long_mode(v_prof, LV_LABEL_LONG_DOT);
    lv_obj_set_size(v_prof, 150, 20);
    lv_obj_align(v_prof, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t *sb = flat_btn(scr, sound_cb);
    lv_obj_set_size(sb, 38, TOP_H);
    lv_obj_set_pos(sb, 202, 0);
    v_spk = ui_label(sb, &font_text_16, C_FG, LV_SYMBOL_VOLUME_MAX);
    lv_obj_center(v_spk);

    /* fréquence */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 240, 52);
    lv_obj_set_pos(row, 0, FREQ_Y);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    ui_bubble(row);
    for (int i = 0; i < NDIG; i++) {
        if (i == 4) v_dot_sep = ui_label(row, &font_num_44, C_DIM, ".");
        v_dig[i] = ui_label(row, &font_num_44, C_FG, "0");
        lv_obj_set_style_border_side(v_dig[i], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(v_dig[i], C_ACC, 0);
        lv_obj_add_flag(v_dig[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(v_dig[i], 4);
        lv_obj_add_event_cb(v_dig[i], dig_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui_bubble(v_dig[i]);
    }

    /* signet / station et mode */
    lv_obj_t *nb = flat_btn(scr, bm_cb);
    lv_obj_set_size(nb, 148, 26);
    lv_obj_set_pos(nb, 4, INFO_Y);
    v_name = ui_label(nb, &font_text_16, C_DIM, "");
    lv_label_set_long_mode(v_name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(v_name, 142, 20);              /* hauteur fixe : « … » sur une ligne */
    lv_obj_align(v_name, LV_ALIGN_LEFT_MID, 4, 0);
    v_mod = lv_button_create(scr);
    lv_obj_set_size(v_mod, 82, 24);
    lv_obj_set_pos(v_mod, 154, INFO_Y + 1);
    lv_obj_set_style_radius(v_mod, 8, 0);
    lv_obj_set_style_shadow_width(v_mod, 0, 0);
    lv_obj_set_style_bg_color(v_mod, C_CARD, 0);
    lv_obj_set_style_bg_color(v_mod, C_OFF, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(v_mod, 0, 0);
    lv_obj_add_event_cb(v_mod, mode_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(v_mod);
    v_mod_lbl = ui_label(v_mod, &font_text_16, C_ACC, "FM");
    lv_label_set_long_mode(v_mod_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(v_mod_lbl, 78, 20);
    lv_obj_set_style_text_align(v_mod_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(v_mod_lbl);

    /* S-mètre */
    v_meter = lv_bar_create(scr);
    lv_obj_set_size(v_meter, 176, 6);
    lv_obj_set_pos(v_meter, 8, MET_Y + 2);
    lv_bar_set_range(v_meter, 0, 100);
    lv_obj_set_style_bg_color(v_meter, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_radius(v_meter, 3, 0);
    lv_obj_set_style_radius(v_meter, 3, LV_PART_INDICATOR);
    ui_bubble(v_meter);
    v_meter_lbl = ui_label(scr, &font_small_11, C_DIM, "");
    lv_obj_set_width(v_meter_lbl, 48);
    lv_obj_set_style_text_align(v_meter_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(v_meter_lbl, 188, MET_Y - 2);

    /* waterfall */
    v_wf_buf = heap_caps_calloc(WRX_FFT_W * WF_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    v_wf = lv_canvas_create(scr);
    if (v_wf_buf) lv_canvas_set_buffer(v_wf, v_wf_buf, WRX_FFT_W, WF_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(v_wf, 0, WF_Y);
    lv_obj_remove_flag(v_wf, LV_OBJ_FLAG_CLICKABLE);
    v_over = lv_obj_create(scr);
    lv_obj_remove_style_all(v_over);
    lv_obj_set_size(v_over, WRX_FFT_W, WF_H);
    lv_obj_set_pos(v_over, 0, WF_Y);
    lv_obj_add_flag(v_over, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_over, over_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(v_over, wf_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    ui_bubble(v_over);
    v_zoom = lv_button_create(scr);
    lv_obj_set_size(v_zoom, 34, 20);
    lv_obj_set_pos(v_zoom, 240 - 36, WF_Y + 2);
    lv_obj_set_style_radius(v_zoom, 6, 0);
    lv_obj_set_style_shadow_width(v_zoom, 0, 0);
    lv_obj_set_style_bg_color(v_zoom, C_BG, 0);
    lv_obj_set_style_bg_opa(v_zoom, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(v_zoom, 0, 0);
    lv_obj_set_ext_click_area(v_zoom, 6);
    lv_obj_add_event_cb(v_zoom, zoom_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(v_zoom);
    char zb[8];
    snprintf(zb, sizeof zb, "\xC3\x97%d", m_zoom);
    lv_obj_center(ui_label(v_zoom, &font_small_11, C_FG, zb));
    v_status = lv_obj_create(scr);
    lv_obj_remove_style_all(v_status);
    lv_obj_set_size(v_status, 240, WF_H + 4);
    lv_obj_set_pos(v_status, 0, WF_Y - 2);
    lv_obj_set_style_bg_color(v_status, C_BG, 0);
    lv_obj_set_style_bg_opa(v_status, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(v_status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v_status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(v_status, 2, 0);
    lv_obj_remove_flag(v_status, LV_OBJ_FLAG_SCROLLABLE);
    v_st1 = ui_label(v_status, &font_text_16, C_FG, "");
    v_st2 = ui_label(v_status, &font_small_11, C_DIM, "");
    for (int k = 0; k < 2; k++) {
        lv_obj_t *l = k ? v_st2 : v_st1;
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, 228);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        ui_bubble(l);
    }
    lv_obj_add_flag(v_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_status, status_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(v_status);

    /* boutons */
    v_minus = round_btn(scr, "-", minus_cb, C_OFF, -78, true);
    v_play = round_btn(scr, LV_SYMBOL_PLAY, play_cb, lv_color_hex(0x16A34A), 0, false);
    v_play_lbl = lv_obj_get_child(v_play, 0);
    v_plus = round_btn(scr, "+", plus_cb, C_OFF, 78, true);

    build_setup(scr);
    build_sheet(scr);
    v_tick = lv_timer_create(tick_cb, 100, NULL);
    if (m_want && !webrx_running()) start();
    refresh(true);
}

static void destroy(void)
{
    webrx_fft_enable(false, m_zoom);
    if (webrx_running()) webrx_stop();
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    free(v_wf_buf);
    v_wf_buf = NULL;
    v_scr = v_dot = v_prof = v_dot_sep = v_name = v_mod = v_mod_lbl = v_meter = v_meter_lbl = NULL;
    v_wf = v_over = v_zoom = v_status = v_st1 = v_st2 = v_play = v_play_lbl = v_minus = v_plus = v_setup = NULL;
    v_sheet = v_sheet_title = v_sheet_list = v_spk = NULL;
    v_vol = v_vol_lbl = v_sq = v_sq_lbl = v_sq_sig = NULL;
    v_sheet_kind = SH_NONE;
    memset(v_dig, 0, sizeof v_dig);
}

static void on_show(void)
{
    webrx_fft_enable(true, m_zoom);
    if (v_tick) lv_timer_resume(v_tick);
    if (m_want && !webrx_running() && v_scr) start();
    refresh(true);
}

/* écran éteint (ou notification par-dessus) : on continue d'écouter ; on
 * quitte l'app : arrêt */
static void on_hide(void)
{
    webrx_fft_enable(false, m_zoom);
    if (v_tick) lv_timer_pause(v_tick);
    if (app_leaving_for_good()) {
        sheet_close();
        if (webrx_running()) webrx_stop();
    }
}

static bool busy(void) { return webrx_running(); }

/* écoute en cours : écran gardé allumé (fréquence, S-mètre, waterfall) */
static bool keep_on(void) { return webrx_running(); }

const app_t app_webrx = {
    .name = "WebRX",
    .icon = SYM_HEADPHONES,
    .color = 0x10B981,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .busy = busy,
    .keep_screen_on = keep_on,
};
