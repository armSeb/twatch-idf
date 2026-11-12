/*
 * app_wifi.c — analyseur Wi-Fi : réseaux à portée (RSSI, canal, sécurité),
 * occupation des canaux 2,4 GHz, fiche détaillée de chaque point d'accès.
 *
 * Énergie : le Wi-Fi n'existe que tant que l'app est visible, écran allumé.
 *   on_show  → esp_wifi_init (mode station, rien n'est enregistré en NVS)
 *   relève   → esp_wifi_start + balayage actif (~1,2 s), puis esp_wifi_stop :
 *              la radio est coupée entre deux relèves (4 s)
 *   on_hide  → (écran éteint, autre écran, autre app) arrêt et esp_wifi_deinit,
 *              toute la mémoire du pilote est rendue.
 * Le Bluetooth reste connecté pendant les balayages (coexistence logicielle).
 *
 * Le modèle (points d'accès, historique RSSI) vit en PSRAM et survit à la mise
 * en veille : au réveil, la liste réapparaît telle quelle puis se rafraîchit.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "ota.h"
#include "ble.h"
#include "power.h"
#include "display.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "src/misc/lv_area_private.h"   /* lv_area_intersect */

static const char *TAG = "wifi_app";

#define MAX_AP      40
#define MAX_REC     40
#define HIST        24          /* échantillons RSSI gardés par AP */
#define SCAN_GAP_MS 4000        /* radio coupée entre deux relèves */
#define SCAN_TMO_MS 6000
#define STALE_SCANS 3           /* disparu depuis 3 relèves → retiré */
#define LIST_ROWS   30
#define EV_SCAN_DONE 1

extern const app_t app_wifi;

/* ------------------------------------------------------------------ */
/* Modèle                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  ch;
    uint8_t  second;            /* wifi_second_chan_t */
    uint8_t  bw;                /* wifi_bandwidth_t */
    uint8_t  auth, pcipher, gcipher;
    uint8_t  b : 1, g : 1, n : 1, ax : 1, lr : 1, wps : 1;
    char     cc[3];
    uint8_t  cc_first, cc_num;
    int8_t   rssi;              /* dernière mesure */
    int8_t   best;
    int8_t   hist[HIST];
    uint8_t  nh;
    uint32_t seen;              /* numéro de relève */
    int64_t  seen_us;
} ap_t;

static ap_t   *s_ap;            /* MAX_AP entrées, PSRAM */
static int     s_nap;
static int     s_order[MAX_AP]; /* indices triés par RSSI */
static uint32_t s_scan_no;
static wifi_ap_record_t *s_rec; /* tampon de relève, alloué pendant la vie du Wi-Fi */

static bool    s_inited, s_started, s_scanning, s_paused, s_visible, s_err_mem;
static bool    s_ble_paused;   /* Bluetooth arrêté par l'app faute de RAM, à relancer en sortant */
static void    ble_resume(void);
static int64_t s_next_us, s_scan_t0;
static esp_event_handler_instance_t s_evh;
static uint8_t s_det[6];        /* AP affiché en détail */
static bool    s_det_open;

/* ------------------------------------------------------------------ */
/* Vue                                                                  */
/* ------------------------------------------------------------------ */
static lv_obj_t *v_scr, *v_status, *v_tabs, *v_list, *v_chart, *v_best, *v_empty;
static lv_timer_t *v_tick;
static int v_tab;               /* 0 réseaux, 1 canaux */
typedef struct { lv_obj_t *row, *bar, *ssid, *sub, *rssi, *lock; } row_t;
static row_t v_rows[LIST_ROWS];
static int v_nrows;
static bool v_list_dirty;
/* détail */
static lv_obj_t *v_det, *v_det_title, *v_det_rssi, *v_det_q, *v_det_spark, *v_det_val[10];

static ap_t *ap_find(const uint8_t *bssid)
{
    for (int i = 0; i < s_nap; i++) if (!memcmp(s_ap[i].bssid, bssid, 6)) return &s_ap[i];
    return NULL;
}

static lv_color_t q_color(int rssi)
{
    return rssi >= -60 ? C_OK : rssi >= -72 ? lv_color_hex(0xA3E635) : rssi >= -82 ? C_WARN : C_BAD;
}

static const char *q_text(int rssi)
{
    return rssi >= -55 ? _("Excellent") : rssi >= -67 ? _("Bon") : rssi >= -77 ? _("Moyen") : _("Faible");
}

static const char *auth_str(uint8_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return _("Ouvert");
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent.";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    case WIFI_AUTH_OWE:             return "OWE";
    case WIFI_AUTH_WPA3_ENT_192:    return "WPA3-Ent. 192";
    case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3-Ent.";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "WPA2/3-Ent.";
    case WIFI_AUTH_WPA_ENTERPRISE:  return "WPA-Ent.";
    case WIFI_AUTH_DPP:             return "DPP";
    default:                        return "?";
    }
}

