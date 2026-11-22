/*
 * app_remote.c — « Télécommande » : télécommandes infrarouges multiples,
 * décrites en JSON (remote_cfg.c), émises par la LED IR (ir_tx.c).
 *
 * Écran principal : les touches de la télécommande en cours, en grille
 * (1 à 4 colonnes, la grille défile si besoin). Toucher = envoyer ; garder
 * le doigt sur une touche « repeat » (volume…) = répéter ; appui long sur une
 * autre touche = sa fiche (code, tester, chercher, oublier). En haut à
 * droite : ≡ la liste des télécommandes (en changer, ajouter un modèle,
 * modifier sur le téléphone), ✎ le mode réglage (toucher une touche ouvre sa
 * fiche, même les touches « repeat »).
 *
 * La montre n'a pas de récepteur infrarouge : un code inconnu se cherche en
 * émettant. Balayage des codes (protocole et adresse de la télécommande s'ils
 * sont connus, sinon les plus courants), « ÇA A RÉAGI ! » au bon moment, puis
 * recherche par moitiés sur les codes des 2,5 dernières secondes. « Codes
 * probables » fait directement la recherche par moitiés sur les codes usuels
 * du protocole (Philips RC6, Samsung, Sony, télécommande chinoise NEC…).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "power.h"
#include "nvs.h"
#include "esp_timer.h"
#include "ota.h"
#include "ir_tx.h"
#include "remote_cfg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef ESP_PLATFORM
#define DELAY_MS(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#else
#define DELAY_MS(ms) ((void)0)
#endif

extern const app_t app_remote;
#define EV_TICK 1

/* ------------------------------------------------------------------ données */
static rc_set_t *S;                     /* les télécommandes (PSRAM) */
static int s_cur;                       /* télécommande affichée */
static int s_key;                       /* touche dont on montre la fiche / qu'on cherche */
static bool s_edit;

static rc_remote_t *R(void) { return &S->r[s_cur]; }
static rc_key_t *K(void) { return &R()->keys[s_key]; }

static void cur_save(void)
{
    nvs_handle_t h;
    if (nvs_open("remote", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "cur", (uint8_t)s_cur);
    nvs_commit(h);
    nvs_close(h);
}

static void cur_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("remote", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "cur", &v); nvs_close(h); }
    s_cur = v < S->n ? v : 0;
}

/* ------------------------------------------------------------------ recherche */
typedef struct { uint8_t proto; uint16_t addr; } target_t;
#define MAX_TARGETS 24
static target_t s_tg[MAX_TARGETS];
static int s_ntg;
static bool s_spiral;                   /* une seule cible : en partant du code connu */
static uint16_t s_hint;

typedef struct { ir_code_t c; int32_t idx; int64_t ms; } sent_t;
#define LOG_N 64
static sent_t s_log[LOG_N];
static int s_log_n;

typedef enum { ST_PAD, ST_LIST, ST_PRESETS, ST_KEY, ST_SCAN, ST_TRY, ST_ASK, ST_FOUND, ST_NONE, ST_DELETE } state_t;
static state_t st = ST_PAD;
static volatile int32_t s_pos;
static int32_t s_total;
static volatile bool s_run, s_stop;
static sent_t s_cand[LOG_N];
static int s_ncand, s_half;
static int s_del;                       /* télécommande à supprimer */

static const uint16_t NEC_ADDRS[] = { 0x00, 0x80, 0x01, 0x02, 0x03, 0x04, 0x08, 0x10, 0x20, 0x40, 0x7F, 0xFE, 0xFF };

static void targets_for(int proto)
{
    switch (proto) {
    case IR_NEC:
        for (size_t i = 0; i < sizeof NEC_ADDRS / sizeof NEC_ADDRS[0] && s_ntg < MAX_TARGETS; i++)
            s_tg[s_ntg++] = (target_t){ IR_NEC, ir_nec_std((uint8_t)NEC_ADDRS[i]) };
        if (s_ntg < MAX_TARGETS) s_tg[s_ntg++] = (target_t){ IR_NEC, 0xEF00 };
        break;
    case IR_SAMSUNG: s_tg[s_ntg++] = (target_t){ IR_SAMSUNG, 0x07 }; s_tg[s_ntg++] = (target_t){ IR_SAMSUNG, 0x05 }; break;
    case IR_RC5: s_tg[s_ntg++] = (target_t){ IR_RC5, 0x00 }; s_tg[s_ntg++] = (target_t){ IR_RC5, 0x05 }; break;
    case IR_RC6: s_tg[s_ntg++] = (target_t){ IR_RC6, 0x00 }; s_tg[s_ntg++] = (target_t){ IR_RC6, 0x04 }; break;
    default: s_tg[s_ntg++] = (target_t){ (uint8_t)proto, 0x01 }; break;
    }
}

