/*
 * app_irlamp.c — « Lampe IR » : télécommande infrarouge pour les lampes
 * solaires à 8 touches (ON, OFF, 3H, 5H, 8H, 25 %, 50 %, 100 %).
 *
 * La montre n'a pas de récepteur infrarouge : elle ne peut pas lire la
 * télécommande d'origine. Elle cherche donc les codes elle-même, au protocole
 * NEC (celui de presque toutes ces télécommandes) :
 *
 *  1. balayage : elle envoie un à un les 256 codes d'une adresse (≈ 30 s par
 *     adresse, adresses les plus courantes d'abord ; une fois une touche
 *     trouvée, seulement son adresse, en partant des codes voisins) ;
 *  2. on touche « ÇA A RÉAGI ! » dès que la lampe réagit : les codes des
 *     2,5 dernières secondes sont les suspects ;
 *  3. recherche par moitiés : la montre renvoie la moitié des suspects, on
 *     répond si la lampe a réagi… en 4 ou 5 essais il n'en reste qu'un ;
 *  4. on vérifie, on enregistre. Les touches trouvées s'utilisent ensuite
 *     comme sur la télécommande.
 */
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "power.h"
#include "nvs.h"
#include "esp_timer.h"
#include "ir_nec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef ESP_PLATFORM
#define DELAY_MS(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#else
#define DELAY_MS(ms) ((void)0)
#endif

extern const app_t app_irlamp;

#define NKEY 8
#define EV_TICK 1
static const char *const KEY_NAME[NKEY] = { "ON", "OFF", "3H", "5H", "8H", "25 %", "50 %", "100 %" };

/* adresses NEC essayées sans indice : les plus courantes sur ce matériel */
static const uint16_t ADDRS[] = {
    0xFF00, 0x7F80, 0xFE01, 0xFD02, 0xFC03, 0xFB04, 0xF708, 0xEF10, 0xDF20, 0xBF40,
    0xEF00, 0x7F00, 0x807F, 0x01FE, 0x00FF,
};
#define NADDR (int)(sizeof ADDRS / sizeof ADDRS[0])

/* ------------------------------------------------------------------ modèle */
typedef struct {
    uint8_t ver, valid;                 /* valid : un bit par touche */
    uint16_t addr[NKEY];
    uint8_t cmd[NKEY];
} keys_t;
static keys_t s_keys;

typedef struct { uint16_t addr; uint8_t cmd; int32_t idx; int64_t ms; } sent_t;
#define LOG_N 64
static sent_t s_log[LOG_N];
static int s_log_n;                      /* entrées valides, la plus récente en dernier */

typedef enum { ST_KEYS, ST_READY, ST_SCAN, ST_TRY, ST_ASK, ST_FOUND, ST_NONE } state_t;
static state_t st = ST_KEYS;
static int s_key;                        /* touche cherchée */
static bool s_one_addr;                  /* adresse connue : un seul balayage */
static uint16_t s_addr_hint;
static uint8_t s_cmd_hint;
static volatile int32_t s_pos;           /* position du balayage */
static int32_t s_total;
static volatile bool s_run, s_stop;      /* tâche d'envoi */
static sent_t s_cand[LOG_N];
static int s_ncand, s_half;              /* suspects ; la moitié essayée */
static int s_tries;

static void keys_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof s_keys;
    memset(&s_keys, 0, sizeof s_keys);
    if (nvs_open("irlamp", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "keys", &s_keys, &n) != ESP_OK || n != sizeof s_keys || s_keys.ver != 1)
            memset(&s_keys, 0, sizeof s_keys);
        nvs_close(h);
    }
    s_keys.ver = 1;
}