static const char *cipher_str(uint8_t c)
{
    switch (c) {
    case WIFI_CIPHER_TYPE_NONE:      return "—";
    case WIFI_CIPHER_TYPE_WEP40:     return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104:    return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP:      return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP:      return "CCMP";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP+CCMP";
    case WIFI_CIPHER_TYPE_GCMP:      return "GCMP";
    case WIFI_CIPHER_TYPE_GCMP256:   return "GCMP-256";
    case WIFI_CIPHER_TYPE_SMS4:      return "SMS4";
    default:                         return "?";
    }
}

/* centre et demi-largeur (en canaux) de l'occupation spectrale */
static void ap_span(const ap_t *a, float *center, float *half)
{
    bool ht40 = a->bw == WIFI_BW_HT40 || a->second == WIFI_SECOND_CHAN_ABOVE || a->second == WIFI_SECOND_CHAN_BELOW;
    *center = a->ch;
    *half = 2.0f;
    if (ht40) {
        *center += a->second == WIFI_SECOND_CHAN_BELOW ? -2.0f : 2.0f;
        *half = 4.0f;
    }
}

static int ap_width_mhz(const ap_t *a)
{
    float c, h;
    ap_span(a, &c, &h);
    return h > 3 ? 40 : 20;
}

static uint32_t ap_color_hex(const ap_t *a)
{
    static const uint32_t PAL[] = { 0x3B82F6, 0x22C55E, 0xF59E0B, 0xEC4899, 0x14B8A6,
                                    0xA855F7, 0xEF4444, 0x84CC16, 0x06B6D4, 0xF97316 };
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ a->bssid[i]) * 16777619u;
    return PAL[h % (sizeof(PAL) / sizeof(PAL[0]))];
}

static void sort_aps(void)
{
    for (int i = 0; i < s_nap; i++) s_order[i] = i;
    for (int i = 1; i < s_nap; i++) {           /* insertion : 40 éléments au plus */
        int k = s_order[i], j = i - 1;
        while (j >= 0) {
            const ap_t *x = &s_ap[s_order[j]], *y = &s_ap[k];
            bool xs = x->seen != s_scan_no, ys = y->seen != s_scan_no;
            /* visibles d'abord, puis RSSI décroissant */
            if (xs > ys || (xs == ys && x->rssi < y->rssi)) { s_order[j + 1] = s_order[j]; j--; }
            else break;
        }
        s_order[j + 1] = k;
    }
}

static void merge_records(const wifi_ap_record_t *r, int n)
{
    s_scan_no++;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        ap_t *a = ap_find(r[i].bssid);
        if (!a) {
            if (s_nap < MAX_AP) a = &s_ap[s_nap++];
            else {                                  /* plein : remplace le plus faible */
                int w = 0;
                for (int k = 1; k < s_nap; k++) if (s_ap[k].rssi < s_ap[w].rssi) w = k;
                if (s_ap[w].rssi >= r[i].rssi) continue;
                a = &s_ap[w];
            }
            memset(a, 0, sizeof(*a));
            memcpy(a->bssid, r[i].bssid, 6);
            a->best = -127;
        }
        memcpy(a->ssid, r[i].ssid, 32);
        a->ssid[32] = 0;
        a->ch = r[i].primary;
        a->second = r[i].second;
        a->bw = r[i].bandwidth;
        a->auth = r[i].authmode;
        a->pcipher = r[i].pairwise_cipher;
        a->gcipher = r[i].group_cipher;
        a->b = r[i].phy_11b; a->g = r[i].phy_11g; a->n = r[i].phy_11n;
        a->ax = r[i].phy_11ax; a->lr = r[i].phy_lr; a->wps = r[i].wps;
        memcpy(a->cc, r[i].country.cc, 2);
        a->cc[2] = 0;
        if (a->cc[0] < 'A' || a->cc[0] > 'Z') a->cc[0] = 0;
        a->cc_first = r[i].country.schan;
        a->cc_num = r[i].country.nchan;
        a->rssi = r[i].rssi > 0 ? 0 : r[i].rssi;
        if (a->rssi > a->best) a->best = a->rssi;
        if (a->nh < HIST) a->hist[a->nh++] = a->rssi;
        else { memmove(a->hist, a->hist + 1, HIST - 1); a->hist[HIST - 1] = a->rssi; }
        a->seen = s_scan_no;
        a->seen_us = now;
    }
    /* oubli des AP disparus */
    for (int i = 0; i < s_nap;) {
        if (s_scan_no - s_ap[i].seen >= STALE_SCANS) s_ap[i] = s_ap[--s_nap];
        else i++;
    }
    sort_aps();
}

/* ------------------------------------------------------------------ */
/* Pilote Wi-Fi                                                         */
/* ------------------------------------------------------------------ */
static void scan_done_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    app_post_event(&app_wifi, EV_SCAN_DONE);
}