/* cibles du balayage pour la touche s_key */
static void targets_build(void)
{
    rc_remote_t *r = R();
    rc_key_t *k = K();
    s_ntg = 0;
    s_spiral = false;
    int proto = k->own ? k->proto : r->proto;
    bool has_addr = k->own || r->has_addr;
    if (proto >= 0 && has_addr) {
        s_tg[s_ntg++] = (target_t){ (uint8_t)proto, k->own ? k->addr : r->addr };
        s_spiral = true;
        s_hint = 0;
        for (int i = 0; i < r->nkeys; i++) if (r->keys[i].has && !r->keys[i].own) { s_hint = r->keys[i].cmd; break; }
    } else if (proto >= 0) {
        targets_for(proto);
    } else {                                              /* protocole inconnu : les plus courants */
        s_tg[s_ntg++] = (target_t){ IR_NEC, ir_nec_std(0x00) };
        s_tg[s_ntg++] = (target_t){ IR_SAMSUNG, 0x07 };
        s_tg[s_ntg++] = (target_t){ IR_RC6, 0x00 };
        s_tg[s_ntg++] = (target_t){ IR_RC5, 0x00 };
        s_tg[s_ntg++] = (target_t){ IR_SONY12, 0x01 };
        int n0 = s_ntg;
        targets_for(IR_NEC);
        for (int i = n0; i < s_ntg; i++)                   /* NEC 00 déjà fait */
            if (s_tg[i].addr == ir_nec_std(0x00)) { memmove(&s_tg[i], &s_tg[i + 1], (size_t)(s_ntg - i - 1) * sizeof s_tg[0]); s_ntg--; break; }
    }
    s_total = 0;
    for (int i = 0; i < s_ntg; i++) s_total += ir_cmd_count(s_tg[i].proto);
}