static void keys_save(void)
{
    nvs_handle_t h;
    if (nvs_open("irlamp", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "keys", &s_keys, sizeof s_keys);
    nvs_commit(h);
    nvs_close(h);
}

/* i-ième code du balayage */
static void scan_code(int32_t i, uint16_t *addr, uint8_t *cmd)
{
    if (s_one_addr) {
        *addr = s_addr_hint;
        /* en spirale autour du code connu : h, h+1, h−1, h+2… */
        int k = (int)i, d = (k + 1) / 2;
        *cmd = (uint8_t)(k & 1 ? s_cmd_hint + d : s_cmd_hint - d);
    } else {
        *addr = ADDRS[i / 256];
        *cmd = (uint8_t)(i % 256);
    }
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void log_sent(uint16_t addr, uint8_t cmd, int32_t idx)
{
    if (s_log_n == LOG_N) { memmove(s_log, s_log + 1, (LOG_N - 1) * sizeof s_log[0]); s_log_n--; }
    s_log[s_log_n++] = (sent_t){ addr, cmd, idx, now_ms() };
}

/* ------------------------------------------------------------------ envoi */
typedef enum { JOB_ONE, JOB_SCAN, JOB_LIST } job_t;
static job_t s_job;
static uint16_t s_one_a;
static uint8_t s_one_c;

static void send_task(void *arg)
{
    if (ir_nec_open() == ESP_OK) {
        switch (s_job) {
        case JOB_ONE:
            ir_nec_send(s_one_a, s_one_c);
            DELAY_MS(40);
            ir_nec_send(s_one_a, s_one_c);                 /* deux fois, au cas où */
            break;
        case JOB_SCAN:
#ifndef ESP_PLATFORM
            for (int lim = 0; lim < 20 && !s_stop && s_pos < s_total; lim++) {   /* simulateur : un extrait */
#else
            while (!s_stop && s_pos < s_total) {
#endif
                uint16_t a; uint8_t c;
                scan_code(s_pos, &a, &c);
                ir_nec_send(a, c);
                log_sent(a, c, s_pos);
                s_pos++;
                DELAY_MS(40);
            }
            break;
        case JOB_LIST:
            for (int i = 0; i < s_half && !s_stop; i++) {
                ir_nec_send(s_cand[i].addr, s_cand[i].cmd);
                DELAY_MS(120);
            }
            break;
        }
        ir_nec_close();                                    /* rend le RMT et le verrou de fréquence */
    }
    s_run = false;
    app_post_event(&app_irlamp, EV_TICK);
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
    if (xTaskCreate(send_task, "irlamp", 3072, NULL, 4, NULL) != pdPASS) { s_run = false; return false; }
#else
    send_task(NULL);                                       /* simulateur : synchrone */
#endif
    return true;
}

/* ------------------------------------------------------------------ vue */
static lv_obj_t *v_body, *v_status, *v_bar;
static lv_timer_t *v_tick;
static void build(void);

static void build_async(void *p) { build(); }
/* reconstruit après l'événement en cours : le bouton touché est supprimé */
static void set_state(state_t s) { st = s; lv_async_call(build_async, NULL); }

static void key_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    bool longp = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    if (!longp && (s_keys.valid & (1 << k))) {               /* touche connue : on l'envoie */
        s_one_a = s_keys.addr[k];
        s_one_c = s_keys.cmd[k];
        if (start_job(JOB_ONE)) power_vibrate(VIB_SHORT);
        return;
    }
    s_key = k;
    set_state(ST_READY);
}

static void scan_begin(int32_t from)
{
    s_one_addr = false;
    for (int k = 0; k < NKEY; k++)
        if (s_keys.valid & (1 << k)) { s_one_addr = true; s_addr_hint = s_keys.addr[k]; s_cmd_hint = s_keys.cmd[k]; }
    s_total = s_one_addr ? 256 : NADDR * 256;
    s_pos = from;
    s_log_n = 0;
    set_state(ST_SCAN);
    start_job(JOB_SCAN);
}

static void start_cb(lv_event_t *e) { scan_begin(0); }
static void back_cb(lv_event_t *e) { s_stop = true; set_state(ST_KEYS); }

static void react_cb(lv_event_t *e)
{
    s_stop = true;
    /* suspects : les codes partis dans les 2,5 s avant le toucher */
    int64_t t = now_ms();
    s_ncand = 0;
    for (int i = 0; i < s_log_n; i++)
        if (s_log[i].ms >= t - 2500) s_cand[s_ncand++] = s_log[i];
    s_tries = 0;
    if (s_ncand == 0) { set_state(ST_SCAN); start_job(JOB_SCAN); return; }
    s_half = s_ncand == 1 ? 1 : s_ncand / 2;
    set_state(s_ncand == 1 ? ST_FOUND : ST_TRY);
}

static void try_cb(lv_event_t *e)
{
    if (start_job(JOB_LIST)) { s_tries++; set_state(ST_ASK); }
}

static void answer_cb(lv_event_t *e)
{
    bool yes = (bool)(intptr_t)lv_event_get_user_data(e);
    if (yes) s_ncand = s_half;                            /* c'est dans la moitié essayée */
    else {
        memmove(s_cand, s_cand + s_half, (size_t)(s_ncand - s_half) * sizeof s_cand[0]);
        s_ncand -= s_half;
    }
    if (s_ncand <= 0) { scan_begin(s_pos); return; }        /* réponses contradictoires : on reprend */
    s_half = s_ncand == 1 ? 1 : s_ncand / 2;
    set_state(s_ncand == 1 ? ST_FOUND : ST_TRY);
}

static void verify_cb(lv_event_t *e) { s_half = 1; start_job(JOB_LIST); }

static void save_cb(lv_event_t *e)
{
    s_keys.addr[s_key] = s_cand[0].addr;
    s_keys.cmd[s_key] = s_cand[0].cmd;
    s_keys.valid |= 1 << s_key;
    keys_save();
    power_vibrate(VIB_SHORT);
    set_state(ST_KEYS);
}

static void wrong_cb(lv_event_t *e) { scan_begin(s_cand[0].idx + 1); }

static void forget_cb(lv_event_t *e)
{
    s_keys.valid &= ~(1 << s_key);
    keys_save();
    set_state(ST_KEYS);
}

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

static void refresh(void)
{
    if (st == ST_SCAN && v_status) {
        uint16_t a; uint8_t c;
        int32_t p = s_pos < s_total ? s_pos : s_total - 1;
        scan_code(p, &a, &c);
        lv_label_set_text_fmt(v_status, _("Adresse %04X · code %02X"), a, c);
        lv_bar_set_range(v_bar, 0, s_total);
        lv_bar_set_value(v_bar, s_pos, LV_ANIM_OFF);
        if (!s_run && s_pos >= s_total) set_state(ST_NONE);
    }
}

static void build(void)
{
    if (!v_body) return;
    lv_obj_clean(v_body);
    v_status = v_bar = NULL;
    char b[64];
    lv_color_t green = lv_color_hex(0x16A34A), red = lv_color_hex(0xB91C1C), blue = lv_color_hex(0x2563EB);
    switch (st) {
    case ST_KEYS: {
        /* la disposition de la télécommande */
        static const int8_t POS[NKEY][2] = { { 0, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 }, { 0, 2 }, { 1, 2 }, { 2, 2 } };
        for (int k = 0; k < NKEY; k++) {
            bool ok = s_keys.valid & (1 << k);
            lv_color_t c = !ok ? C_OFF : k == 0 ? green : k == 1 ? red : k <= 4 ? blue : lv_color_hex(0xCA8A04);
            snprintf(b, sizeof b, ok ? "%s" : "%s ?", KEY_NAME[k]);
            lv_obj_t *o = btn(v_body, b, c, NULL, NULL, 68, 46);
            lv_obj_add_event_cb(o, key_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)k);   /* pas après un appui long */
            lv_obj_add_event_cb(o, key_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)k);
            lv_obj_align(o, LV_ALIGN_TOP_LEFT, 6 + POS[k][0] * 74, 2 + POS[k][1] * 52);
        }
        text(s_keys.valid ? _("Grise : trouver son code\nAppui long : rechercher")
                          : _("Touche une touche pour\nchercher son code"), 158, C_DIM);
        break;
    }
    case ST_READY:
        if (s_keys.valid & (1 << s_key)) {                    /* touche connue : son code */
            snprintf(b, sizeof b, "%s", KEY_NAME[s_key]);
            text(b, 4, C_FG);
            snprintf(b, sizeof b, _("adresse %04X · code %02X"), s_keys.addr[s_key], s_keys.cmd[s_key]);
            text(b, 28, lv_color_hex(0xFACC15));
            text(_("Démarrer : chercher à nouveau ce code."), 64, C_DIM);
        } else {
            snprintf(b, sizeof b, _("Recherche : %s"), KEY_NAME[s_key]);
            text(b, 4, C_FG);
            text(_("Lampe allumée pour OFF, éteinte pour ON… Vise-la, démarre, et touche l'écran dès qu'elle réagit."), 30, C_DIM);
        }
        btn(v_body, _("Démarrer"), green, start_cb, NULL, 120, 40);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, s_keys.valid & (1 << s_key) ? -62 : 0, 150);
        if (s_keys.valid & (1 << s_key)) {
            btn(v_body, _("Oublier"), red, forget_cb, NULL, 100, 40);
            lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 62, 150);
        }
        break;
    case ST_SCAN:
        snprintf(b, sizeof b, _("Recherche : %s"), KEY_NAME[s_key]);
        text(b, 4, C_FG);
        btn(v_body, _("ÇA A RÉAGI !"), green, react_cb, NULL, 200, 76);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 0, 34);
        v_status = text("", 122, C_DIM);
        v_bar = lv_bar_create(v_body);
        lv_obj_set_size(v_bar, 200, 6);
        lv_obj_align(v_bar, LV_ALIGN_TOP_MID, 0, 148);
        lv_obj_set_style_bg_color(v_bar, C_OFF, LV_PART_MAIN);
        lv_obj_set_style_bg_color(v_bar, green, LV_PART_INDICATOR);
        btn(v_body, _("Arrêter"), C_OFF, back_cb, NULL, 100, 32);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 0, 162);
        refresh();
        break;
    case ST_TRY:
        snprintf(b, sizeof b, _("Encore %d codes possibles"), s_ncand);
        text(b, 4, C_FG);
        text(_("Remets la lampe dans son état de départ, vise-la, puis essaie."), 34, C_DIM);
        btn(v_body, _("Essayer"), green, try_cb, NULL, 120, 44);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 0, 110);
        btn(v_body, _("Annuler"), C_OFF, back_cb, NULL, 100, 32);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 0, 160);
        break;
    case ST_ASK:
        text(_("La lampe a-t-elle réagi ?"), 30, C_FG);
        btn(v_body, _("Oui"), green, answer_cb, (void *)(intptr_t)1, 96, 56);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, -54, 80);
        btn(v_body, _("Non"), red, answer_cb, (void *)(intptr_t)0, 96, 56);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 54, 80);
        btn(v_body, _("Renvoyer"), C_OFF, try_cb, NULL, 120, 32);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 0, 152);
        break;
    case ST_FOUND:
        snprintf(b, sizeof b, _("Code trouvé pour %s"), KEY_NAME[s_key]);
        text(b, 4, C_FG);
        snprintf(b, sizeof b, _("adresse %04X · code %02X"), s_cand[0].addr, s_cand[0].cmd);
        text(b, 28, C_DIM);
        btn(v_body, _("Vérifier"), blue, verify_cb, NULL, 104, 40);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, -56, 66);
        btn(v_body, _("Enregistrer"), green, save_cb, NULL, 104, 40);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 56, 66);
        btn(v_body, _("Ce n'est pas ça"), C_OFF, wrong_cb, NULL, 180, 34);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 0, 120);
        break;
    case ST_NONE:
        text(_("Aucune réaction sur toutes les adresses essayées. La lampe utilise peut-être un autre protocole : il faudra relever ses codes avec un récepteur."), 10, C_DIM);
        btn(v_body, _("Retour"), C_OFF, back_cb, NULL, 100, 36);
        lv_obj_align(lv_obj_get_child(v_body, -1), LV_ALIGN_TOP_MID, 0, 150);
        break;
    }
}

static void tick_cb(lv_timer_t *t) { refresh(); }

static void create(lv_obj_t *scr)
{
    int y = ui_header(scr, _("Lampe IR"));
    v_body = ui_container(scr);
    lv_obj_set_size(v_body, 240, 240 - y);
    lv_obj_align(v_body, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_remove_flag(v_body, LV_OBJ_FLAG_SCROLLABLE);
    keys_load();
    st = ST_KEYS;
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
    v_body = v_status = v_bar = NULL;
}

static void on_show(void) { if (v_tick) lv_timer_resume(v_tick); }

static void on_hide(void)
{
    s_stop = true;                                         /* écran éteint : balayage arrêté */
    if (v_tick) lv_timer_pause(v_tick);
    if (st != ST_KEYS) { st = ST_KEYS; build(); }
}

static bool on_event(int code) { refresh(); return false; }
static bool busy(void) { return s_run; }
static bool keep_on(void) { return st != ST_KEYS; }

const app_t app_irlamp = {
    .name = N_("Lampe IR"),
    .icon = LV_SYMBOL_CHARGE,
    .color = 0xCA8A04,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .on_event = on_event,
    .busy = busy,
    .keep_screen_on = keep_on,
};