static bool wifi_up(void)
{
    if (s_inited) return true;
    if (ota_active()) return false;
    s_err_mem = false;
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { s_err_mem = true; return false; }
    if (!s_rec) {
        s_rec = heap_caps_malloc(sizeof(wifi_ap_record_t) * MAX_REC, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rec) s_rec = malloc(sizeof(wifi_ap_record_t) * MAX_REC);
        if (!s_rec) { s_err_mem = true; return false; }
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;                        /* rien à mémoriser */
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK && ble_running()) {
        /* pas assez de RAM interne à côté du Bluetooth : on le suspend le
         * temps de l'analyse (relancé dans wifi_down) et on réessaie */
        ESP_LOGW(TAG, "esp_wifi_init: %s, Bluetooth suspendu", esp_err_to_name(e));
        ble_diag("Wi-Fi : Bluetooth suspendu (RAM)");
        ble_stop();
        if (!ble_running()) {
            s_ble_paused = true;
            ui_toast(_("Bluetooth en pause pendant l'analyse"));
            esp_wifi_deinit();                   /* au cas où l'échec ait laissé un état partiel */
            e = esp_wifi_init(&cfg);
        }
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_init: %s (interne libre %u, bloc %u)", esp_err_to_name(e),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_err_mem = true;
        ble_resume();
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_country_code("FR", false);    /* canaux 1 à 13 */
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_done_handler, NULL, &s_evh);
    power_set_cpu_max(160);                    /* radio en marche : 80 MHz suffisent écran éteint */
    s_inited = true;
    return true;
}

static void wifi_down(void)
{
    if (!s_inited) return;
    if (s_scanning) esp_wifi_scan_stop();
    if (s_started) esp_wifi_stop();
    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, s_evh);
    esp_wifi_deinit();
    power_set_cpu_max(0);
    s_inited = s_started = s_scanning = false;
    free(s_rec);
    s_rec = NULL;
    /* Bluetooth suspendu : rendu en quittant l'app, pas pendant une simple
     * mise en veille sur l'app (on éviterait mal des arrêts/relances en boucle) */
    if (s_ble_paused && display_is_on()) ble_resume();
}

static void ble_resume(void)
{
    if (!s_ble_paused) return;
    s_ble_paused = false;
    ble_start();
}

static void scan_kick(void)
{
    if (!s_inited || s_scanning) return;
    if (!s_started) {
        if (esp_wifi_start() != ESP_OK) return;
        s_started = true;
    }
    wifi_scan_config_t sc = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 40, .max = 90 },
    };
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        s_scanning = true;
        s_scan_t0 = esp_timer_get_time();
    } else {
        s_next_us = esp_timer_get_time() + 1000000;
    }
}

static void radio_off(void)
{
    if (s_started) { esp_wifi_stop(); s_started = false; }
}

/* ------------------------------------------------------------------ */
/* Affichage                                                            */
/* ------------------------------------------------------------------ */
static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void ssid_text(const ap_t *a, char *out, size_t n)
{
    if (a->ssid[0]) snprintf(out, n, "%s", a->ssid);
    else snprintf(out, n, "%s", _("(réseau masqué)"));
}

static void status_refresh(void)
{
    if (!v_status) return;
    int vis = 0;
    for (int i = 0; i < s_nap; i++) if (s_ap[i].seen == s_scan_no) vis++;
    char b[32];
    snprintf(b, sizeof b, "%s %d", s_paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE, vis);
    set_text(lv_obj_get_child(v_status, 0), b);
    lv_obj_set_style_bg_color(v_status, s_scanning ? lv_color_hex(0x1E3A8A) : C_CARD, 0);
}

static void row_click_cb(lv_event_t *e);