static void scan_code(int32_t i, ir_code_t *c)
{
    for (int t = 0; t < s_ntg; t++) {
        int n = ir_cmd_count(s_tg[t].proto);
        if (i < n) {
            c->proto = s_tg[t].proto;
            c->addr = s_tg[t].addr;
            if (s_spiral) {                               /* h, h+1, h−1, h+2… */
                int d = (int)(i + 1) / 2;
                int v = i & 1 ? s_hint + d : s_hint - d;
                c->cmd = (uint16_t)(((v % n) + n) % n);
            } else c->cmd = (uint16_t)i;
            return;
        }
        i -= n;
    }
    *c = (ir_code_t){ 0 };
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void log_sent(const ir_code_t *c, int32_t idx)
{
    if (s_log_n == LOG_N) { memmove(s_log, s_log + 1, (LOG_N - 1) * sizeof s_log[0]); s_log_n--; }
    s_log[s_log_n++] = (sent_t){ *c, idx, now_ms() };
}

/* codes usuels par protocole, pour « Codes probables » */
static const uint8_t L_NEC[] = { 0x45, 0x46, 0x47, 0x44, 0x40, 0x43, 0x07, 0x15, 0x09, 0x16, 0x19, 0x0D, 0x0C, 0x18, 0x5E, 0x08, 0x1C, 0x5A, 0x42, 0x52, 0x4A,
                                 0x00, 0x01, 0x02, 0x03, 0x08, 0x0B, 0x10, 0x12 };
static const uint8_t L_RC6[] = { 0x0C, 0x0D, 0x10, 0x11, 0x20, 0x21, 0x4C, 0x4D, 0x38, 0x54, 0x5C, 0x58, 0x59, 0x5A, 0x5B, 0x0A, 0x40, 0x0F,
                                 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x2C, 0x30, 0x31, 0x6D, 0x6E, 0x6F, 0x70 };
static const uint8_t L_RC5[] = { 0x0C, 0x0D, 0x10, 0x11, 0x20, 0x21, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0F, 0x38, 0x3F };
static const uint8_t L_SAM[] = { 0x02, 0x0F, 0x07, 0x0B, 0x12, 0x10, 0x01, 0x68, 0x1A, 0x58, 0x60, 0x61, 0x65, 0x62, 0x2D, 0x79, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0A, 0x0C, 0x0D, 0x0E, 0x11 };
static const uint8_t L_SONY[] = { 0x15, 0x14, 0x12, 0x13, 0x10, 0x11, 0x25, 0x65, 0x74, 0x75, 0x33, 0x34, 0x60, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };

static bool likely_list(const uint8_t **l, int *n)
{
    ir_code_t c;
    rc_key_t *k = K();
    int proto = k->own ? k->proto : R()->proto;
    if (proto < 0 || !(k->own || R()->has_addr)) return false;
    (void)c;
    switch (proto) {
    case IR_NEC: *l = L_NEC; *n = sizeof L_NEC; return true;
    case IR_RC6: *l = L_RC6; *n = sizeof L_RC6; return true;
    case IR_RC5: *l = L_RC5; *n = sizeof L_RC5; return true;
    case IR_SAMSUNG: *l = L_SAM; *n = sizeof L_SAM; return true;
    default: *l = L_SONY; *n = sizeof L_SONY; return true;
    }
}

/* ------------------------------------------------------------------ envoi */
typedef enum { JOB_ONE, JOB_SCAN, JOB_LIST } job_t;
static job_t s_job;
static ir_code_t s_one;
static bool s_one_held;

static void send_task(void *arg)
{
    if (ir_tx_open() == ESP_OK) {
        switch (s_job) {
        case JOB_ONE:
            ir_tx_send(&s_one, s_one_held);
            break;
        case JOB_SCAN:
#ifndef ESP_PLATFORM
            for (int lim = 0; lim < 20 && !s_stop && s_pos < s_total; lim++) {   /* simulateur : un extrait */
#else
            while (!s_stop && s_pos < s_total) {
#endif
                ir_code_t c;
                scan_code(s_pos, &c);
                ir_tx_send(&c, false);
                log_sent(&c, s_pos);
                s_pos++;
                DELAY_MS(40);
            }
            break;
        case JOB_LIST:
            for (int i = 0; i < s_half && !s_stop; i++) {
                ir_tx_send(&s_cand[i].c, false);
                DELAY_MS(120);
            }
            break;
        }
        ir_tx_close();                                     /* rend le RMT et le verrou de fréquence */
    }
    s_run = false;
    app_post_event(&app_remote, EV_TICK);
#ifdef ESP_PLATFORM
    vTaskDelete(NULL);
#endif
}

static bool start_job(job_t j)
{
    if (s_run) return false;
    s_job = j;
    s_stop = false;
    s_run = true;
#ifdef ESP_PLATFORM
    if (xTaskCreate(send_task, "irtx", 3072, NULL, 4, NULL) != pdPASS) { s_run = false; return false; }
#else
    send_task(NULL);                                       /* simulateur : synchrone */
#endif
    return true;
}

static bool send_code(const ir_code_t *c, bool held)
{
    s_one = *c;
    s_one_held = held;
    return start_job(JOB_ONE);
}

/* ------------------------------------------------------------------ vue */
static lv_obj_t *v_scr, *v_title, *v_body, *v_status, *v_bar, *v_btn_list, *v_btn_edit;
static lv_timer_t *v_tick;
static void build(void);
static void build_async(void *p) { build(); }
/* reconstruit après l'événement en cours (le bouton touché disparaît) */
static void set_state(state_t s) { st = s; lv_async_call(build_async, NULL); }

static const uint32_t COLOR_HEX[RC_COLOR_N] = { 0x374151, 0x16A34A, 0xB91C1C, 0x2563EB, 0xCA8A04, 0xEA580C, 0x7C3AED };

static lv_obj_t *btn(lv_obj_t *p, const char *t, lv_color_t c, lv_event_cb_t cb, void *ud, int w, int h)
{
    lv_obj_t *b = ui_button(p, t, c, cb, ud);
    lv_obj_set_size(b, w, h);
    return b;
}

static lv_obj_t *text(const char *t, int y, lv_color_t c)
{
    lv_obj_t *l = ui_label(v_body, &font_text_16, c, t);
    lv_obj_set_width(l, 224);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static void place(int y) { lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 0, y); }
static void place_x(int x, int y) { lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, x, y); }

/* texte d'un code : « RC6 00 · 0C » */
static void code_str(char *b, size_t n, const ir_code_t *c)
{
    if (c->proto == IR_NEC && ir_nec_is_std(c->addr)) snprintf(b, n, "NEC %02X · %02X", c->addr & 0xFF, c->cmd);
    else if (c->proto == IR_NEC) snprintf(b, n, "NEC %04X · %02X", c->addr, c->cmd);
    else {
        char p[8];
        snprintf(p, sizeof p, "%s", ir_proto_name(c->proto));
        for (char *q = p; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        snprintf(b, n, "%s %02X · %02X", p, c->addr, c->cmd);
    }
}

/* --- touches */
static void key_event(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    rc_key_t *key = &R()->keys[k];
    ir_code_t c;
    bool known = rc_key_code(R(), key, &c);
    bool send = !s_edit && known;
    if (code == LV_EVENT_PRESSED) {                        /* répétées : dès l'appui */
        if (send && key->repeat && send_code(&c, false)) power_vibrate(VIB_SHORT);
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED_REPEAT) {
        if (send && key->repeat) send_code(&c, true);
        return;
    }
    if (code == LV_EVENT_SHORT_CLICKED && send) {          /* au relâcher : pas d'envoi en faisant défiler */
        if (!key->repeat && send_code(&c, false)) power_vibrate(VIB_SHORT);
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED && send && key->repeat) return;
    s_key = k;
    set_state(ST_KEY);
}

static void list_cb(lv_event_t *e) { s_stop = true; set_state(st == ST_LIST ? ST_PAD : ST_LIST); }
static void edit_cb(lv_event_t *e) { s_edit = !s_edit; set_state(ST_PAD); }

static void build_pad(void)
{
    rc_remote_t *r = R();
    lv_obj_add_flag(v_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(v_body, LV_DIR_VER);
    lv_obj_set_flex_flow(v_body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(v_body, 6, 0);
    lv_obj_set_style_pad_gap(v_body, 6, 0);
    int cols = r->cols ? r->cols : 3;
    int cw = (228 - (cols - 1) * 6) / cols;
    int h = cols == 1 ? 52 : cols == 2 ? 58 : cols == 3 ? 46 : 40;
    const lv_font_t *f = cols <= 2 ? &font_title_20 : &font_text_16;
    for (int i = 0; i < r->nkeys; i++) {
        rc_key_t *k = &r->keys[i];
        int w = cw * k->w + 6 * (k->w - 1);
        if (!k->label[0]) {                                /* case vide */
            lv_obj_t *sp = lv_obj_create(v_body);
            lv_obj_remove_style_all(sp);
            lv_obj_set_size(sp, w, h);
            continue;
        }
        ir_code_t c;
        bool known = rc_key_code(r, k, &c);
        char b[RC_LABEL_LEN + 4];
        snprintf(b, sizeof b, known ? "%s" : "%s ?", k->label);
        lv_color_t col = known ? lv_color_hex(COLOR_HEX[k->color % RC_COLOR_N]) : C_OFF;
        lv_obj_t *o = btn(v_body, b, col, NULL, NULL, w, h);
        lv_obj_t *lbl = lv_obj_get_child(o, 0);
        if (lbl) lv_obj_set_style_text_font(lbl, f, 0);
        if (s_edit) { lv_obj_set_style_border_width(o, 2, 0); lv_obj_set_style_border_color(o, lv_color_hex(0xFACC15), 0); }
        lv_obj_add_event_cb(o, key_event, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(o, key_event, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(o, key_event, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(o, key_event, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)i);
    }
    if (s_edit) {
        lv_obj_t *l = ui_label(v_body, &font_text_16, lv_color_hex(0xFACC15), _("Réglage : touche une touche pour sa fiche"));
        lv_obj_set_width(l, 228);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
}

/* --- liste des télécommandes */
static void pick_cb(lv_event_t *e)
{
    s_cur = (int)(intptr_t)lv_event_get_user_data(e);
    cur_save();
    s_edit = false;
    set_state(ST_PAD);
}

static void pick_long_cb(lv_event_t *e)
{
    s_del = (int)(intptr_t)lv_event_get_user_data(e);
    set_state(ST_DELETE);
}

static void presets_cb(lv_event_t *e) { set_state(ST_PRESETS); }
static void portal_cb(lv_event_t *e) { app_open_settings_page(&app_remote, OTA_PAGE_REMOTE); }

static void add_preset_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (rc_add_preset(S, i) == 0) {
        s_cur = S->n - 1;
        rc_save(S);
        cur_save();
        power_vibrate(VIB_SHORT);
    }
    set_state(ST_PAD);
}

static void del_yes_cb(lv_event_t *e)
{
    if (S->n > 1 && s_del < S->n) {
        memmove(&S->r[s_del], &S->r[s_del + 1], (size_t)(S->n - s_del - 1) * sizeof S->r[0]);
        S->n--;
        if (s_cur >= S->n) s_cur = S->n - 1;
        rc_save(S);
        cur_save();
    }
    set_state(ST_LIST);
}

static void to_list_cb(lv_event_t *e) { set_state(ST_LIST); }

static lv_obj_t *row(const char *t, const char *sub, lv_color_t c, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = btn(v_body, "", c, cb, ud, 228, sub ? 50 : 42);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l) {
        lv_label_set_text(l, t);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, sub ? -9 : 0);
    }
    if (sub) {
        lv_obj_t *s2 = ui_label(b, &font_text_16, C_DIM, sub);
        lv_obj_align(s2, LV_ALIGN_LEFT_MID, 6, 11);
    }
    return b;
}

static void build_list(void)
{
    lv_obj_add_flag(v_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(v_body, LV_DIR_VER);
    lv_obj_set_flex_flow(v_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(v_body, 6, 0);
    lv_obj_set_style_pad_gap(v_body, 6, 0);
    char sub[40];
    for (int i = 0; i < S->n; i++) {
        rc_remote_t *r = &S->r[i];
        int known = 0;
        for (int k = 0; k < r->nkeys; k++) known += r->keys[k].label[0] && r->keys[k].has;
        snprintf(sub, sizeof sub, "%s · %d %s", r->proto < 0 ? "?" : ir_proto_name(r->proto), known, _("touches"));
        lv_obj_t *b = row(r->name, sub, i == s_cur ? lv_color_hex(0x1E3A8A) : C_CARD, pick_cb, (void *)(intptr_t)i);
        if (S->n > 1) lv_obj_add_event_cb(b, pick_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    }
    if (S->n < RC_MAX_REMOTES) row(_("+ Ajouter un modèle"), NULL, lv_color_hex(0x14532D), presets_cb, NULL);
    row(_("Modifier sur le téléphone"), _("JSON, nouvelles touches, ordre"), C_CARD, portal_cb, NULL);
    lv_obj_t *l = ui_label(v_body, &font_text_16, C_DIM, _("Appui long : supprimer"));
    lv_obj_set_width(l, 228);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_presets(void)
{
    build_list();                                         /* même mise en page */
    lv_obj_clean(v_body);
    for (int i = 0; i < RC_PRESET_N; i++) row(_(rc_preset_name(i)), NULL, C_CARD, add_preset_cb, (void *)(intptr_t)i);
}

/* --- fiche d'une touche */
static void test_cb(lv_event_t *e)
{
    ir_code_t c;
    if (rc_key_code(R(), K(), &c) && send_code(&c, false)) power_vibrate(VIB_SHORT);
}

static void forget_cb(lv_event_t *e)
{
    K()->has = false;
    rc_save(S);
    set_state(ST_PAD);
}

static void scan_begin(int32_t from)
{
    targets_build();
    s_pos = from;
    s_log_n = 0;
    set_state(ST_SCAN);
    start_job(JOB_SCAN);
}

static void search_cb(lv_event_t *e) { scan_begin(0); }

static void likely_cb(lv_event_t *e)
{
    const uint8_t *l; int n;
    if (!likely_list(&l, &n)) return;
    rc_remote_t *r = R();
    rc_key_t *k = K();
    uint8_t proto = (uint8_t)(k->own ? k->proto : r->proto);
    uint16_t addr = k->own ? k->addr : r->addr;
    s_ncand = 0;
    for (int i = 0; i < n && s_ncand < LOG_N; i++) {
        bool used = false;                                 /* déjà pris par une autre touche */
        for (int j = 0; j < r->nkeys; j++) {
            ir_code_t c;
            if (j != s_key && rc_key_code(r, &r->keys[j], &c) && c.proto == proto && c.addr == addr && c.cmd == l[i]) used = true;
        }
        bool dup = false;
        for (int j = 0; j < s_ncand; j++) dup |= s_cand[j].c.cmd == l[i];
        if (!used && !dup) s_cand[s_ncand++] = (sent_t){ { proto, addr, l[i] }, -1, 0 };
    }
    s_half = s_ncand / 2 ? s_ncand / 2 : 1;
    set_state(s_ncand == 1 ? ST_FOUND : ST_TRY);
}

static void build_key(void)
{
    rc_key_t *k = K();
    char b[48];
    ir_code_t c;
    bool known = rc_key_code(R(), k, &c);
    text(k->label, 2, C_FG);
    if (known) { code_str(b, sizeof b, &c); text(b, 26, lv_color_hex(0xFACC15)); }
    else text(_("code inconnu"), 26, C_DIM);
    int y = 56;
    if (known) {
        btn(v_body, _("Tester"), lv_color_hex(0x2563EB), test_cb, NULL, 104, 40); place_x(-56, y);
        btn(v_body, _("Oublier"), lv_color_hex(0xB91C1C), forget_cb, NULL, 104, 40); place_x(56, y);
        y += 48;
    }
    const uint8_t *l; int n;
    if (likely_list(&l, &n)) { btn(v_body, _("Codes probables"), lv_color_hex(0x7C3AED), likely_cb, NULL, 212, 38); place(y); y += 44; }
    btn(v_body, known ? _("Chercher à nouveau") : _("Chercher le code"), lv_color_hex(0x16A34A), search_cb, NULL, 212, 38);
    place(y);
}

/* --- recherche */
static void react_cb(lv_event_t *e)
{
    s_stop = true;
    int64_t t = now_ms();
    s_ncand = 0;
    for (int i = 0; i < s_log_n; i++) if (s_log[i].ms >= t - 2500) s_cand[s_ncand++] = s_log[i];
    if (s_ncand == 0) { set_state(ST_SCAN); start_job(JOB_SCAN); return; }
    s_half = s_ncand == 1 ? 1 : s_ncand / 2;
    set_state(s_ncand == 1 ? ST_FOUND : ST_TRY);
}

static void try_cb(lv_event_t *e) { if (start_job(JOB_LIST)) set_state(ST_ASK); }

static void answer_cb(lv_event_t *e)
{
    bool yes = (bool)(intptr_t)lv_event_get_user_data(e);
    if (yes) s_ncand = s_half;
    else {
        memmove(s_cand, s_cand + s_half, (size_t)(s_ncand - s_half) * sizeof s_cand[0]);
        s_ncand -= s_half;
    }
    if (s_ncand <= 0) { scan_begin(s_pos); return; }       /* réponses contradictoires : on reprend */
    s_half = s_ncand == 1 ? 1 : s_ncand / 2;
    set_state(s_ncand == 1 ? ST_FOUND : ST_TRY);
}

static void verify_cb(lv_event_t *e) { s_half = 1; start_job(JOB_LIST); }

static void save_cb(lv_event_t *e)
{
    rc_remote_t *r = R();
    rc_key_t *k = K();
    const ir_code_t *c = &s_cand[0].c;
    if (!k->own && (r->proto < 0 || !r->has_addr)) {      /* la télécommande apprend son protocole */
        r->proto = (int8_t)c->proto;
        r->addr = c->addr;
        r->has_addr = true;
    }
    if (!k->own && (r->proto != c->proto || r->addr != c->addr)) { k->own = true; }
    k->proto = c->proto;
    k->addr = c->addr;
    k->cmd = c->cmd;
    k->has = true;
    rc_save(S);
    power_vibrate(VIB_SHORT);
    set_state(ST_PAD);
}

static void wrong_cb(lv_event_t *e) { scan_begin(s_cand[0].idx >= 0 ? s_cand[0].idx + 1 : 0); }
static void stop_cb(lv_event_t *e) { s_stop = true; set_state(ST_KEY); }

static void refresh(void)
{
    if (st == ST_SCAN && v_status) {
        ir_code_t c;
        int32_t p = s_pos < s_total ? s_pos : s_total - 1;
        scan_code(p, &c);
        char b[32];
        code_str(b, sizeof b, &c);
        lv_label_set_text(v_status, b);
        lv_bar_set_range(v_bar, 0, s_total);
        lv_bar_set_value(v_bar, s_pos, LV_ANIM_OFF);
        if (!s_run && s_pos >= s_total) set_state(ST_NONE);
    }
}

static void build(void)
{
    if (!v_body) return;
    lv_obj_clean(v_body);
    lv_obj_remove_flag(v_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(v_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_layout(v_body, 0);
    lv_obj_set_style_pad_all(v_body, 0, 0);
    lv_obj_scroll_to_y(v_body, 0, LV_ANIM_OFF);
    v_status = v_bar = NULL;
    char b[64];
    const char *title = st == ST_LIST || st == ST_DELETE ? _("Télécommandes") : st == ST_PRESETS ? _("Ajouter") : R()->name;
    lv_label_set_text(v_title, title);
    bool pad = st == ST_PAD;
    lv_obj_set_style_bg_color(v_btn_list, st == ST_LIST ? C_ACC : C_CARD, 0);
    if (pad) lv_obj_remove_flag(v_btn_edit, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(v_btn_edit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(v_btn_edit, s_edit ? lv_color_hex(0xCA8A04) : C_CARD, 0);
    lv_color_t green = lv_color_hex(0x16A34A), red = lv_color_hex(0xB91C1C), blue = lv_color_hex(0x2563EB);
    switch (st) {
    case ST_PAD: build_pad(); break;
    case ST_LIST: build_list(); break;
    case ST_PRESETS: build_presets(); break;
    case ST_KEY: build_key(); break;
    case ST_DELETE:
        snprintf(b, sizeof b, _("Supprimer « %s » ?"), S->r[s_del].name);
        text(b, 30, C_FG);
        btn(v_body, _("Supprimer"), red, del_yes_cb, NULL, 104, 44); place_x(-56, 100);
        btn(v_body, _("Annuler"), C_OFF, to_list_cb, NULL, 104, 44); place_x(56, 100);
        break;
    case ST_SCAN:
        snprintf(b, sizeof b, _("Recherche : %s"), K()->label);
        text(b, 2, C_FG);
        btn(v_body, _("ÇA A RÉAGI !"), green, react_cb, NULL, 212, 80); place(30);
        v_status = text("", 118, C_DIM);
        v_bar = lv_bar_create(v_body);
        lv_obj_set_size(v_bar, 200, 6);
        lv_obj_align(v_bar, LV_ALIGN_TOP_MID, 0, 144);
        lv_obj_set_style_bg_color(v_bar, C_OFF, LV_PART_MAIN);
        lv_obj_set_style_bg_color(v_bar, green, LV_PART_INDICATOR);
        btn(v_body, _("Arrêter"), C_OFF, stop_cb, NULL, 100, 32); place(158);
        refresh();
        break;
    case ST_TRY:
        snprintf(b, sizeof b, _("Encore %d codes possibles"), s_ncand);
        text(b, 2, C_FG);
        text(_("Remets l'appareil dans son état de départ, vise-le, puis essaie."), 30, C_DIM);
        btn(v_body, _("Essayer"), green, try_cb, NULL, 120, 44); place(104);
        btn(v_body, _("Annuler"), C_OFF, stop_cb, NULL, 100, 32); place(156);
        break;
    case ST_ASK:
        text(_("L'appareil a-t-il réagi ?"), 26, C_FG);
        btn(v_body, _("Oui"), green, answer_cb, (void *)(intptr_t)1, 100, 56); place_x(-56, 66);
        btn(v_body, _("Non"), red, answer_cb, (void *)(intptr_t)0, 100, 56); place_x(56, 66);
        btn(v_body, _("Renvoyer"), C_OFF, try_cb, NULL, 120, 32); place(138);
        break;
    case ST_FOUND: {
        snprintf(b, sizeof b, _("Code trouvé pour %s"), K()->label);
        text(b, 2, C_FG);
        code_str(b, sizeof b, &s_cand[0].c);
        text(b, 26, lv_color_hex(0xFACC15));
        btn(v_body, _("Vérifier"), blue, verify_cb, NULL, 104, 42); place_x(-56, 58);
        btn(v_body, _("Enregistrer"), green, save_cb, NULL, 104, 42); place_x(56, 58);
        btn(v_body, _("Ce n'est pas ça"), C_OFF, wrong_cb, NULL, 180, 34); place(112);
        break;
    }
    case ST_NONE:
        text(_("Aucune réaction. L'appareil utilise peut-être un autre protocole ou une autre adresse : ses codes peuvent être saisis sur le téléphone."), 6, C_DIM);
        btn(v_body, _("Retour"), C_OFF, stop_cb, NULL, 100, 36); place(150);
        break;
    }
}

static void tick_cb(lv_timer_t *t) { refresh(); }

static lv_obj_t *hdr_btn(lv_obj_t *scr, const char *sym, int x, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 40, 34);
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
    v_scr = scr;
    S = rc_alloc();
    if (!S) {
        lv_obj_center(ui_label(scr, &font_text_16, C_FG, _("Mémoire insuffisante")));
        return;
    }
    rc_load(S);
    cur_load();
    int y = ui_header(scr, "");
    lv_obj_t *hdr = lv_obj_get_child(scr, -1);
    v_title = lv_obj_get_child(hdr, 1);
    lv_label_set_long_mode(v_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(v_title, 112, 26);                      /* une ligne, tronquée */
    v_btn_edit = hdr_btn(scr, SYM_PEN, 152, edit_cb);
    v_btn_list = hdr_btn(scr, LV_SYMBOL_LIST, 196, list_cb);
    v_body = ui_container(scr);
    lv_obj_set_size(v_body, 240, 240 - y);
    lv_obj_align(v_body, LV_ALIGN_TOP_MID, 0, y);
    st = ST_PAD;
    s_edit = false;
    build();
    v_tick = lv_timer_create(tick_cb, 150, NULL);
}

static void destroy(void)
{
    s_stop = true;
#ifdef ESP_PLATFORM
    for (int i = 0; i < 50 && s_run; i++) vTaskDelay(pdMS_TO_TICKS(20));
#endif
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    v_body = v_status = v_bar = v_title = v_btn_list = v_btn_edit = NULL;
    free(S);
    S = NULL;
}

static void on_show(void) { if (v_tick) lv_timer_resume(v_tick); }

static void on_hide(void)
{
    s_stop = true;
    if (v_tick) lv_timer_pause(v_tick);
    if (S && st != ST_PAD) { st = ST_PAD; build(); }
}

static bool on_event(int code) { refresh(); return false; }
static bool busy(void) { return s_run && s_job != JOB_ONE; }
static bool keep_on(void) { return st == ST_SCAN || st == ST_TRY || st == ST_ASK || st == ST_FOUND; }

static bool on_back(void)
{
    if (!S) return false;
    if (st == ST_PAD && !s_edit) return false;             /* écran principal : on quitte */
    s_stop = true;
    if (st == ST_PAD) s_edit = false;
    set_state(st == ST_PRESETS || st == ST_DELETE ? ST_LIST : ST_PAD);
    return true;
}

const app_t app_remote = {
    .name = N_("Télécommande"),
    .icon = SYM_KEYPAD,
    .color = 0xCA8A04,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .on_event = on_event,
    .busy = busy,
    .keep_screen_on = keep_on,
    .on_back = on_back,
};

#ifndef ESP_PLATFORM
lv_obj_t *remote_sim_btn(int edit) { return edit ? v_btn_edit : v_btn_list; }   /* simulateur */
#endif