static void row_make(int i)
{
    row_t *r = &v_rows[i];
    r->row = lv_obj_create(v_list);
    lv_obj_remove_style_all(r->row);
    lv_obj_set_size(r->row, 228, 48);
    lv_obj_set_style_bg_color(r->row, C_CARD, 0);
    lv_obj_set_style_bg_opa(r->row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r->row, lv_color_hex(0x2A2E37), LV_STATE_PRESSED);
    lv_obj_set_style_radius(r->row, 12, 0);
    lv_obj_set_style_clip_corner(r->row, true, 0);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r->row, row_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    ui_bubble(r->row);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    r->bar = lv_obj_create(r->row);
    lv_obj_remove_style_all(r->bar);
    lv_obj_set_size(r->bar, 5, 48);
    lv_obj_set_style_bg_opa(r->bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(r->bar, LV_OBJ_FLAG_CLICKABLE);

    r->ssid = ui_label(r->row, &font_text_16, C_FG, "");
    lv_label_set_long_mode(r->ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_size(r->ssid, 156, 20);
    lv_obj_set_pos(r->ssid, 14, 5);

    r->sub = ui_label(r->row, &font_small_11, C_DIM, "");
    lv_label_set_long_mode(r->sub, LV_LABEL_LONG_DOT);
    lv_obj_set_size(r->sub, 184, 14);
    lv_obj_set_pos(r->sub, 14, 28);

    r->rssi = ui_label(r->row, &font_title_20, C_FG, "");
    lv_obj_align(r->rssi, LV_ALIGN_TOP_RIGHT, -10, 3);

    r->lock = ui_label(r->row, &font_small_11, C_DIM, "");
    lv_obj_set_style_text_font(r->lock, &font_text_16, 0);
    lv_obj_align(r->lock, LV_ALIGN_BOTTOM_RIGHT, -12, -3);
}

static void list_refresh(void)
{
    if (!v_list) return;
    if (lv_obj_is_scrolling(v_list)) { v_list_dirty = true; return; }   /* pas sous le doigt */
    v_list_dirty = false;
    int n = s_nap < LIST_ROWS ? s_nap : LIST_ROWS;
    while (v_nrows < n) row_make(v_nrows++);
    for (int i = 0; i < v_nrows; i++) {
        row_t *r = &v_rows[i];
        if (i >= n) { lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        const ap_t *a = &s_ap[s_order[i]];
        bool here = a->seen == s_scan_no;
        char b[64];
        ssid_text(a, b, sizeof b);
        set_text(r->ssid, b);
        lv_obj_set_style_text_color(r->ssid, a->ssid[0] ? C_FG : C_DIM, 0);
        snprintf(b, sizeof b, _("canal %d · %s · %d MHz"), a->ch, auth_str(a->auth), ap_width_mhz(a));
        set_text(r->sub, b);
        snprintf(b, sizeof b, "%d", a->rssi);
        set_text(r->rssi, b);
        lv_color_t qc = q_color(a->rssi);
        lv_obj_set_style_text_color(r->rssi, here ? qc : C_OFF, 0);
        lv_obj_set_style_bg_color(r->bar, qc, 0);
        lv_obj_set_style_bg_opa(r->bar, here ? LV_OPA_COVER : LV_OPA_30, 0);
        set_text(r->lock, a->auth == WIFI_AUTH_OPEN ? "" : SYM_LOCK);
        lv_obj_set_style_opa(r->ssid, here ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if (v_empty) {
        const char *t = s_err_mem ? _("Wi-Fi indisponible :\nmémoire insuffisante") :
                        ota_active() ? _("Mise à jour en cours") :
                        s_nap ? "" : s_paused ? _("En pause") : _("Recherche…");
        set_text(v_empty, t);
        if (t[0]) lv_obj_remove_flag(v_empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(v_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

/* encombrement des canaux 1, 6, 11 : somme des puissances qui se recouvrent */
static void best_refresh(void)
{
    if (!v_best) return;
    static const int CH[3] = { 1, 6, 11 };
    double load[3] = { 0 };
    int cnt[3] = { 0 };
    for (int i = 0; i < s_nap; i++) {
        const ap_t *a = &s_ap[i];
        if (a->seen != s_scan_no) continue;
        float c, h;
        ap_span(a, &c, &h);
        for (int k = 0; k < 3; k++) {
            float d = fabsf(CH[k] - c);
            if (d >= h + 2) continue;                /* pas de recouvrement */
            double w = 1.0 - d / (h + 2);
            load[k] += w * pow(10.0, a->rssi / 10.0);
            cnt[k]++;
        }
    }
    int best = 0;
    for (int k = 1; k < 3; k++) if (load[k] < load[best]) best = k;
    char b[96];
    if (!s_nap) snprintf(b, sizeof b, "%s", s_paused ? _("En pause") : _("Recherche…"));
    else snprintf(b, sizeof b, _("Le plus libre : canal %d\n1 : %d   ·   6 : %d   ·   11 : %d réseaux"),
                  CH[best], cnt[0], cnt[1], cnt[2]);
    set_text(v_best, b);
}

static void chart_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t A;
    lv_obj_get_coords(o, &A);
    const int top = A.y1 + 4, bot = A.y2 - 16;   /* bas : numéros de canaux */
    const int x0 = A.x1 + 4, w = lv_area_get_width(&A) - 8;
    const float unit = w / 16.0f;                /* canaux -1 … 15 */
#define XC(c) ((float)x0 + ((c) + 1.0f) * unit)
#define YR(r) ((float)bot - ((float)(r) + 95.0f) / 60.0f * (float)(bot - top))

    /* grille : -90, -70, -50 dBm */
    lv_draw_line_dsc_t gl;
    lv_draw_line_dsc_init(&gl);
    gl.color = lv_color_hex(0x2A2E37);
    gl.width = 1;
    gl.dash_width = 3;
    gl.dash_gap = 3;
    lv_draw_label_dsc_t lab;
    lv_draw_label_dsc_init(&lab);
    lab.font = &font_small_11;
    lab.color = lv_color_hex(0x5A606B);
    static const int GRID[] = { -90, -70, -50 };
    static const char *const GTXT[] = { "-90", "-70", "-50" };
    for (int k = 0; k < 3; k++) {
        float y = YR(GRID[k]);
        gl.p1.x = x0; gl.p1.y = y; gl.p2.x = x0 + w; gl.p2.y = y;
        lv_draw_line(layer, &gl);
        lv_area_t la = { x0, (int)y - 12, x0 + 26, (int)y - 1 };
        lab.text = GTXT[k];
        lv_draw_label(layer, &lab, &la);
    }
    /* axe des canaux */
    for (int c = 1; c <= 13; c++) {
        char t[4];
        snprintf(t, sizeof t, "%d", c);
        bool key = c == 1 || c == 6 || c == 11;
        lab.color = key ? C_FG : lv_color_hex(0x5A606B);
        lab.align = LV_TEXT_ALIGN_CENTER;
        lab.text = t;
        lab.text_local = 1;
        lv_area_t la = { (int)XC(c) - 8, bot + 2, (int)XC(c) + 8, bot + 14 };
        lv_draw_label(layer, &lab, &la);
    }
    lab.text_local = 0;

    /* une « colline » par AP visible, du plus faible au plus fort */
    for (int oi = s_nap - 1; oi >= 0; oi--) {
        const ap_t *a = &s_ap[s_order[oi]];
        if (a->seen != s_scan_no) continue;
        float c, h;
        ap_span(a, &c, &h);
        float xa = XC(c - h), xb = XC(c - h + 0.5f), xc = XC(c + h - 0.5f), xd = XC(c + h);
        float yp = YR(a->rssi < -95 ? -95 : a->rssi > -35 ? -35 : a->rssi), yb = (float)bot;
        lv_color_t col = lv_color_hex(ap_color_hex(a));

        lv_draw_triangle_dsc_t td;
        lv_draw_triangle_dsc_init(&td);
        td.bg_color = col;
        td.bg_opa = LV_OPA_20;
        td.p[0].x = xa; td.p[0].y = yb; td.p[1].x = xb; td.p[1].y = yp; td.p[2].x = xb; td.p[2].y = yb;
        lv_draw_triangle(layer, &td);
        td.p[0].x = xd; td.p[0].y = yb; td.p[1].x = xc; td.p[1].y = yp; td.p[2].x = xc; td.p[2].y = yb;
        lv_draw_triangle(layer, &td);
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = col;
        rd.bg_opa = LV_OPA_20;
        lv_area_t ra = { (int)xb, (int)yp, (int)xc, bot };
        lv_draw_rect(layer, &rd, &ra);

        lv_draw_line_dsc_t ln;
        lv_draw_line_dsc_init(&ln);
        ln.color = col;
        ln.width = 2;
        ln.round_start = ln.round_end = 1;
        ln.p1.x = xa; ln.p1.y = yb; ln.p2.x = xb; ln.p2.y = yp; lv_draw_line(layer, &ln);
        ln.p1 = ln.p2; ln.p2.x = xc; ln.p2.y = yp; lv_draw_line(layer, &ln);
        ln.p1 = ln.p2; ln.p2.x = xd; ln.p2.y = yb; lv_draw_line(layer, &ln);
    }
    /* noms des 4 plus forts au-dessus de leur sommet */
    lab.align = LV_TEXT_ALIGN_CENTER;
    int named = 0;
    lv_area_t placed[4];
    for (int oi = 0; oi < s_nap && named < 4; oi++) {
        const ap_t *a = &s_ap[s_order[oi]];
        if (a->seen != s_scan_no) continue;
        float c, h;
        ap_span(a, &c, &h);
        char t[14];
        if (a->ssid[0]) snprintf(t, sizeof t, "%.12s", a->ssid);
        else snprintf(t, sizeof t, "%s", "(?)");
        int rr = a->rssi > -35 ? -35 : a->rssi;
        int x = (int)XC(c), y0 = (int)YR(rr);
        lab.color = lv_color_hex(ap_color_hex(a));
        lab.text = t;
        lab.text_local = 1;
        /* au-dessus du sommet, sinon juste dessous, sinon plus haut ; omise
         * si elle chevauche toujours une autre étiquette */
        static const int DY[] = { -14, 3, -27, 16 };
        lv_area_t la;
        bool ok = false;
        for (int t = 0; t < 4 && !ok; t++) {
            int y = y0 + DY[t];
            if (y < A.y1 || y + 12 > bot) continue;
            la.x1 = x - 40; la.x2 = x + 40; la.y1 = y; la.y2 = y + 12;
            if (la.x1 < A.x1) { la.x1 = A.x1; la.x2 = A.x1 + 80; }
            if (la.x2 > A.x2) { la.x2 = A.x2; la.x1 = A.x2 - 80; }
            ok = true;
            for (int k = 0; k < named && ok; k++) {
                lv_area_t t2;
                if (lv_area_intersect(&t2, &la, &placed[k])) ok = false;
            }
        }
        if (!ok) continue;
        lv_draw_label(layer, &lab, &la);
        placed[named++] = la;
    }
#undef XC
#undef YR
}

static void spark_draw_cb(lv_event_t *e)
{
    const ap_t *a = ap_find(s_det);
    if (!a || a->nh < 2) return;
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t A;
    lv_obj_get_coords(o, &A);
    int lo = 0, hi = -127;
    for (int i = 0; i < a->nh; i++) { if (a->hist[i] < lo) lo = a->hist[i]; if (a->hist[i] > hi) hi = a->hist[i]; }
    if (hi - lo < 10) { int m = (hi + lo) / 2; lo = m - 5; hi = m + 5; }
    float dx = (float)(lv_area_get_width(&A) - 4) / (HIST - 1);
    float x0 = A.x2 - 2 - dx * (a->nh - 1);
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = q_color(a->rssi);
    ln.width = 2;
    ln.round_start = ln.round_end = 1;
    for (int i = 1; i < a->nh; i++) {
        ln.p1.x = x0 + dx * (i - 1);
        ln.p1.y = A.y2 - 2 - (float)(a->hist[i - 1] - lo) / (hi - lo) * (lv_area_get_height(&A) - 4);
        ln.p2.x = x0 + dx * i;
        ln.p2.y = A.y2 - 2 - (float)(a->hist[i] - lo) / (hi - lo) * (lv_area_get_height(&A) - 4);
        lv_draw_line(layer, &ln);
    }
}

enum { K_BSSID, K_ADDR, K_CH, K_BW, K_SEC, K_CIPH, K_PHY, K_WPS, K_CC, K_SEEN, K_COUNT };

static void detail_refresh(void)
{
    if (!v_det || !s_det_open) return;
    const ap_t *a = ap_find(s_det);
    if (!a) {
        set_text(v_det_q, _("Hors de portée"));
        lv_obj_set_style_text_color(v_det_q, C_DIM, 0);
        return;
    }
    char b[64];
    ssid_text(a, b, sizeof b);
    set_text(v_det_title, b);
    bool here = a->seen == s_scan_no;
    snprintf(b, sizeof b, "%d", a->rssi);
    set_text(v_det_rssi, b);
    lv_obj_set_style_text_color(v_det_rssi, here ? q_color(a->rssi) : C_OFF, 0);
    snprintf(b, sizeof b, _("dBm · %s\nmeilleur %d"), here ? q_text(a->rssi) : _("perdu"), a->best);
    set_text(v_det_q, b);
    lv_obj_set_style_text_color(v_det_q, C_DIM, 0);
    lv_obj_invalidate(v_det_spark);

    snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X",
             a->bssid[0], a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4], a->bssid[5]);
    set_text(v_det_val[K_BSSID], b);
    set_text(v_det_val[K_ADDR], (a->bssid[0] & 0x02) ? _("locale (aléatoire)") : _("universelle (constructeur)"));
    snprintf(b, sizeof b, "%d · %d MHz", a->ch, a->ch == 14 ? 2484 : 2407 + 5 * a->ch);
    set_text(v_det_val[K_CH], b);
    if (ap_width_mhz(a) == 40)
        snprintf(b, sizeof b, _("40 MHz (canal second. %s)"), a->second == WIFI_SECOND_CHAN_BELOW ? "-" : "+");
    else snprintf(b, sizeof b, "20 MHz");
    set_text(v_det_val[K_BW], b);
    set_text(v_det_val[K_SEC], auth_str(a->auth));
    snprintf(b, sizeof b, "%s / %s", cipher_str(a->pcipher), cipher_str(a->gcipher));
    set_text(v_det_val[K_CIPH], b);
    snprintf(b, sizeof b, "802.11%s%s%s%s%s", a->b ? " b" : "", a->g ? " g" : "", a->n ? " n" : "",
             a->ax ? " ax" : "", a->lr ? " LR" : "");
    set_text(v_det_val[K_PHY], b);
    set_text(v_det_val[K_WPS], a->wps ? _("oui") : _("non"));
    if (a->cc[0]) snprintf(b, sizeof b, _("%s (canaux %d à %d)"), a->cc, a->cc_first, a->cc_first + a->cc_num - 1);
    else snprintf(b, sizeof b, "—");
    set_text(v_det_val[K_CC], b);
    int s = (int)((esp_timer_get_time() - a->seen_us) / 1000000);
    if (s < 3) snprintf(b, sizeof b, "%s", _("à l'instant"));
    else snprintf(b, sizeof b, _("il y a %d s"), s);
    set_text(v_det_val[K_SEEN], b);
}

static void view_refresh(void)
{
    status_refresh();
    if (v_tab == 0) list_refresh();
    else { if (v_chart) lv_obj_invalidate(v_chart); best_refresh(); }
    detail_refresh();
}

static void show_tab(int t)
{
    v_tab = t;
    if (!v_list) return;
    if (t == 0) { lv_obj_remove_flag(v_list, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(v_chart, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(v_best, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(v_list, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(v_chart, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(v_best, LV_OBJ_FLAG_HIDDEN); }
    if (t == 1 && v_empty) lv_obj_add_flag(v_empty, LV_OBJ_FLAG_HIDDEN);
    view_refresh();
}

/* ------------------------------------------------------------------ */
/* Interaction                                                          */
/* ------------------------------------------------------------------ */
static void det_close(void)
{
    s_det_open = false;
    if (v_det) lv_obj_add_flag(v_det, LV_OBJ_FLAG_HIDDEN);
}

static void det_back_cb(lv_event_t *e) { det_close(); }

static void det_gesture_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in && lv_indev_get_gesture_dir(in) == LV_DIR_RIGHT) {
        det_close();
        lv_indev_wait_release(in);
    }
}

static void row_click_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_nap || !v_det) return;
    memcpy(s_det, s_ap[s_order[i]].bssid, 6);
    s_det_open = true;
    lv_obj_scroll_to_y(lv_obj_get_child(v_det, 1), 0, LV_ANIM_OFF);
    lv_obj_remove_flag(v_det, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v_det);
    detail_refresh();
}

static void tabs_cb(lv_event_t *e)
{
    int t = (int)lv_buttonmatrix_get_selected_button(v_tabs);
    if (t == 0 || t == 1) show_tab(t);
}

static void pause_cb(lv_event_t *e)
{
    s_paused = !s_paused;
    if (s_paused) {
        if (s_scanning) { esp_wifi_scan_stop(); s_scanning = false; }
        radio_off();
    } else {
        s_next_us = 0;
    }
    view_refresh();
}

static void tick_cb(lv_timer_t *t)
{
    if (!s_visible) return;
    int64_t now = esp_timer_get_time();
    if (!s_inited && !s_err_mem && !ota_active()) {
        wifi_up();
        if (s_err_mem) view_refresh();
    }
    if (s_scanning && now - s_scan_t0 > SCAN_TMO_MS * 1000LL) {   /* relève bloquée : on repart */
        esp_wifi_scan_stop();
        s_scanning = false;
        radio_off();
        s_next_us = now + 500000;
    }
    if (s_inited && !s_paused && !s_scanning && now >= s_next_us) {
        scan_kick();
        status_refresh();
    }
    if (v_list_dirty && v_tab == 0) list_refresh();
    static int64_t s_det_t;
    if (s_det_open && now - s_det_t > 1000000) { s_det_t = now; detail_refresh(); }   /* « il y a N s » */
}

/* ------------------------------------------------------------------ */
/* Construction                                                         */
/* ------------------------------------------------------------------ */
static lv_obj_t *kv_row(lv_obj_t *col, const char *key)
{
    lv_obj_t *r = ui_container(col);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(r, 6, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x22252C), 0);
    ui_label(r, &font_small_11, C_DIM, key);
    lv_obj_t *v = ui_label(r, &font_text_16, C_FG, "");
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, lv_pct(100));
    return v;
}

static void build_detail(lv_obj_t *scr)
{
    v_det = lv_obj_create(scr);
    lv_obj_remove_style_all(v_det);
    lv_obj_set_size(v_det, 240, 240);
    lv_obj_set_style_bg_color(v_det, C_BG, 0);
    lv_obj_set_style_bg_opa(v_det, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v_det, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(v_det, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(v_det, det_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* en-tête « ‹ SSID » : ferme la fiche */
    lv_obj_t *hdr = lv_button_create(v_det);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, 240, 40);
    lv_obj_set_style_bg_color(hdr, C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(hdr, det_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ar = ui_label(hdr, &font_title_20, C_ACC, LV_SYMBOL_LEFT);
    lv_obj_align(ar, LV_ALIGN_LEFT_MID, 12, 0);
    v_det_title = ui_label(hdr, &font_title_20, C_FG, "");
    lv_label_set_long_mode(v_det_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v_det_title, 190);
    lv_obj_align(v_det_title, LV_ALIGN_LEFT_MID, 36, 0);

    lv_obj_t *col = ui_container(v_det);
    lv_obj_set_size(col, 240, 200);
    lv_obj_set_pos(col, 0, 40);
    lv_obj_set_style_pad_hor(col, 12, 0);
    lv_obj_set_style_pad_bottom(col, 12, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_ACTIVE);

    /* carte signal : grand RSSI, qualité, courbe */
    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), 64);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    ui_bubble(card);
    v_det_rssi = ui_label(card, &font_num_44, C_FG, "");
    lv_obj_align(v_det_rssi, LV_ALIGN_LEFT_MID, 8, 0);
    v_det_q = ui_label(card, &font_small_11, C_DIM, "");
    lv_obj_align(v_det_q, LV_ALIGN_LEFT_MID, 88, 0);
    v_det_spark = lv_obj_create(card);
    lv_obj_remove_style_all(v_det_spark);
    lv_obj_set_size(v_det_spark, 70, 44);
    lv_obj_align(v_det_spark, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_remove_flag(v_det_spark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_det_spark, spark_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    static const char *const KEYS[K_COUNT] = {
        N_("BSSID"), N_("Adresse"), N_("Canal"), N_("Largeur"), N_("Sécurité"),
        N_("Chiffrement (paires / groupe)"), N_("Normes"), N_("WPS"), N_("Pays annoncé"), N_("Vu"),
    };
    for (int k = 0; k < K_COUNT; k++) v_det_val[k] = kv_row(col, _(KEYS[k]));
}

static void create(lv_obj_t *scr)
{
    v_scr = scr;
    if (!s_ap) {
        s_ap = heap_caps_calloc(MAX_AP, sizeof(ap_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ap) s_ap = calloc(MAX_AP, sizeof(ap_t));
    }
    s_nap = 0;                                   /* nouvelle ouverture : vue fraîche */
    s_det_open = false;
    s_paused = false;
    s_next_us = 0;
    s_err_mem = false;

    int y = ui_header(scr, "Wi-Fi");

    /* pastille : pause/reprise + nombre de réseaux, bleue pendant un balayage */
    v_status = lv_button_create(scr);
    lv_obj_remove_style_all(v_status);
    lv_obj_set_size(v_status, 64, 28);
    lv_obj_align(v_status, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_radius(v_status, 14, 0);
    lv_obj_set_style_bg_opa(v_status, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(v_status, C_CARD, 0);
    lv_obj_add_event_cb(v_status, pause_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(v_status);
    lv_obj_t *sl = ui_label(v_status, &font_text_16, C_FG, "");
    lv_obj_center(sl);

    static const char *map[3];
    map[0] = _("Réseaux"); map[1] = _("Canaux"); map[2] = "";
    v_tabs = ui_segmented(scr, map, tabs_cb);
    lv_obj_set_size(v_tabs, 228, 32);
    lv_obj_align(v_tabs, LV_ALIGN_TOP_MID, 0, y);
    lv_buttonmatrix_set_button_ctrl(v_tabs, v_tab, LV_BUTTONMATRIX_CTRL_CHECKED);

    int cy = y + 36, ch = 240 - cy;
    v_list = lv_obj_create(scr);
    lv_obj_remove_style_all(v_list);
    lv_obj_set_size(v_list, 240, ch);
    lv_obj_set_pos(v_list, 0, cy);
    lv_obj_set_flex_flow(v_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(v_list, 4, 0);
    lv_obj_set_style_pad_bottom(v_list, 8, 0);
    lv_obj_set_scroll_dir(v_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v_list, LV_SCROLLBAR_MODE_ACTIVE);
    ui_bubble(v_list);
    v_nrows = 0;

    v_empty = ui_label(scr, &font_text_16, C_DIM, "");
    lv_obj_set_style_text_align(v_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v_empty, LV_ALIGN_CENTER, 0, 30);

    v_chart = lv_obj_create(scr);
    lv_obj_remove_style_all(v_chart);
    lv_obj_set_size(v_chart, 236, ch - 40);
    lv_obj_set_pos(v_chart, 2, cy);
    lv_obj_remove_flag(v_chart, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(v_chart, chart_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    ui_bubble(v_chart);

    v_best = ui_label(scr, &font_small_11, C_DIM, "");
    lv_obj_set_style_text_align(v_best, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(v_best, 228);
    lv_obj_align(v_best, LV_ALIGN_BOTTOM_MID, 0, -6);

    build_detail(scr);
    v_tick = lv_timer_create(tick_cb, 250, NULL);
    show_tab(v_tab);
}

static void destroy(void)
{
    wifi_down();
    ble_resume();
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    v_scr = v_status = v_tabs = v_list = v_chart = v_best = v_empty = NULL;
    v_det = v_det_title = v_det_rssi = v_det_q = v_det_spark = NULL;
    memset(v_det_val, 0, sizeof v_det_val);
    memset(v_rows, 0, sizeof v_rows);
    v_nrows = 0;
}

static void on_show(void)
{
    s_visible = true;
    s_next_us = 0;
    if (v_tick) lv_timer_resume(v_tick);
    view_refresh();
}

/* écran éteint, autre écran, autre app : plus de Wi-Fi du tout */
static void on_hide(void)
{
    s_visible = false;
    if (v_tick) lv_timer_pause(v_tick);
    wifi_down();
    s_err_mem = false;
    if (v_status) status_refresh();
}

static bool on_event(int code)
{
    if (code != EV_SCAN_DONE || !s_inited || !s_scanning) return false;
    s_scanning = false;
    uint16_t n = MAX_REC;
    if (s_rec && esp_wifi_scan_get_ap_records(&n, s_rec) == ESP_OK) merge_records(s_rec, n);
    else esp_wifi_clear_ap_list();
    radio_off();                                 /* radio coupée jusqu'à la prochaine relève */
    s_next_us = esp_timer_get_time() + SCAN_GAP_MS * 1000LL;
    if (s_visible) view_refresh();
    return false;
}

const app_t app_wifi = {
    .name = "Wi-Fi",
    .icon = SYM_WIFI,
    .color = 0x06B6D4,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .on_event = on_event,
};
