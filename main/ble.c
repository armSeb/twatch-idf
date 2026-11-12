#include "ble.h"
#include "sdr_mode.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_system.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "ancs.h"
#include "app_events.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_bt.h"
#include "esp_mac.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

void ble_store_config_init(void);   /* fourni par NimBLE (store NVS), non exporté en .h */

#define DEVICE_NAME "T-Watch"

/* 1 = passkey affiché sur la montre (SC + MITM), comme l'ancien firmware.
 * 0 = « Just Works ». Les deux fonctionnent avec le contrôleur d'IDF 5.5.6 ;
 * les échecs 0x208 venaient du contrôleur d'IDF 5.5.3, pas du mode d'appairage. */
#define BLE_USE_PASSKEY 1

/* Log du RSSI toutes les 5 s : diagnostic uniquement (réveille la pile). */
#define BLE_DEBUG_RSSI 0

/* Publicité : intervalles recommandés par l'Accessory Design Guidelines Apple */
#define ADV_FAST_ITVL   256     /* 160 ms comme l'ancien firmware (unités de 0,625 ms) */
#define ADV_SLOW_ITVL   1636    /* 1022,5 ms */
#define ADV_FAST_MS     30000   /* 30 s rapides après déconnexion / boot */
#define ADV_PAIRING_MS  60000

/* Paramètres « éco » (règles Apple : min multiple de 15 ms, max >= min+15,
 * max*(lat+1) <= 2 s, timeout > 3*max*(lat+1), timeout <= 6 s).
 * 300 ms * 5 = 1,5 s ; 3*1,5 = 4,5 s < 6 s. Pas d'aller-retour « actif/éco » :
 * on le demande une seule fois, la latence iOS→montre reste <= 1,5 s. */
#define ECO_ITVL_MIN    120     /* 150 ms (unités 1,25 ms) */
#define ECO_ITVL_MAX    240     /* 300 ms */
#define ECO_LATENCY     4
#define ECO_TIMEOUT     600     /* 6 s (unités 10 ms) */
#define ECO_DELAY_MS    15000   /* après ANCS prêt : laisse iOS finir ses propres mises à jour */
#define ECO_RETRY_MS    5000    /* 1er nouvel essai ; doublé à chaque échec */
#define ECO_RETRY_MAX   120000  /* plafond : on n'abandonne jamais, iOS finit par accepter */

static volatile bool s_running;
static volatile bool s_stopping;   /* nimble_port_stop en cours : pas de publicité */
static volatile ble_state_t s_state = BLE_ST_OFF;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static uint8_t s_addr[6];
static volatile int s_bonds;
static volatile uint16_t s_itvl, s_lat, s_tmo;
static volatile uint8_t s_batt = 100;
static uint16_t s_batt_handle;
static bool s_pairing_in_progress;
static bool s_fast_adv;
static int s_eco_tries;
static bool s_eco_pending;

static struct ble_npl_callout s_co_adv, s_co_eco, s_co_rssi, s_co_enc, s_co_sec, s_co_readv;
static int64_t s_conn_t0_us;       /* instant de connexion (diag délai de chiffrement) */
static bool    s_cut_noenc;        /* coupure « pas chiffré » : pause avant republication */
#define SEC_DELAY_MS   500       /* Security Request seule, un peu après la connexion */
#define READV_DELAY_MS 3000      /* après une coupure « pas chiffré » */
#define ADV_RETRY_MS   1000      /* publicité refusée : nouvel essai */
/* Chiffrement attendu peu après la connexion (< 1 s d'habitude). Constaté :
 * parfois l'iPhone ne répond pas à la Security Request et SMP n'abandonne
 * qu'au bout de 30 s. On coupe plus tôt : l'iPhone se reconnecte aussitôt. */
#define ENC_WATCHDOG_MS 8000
static struct ble_npl_event s_ev_batt, s_ev_forget, s_ev_pair;

static int gap_event(struct ble_gap_event *event, void *arg);
static void adv_start(bool fast, uint32_t fast_ms);

/* Apparence GAP : 0x00C1 = Watch: Sports Watch (numéros assignés Bluetooth) */
#define BLE_APPEARANCE 0x00C1


/* ------------------------------------------------------------------ */
/* Service batterie 0x180F                                              */
/* ------------------------------------------------------------------ */
static int batt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t v = s_batt;
        return os_mbuf_append(ctxt->om, &v, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A19),
                .access_cb = batt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_batt_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ------------------------------------------------------------------ */
static void set_state(ble_state_t st)
{
    if (s_state == st) return;
    s_state = st;
    app_post_simple(EV_BLE_STATE, st);
}

void ble_internal_set_state(ble_state_t st) { set_state(st); }

static void refresh_bonds(void)
{
    int n = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n) == 0) s_bonds = n;
}

static void refresh_conn_params(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) == 0) {
        s_itvl = d.conn_itvl;
        s_lat = d.conn_latency;
        s_tmo = d.supervision_timeout;
        ESP_LOGI(TAG, "params : itvl=%.2f ms lat=%u timeout=%u ms", d.conn_itvl * 1.25f,
                 d.conn_latency, d.supervision_timeout * 10);
    }
}

/* ------------------------------------------------------------------ */
/* Publicité : données brutes (flags + apparence montre + sollicitation */
/* ANCS 128 bits, ce qui fait apparaître la montre dans Réglages >      */
/* Bluetooth sur iOS). Le nom va dans la réponse de scan.              */
/* ------------------------------------------------------------------ */
static void adv_set_data(void)
{
    /* Publicité identique à l'ancien firmware fonctionnel :
     * flags + UUID 16 bits 0x1812, nom en réponse de scan. */
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    static const ble_uuid16_t hid = BLE_UUID16_INIT(0x1812);
    f.uuids16 = &hid;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    f.appearance = BLE_APPEARANCE;
    f.appearance_is_present = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)DEVICE_NAME;
    rsp.name_len = strlen(DEVICE_NAME);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc) ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc);
}

/* ------------------------------------------------------------------ */
/* Journal de diagnostic (anneau en RAM, horodaté à l'heure locale)    */
/* ------------------------------------------------------------------ */
/* En mémoire RTC non initialisée : le journal survit à un plantage ou à un
 * redémarrage logiciel (pas à une coupure d'alimentation), pour voir ce qui
 * a précédé un reboot. */
#define DIAG_MAGIC 0x7D1A6B01u
static RTC_NOINIT_ATTR char s_diag[BLE_DIAG_LINES][48];
static RTC_NOINIT_ATTR int  s_diag_head, s_diag_n;
static RTC_NOINIT_ATTR uint32_t s_diag_magic;

void ble_diag_boot(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    bool keep = s_diag_magic == DIAG_MAGIC && r != ESP_RST_POWERON && r != ESP_RST_BROWNOUT &&
                s_diag_head >= 0 && s_diag_head < BLE_DIAG_LINES && s_diag_n >= 0 && s_diag_n <= BLE_DIAG_LINES;
    if (!keep) {
        memset(s_diag, 0, sizeof(s_diag));
        s_diag_head = s_diag_n = 0;
        s_diag_magic = DIAG_MAGIC;
        return;
    }
    for (int i = 0; i < BLE_DIAG_LINES; i++) s_diag[i][47] = 0;
    static const char *const RS[] = { "?", "alim", "ext", "logiciel", "PLANTAGE", "WDT int", "WDT tâche",
                                      "WDT", "veille", "brownout", "SDIO" };
    ble_diag("— redémarrage (%s) —", (unsigned)r < sizeof(RS) / sizeof(RS[0]) ? RS[r] : "?");
}
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;

void ble_diag(const char *fmt, ...)
{
    char msg[36];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char line[48];
    snprintf(line, sizeof(line), "%02d:%02d:%02d %s", tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
    ESP_LOGI(TAG, "diag: %s", msg);
    portENTER_CRITICAL(&s_diag_mux);
    if (s_diag_magic != DIAG_MAGIC || s_diag_head < 0 || s_diag_head >= BLE_DIAG_LINES ||
        s_diag_n < 0 || s_diag_n > BLE_DIAG_LINES) {        /* mémoire RTC pas encore initialisée */
        memset(s_diag, 0, sizeof(s_diag));
        s_diag_head = s_diag_n = 0;
        s_diag_magic = DIAG_MAGIC;
    }
    memcpy(s_diag[s_diag_head], line, sizeof(line));
    s_diag_head = (s_diag_head + 1) % BLE_DIAG_LINES;
    if (s_diag_n < BLE_DIAG_LINES) s_diag_n++;
    portEXIT_CRITICAL(&s_diag_mux);
}

int ble_diag_dump(char *out, size_t len)
{
    static char copy[BLE_DIAG_LINES][48];
    int n, head;
    portENTER_CRITICAL(&s_diag_mux);
    memcpy(copy, s_diag, sizeof(copy));
    n = s_diag_n; head = s_diag_head;
    if (s_diag_magic != DIAG_MAGIC || head < 0 || head >= BLE_DIAG_LINES || n < 0 || n > BLE_DIAG_LINES) n = head = 0;
    portEXIT_CRITICAL(&s_diag_mux);
    size_t pos = 0;
    out[0] = 0;
    for (int i = 0; i < n && pos + 1 < len; i++) {
        int idx = (head - 1 - i + BLE_DIAG_LINES) % BLE_DIAG_LINES;
        int w = snprintf(out + pos, len - pos, "%s%s", i ? "\n" : "", copy[idx]);
        if (w < 0) break;
        pos += (size_t)w;
    }
    return n;
}

static void adv_start(bool fast, uint32_t fast_ms)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE || s_stopping) return;
    if (ble_gap_adv_active()) ble_gap_adv_stop();

    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = p.itvl_max = fast ? ADV_FAST_ITVL : ADV_SLOW_ITVL;
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
        ble_diag("ERREUR pub. rc=%d, nouvel essai", rc);
        /* Sans nouvel essai, la montre restait invisible jusqu'à un
         * redémarrage du Bluetooth. Cas vu : ENOMEM (6) juste après un échec
         * de connexion, la place de connexion n'étant pas encore libérée. */
        ble_npl_callout_reset(&s_co_readv, ble_npl_time_ms_to_ticks32(ADV_RETRY_MS));
        return;
    }
    s_fast_adv = fast;
    if (fast) ble_npl_callout_reset(&s_co_adv, ble_npl_time_ms_to_ticks32(fast_ms));
    else ble_npl_callout_stop(&s_co_adv);
    set_state(BLE_ST_ADVERTISING);
    ESP_LOGI(TAG, "publicité %s", fast ? "rapide" : "lente");
    ble_diag("pub. %s", fast ? "rapide" : "lente");
}

static void co_adv_cb(struct ble_npl_event *ev)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE && s_fast_adv) adv_start(false, 0);
}

/* ------------------------------------------------------------------ */
/* Paramètres de connexion économes                                     */
/* ------------------------------------------------------------------ */
extern bool ancs_idle(void);

/* attente avant le prochain essai : 5 s, 10 s, 20 s… jusqu'à 2 min */
static uint32_t eco_backoff_ms(void)
{
    uint32_t ms = ECO_RETRY_MS;
    for (int i = 1; i < s_eco_tries && ms < ECO_RETRY_MAX; i++) ms *= 2;
    return ms < ECO_RETRY_MAX ? ms : ECO_RETRY_MAX;
}

static void co_eco_cb(struct ble_npl_event *ev)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_eco_pending) return;
    if (s_itvl >= ECO_ITVL_MIN && s_lat >= ECO_LATENCY) return;      /* déjà fait */
    if (!ancs_idle()) {                                                /* file ANCS occupée */
        ble_npl_callout_reset(&s_co_eco, ble_npl_time_ms_to_ticks32(3000));
        return;
    }
    struct ble_gap_upd_params up = {
        .itvl_min = ECO_ITVL_MIN,
        .itvl_max = ECO_ITVL_MAX,
        .latency = ECO_LATENCY,
        .supervision_timeout = ECO_TIMEOUT,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(s_conn, &up);
    s_eco_tries++;
    if (rc == 0) {
        s_eco_pending = true;
        ESP_LOGI(TAG, "demande params éco (essai %d)", s_eco_tries);
    } else {
        ESP_LOGW(TAG, "update_params rc=%d, nouvel essai", rc);
        ble_npl_callout_reset(&s_co_eco, ble_npl_time_ms_to_ticks32(eco_backoff_ms()));
    }
}

/* Qualité de liaison : RSSI vu par la montre toutes les 5 s (diagnostic) */
static void co_enc_cb(struct ble_npl_event *ev)
{
    struct ble_gap_conn_desc d;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_pairing_in_progress) return;
    if (ble_gap_conn_find(s_conn, &d) == 0 && !d.sec_state.encrypted) {
        ESP_LOGW(TAG, "pas de chiffrement après %d s : déconnexion pour réessai", ENC_WATCHDOG_MS / 1000);
        ble_diag("pas chiffré : coupe");
        s_cut_noenc = true;
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/* Demande de sécurité envoyée seule, 0,5 s après la connexion : constaté,
 * l'iPhone ignorait parfois celle émise dans la même milliseconde que la
 * demande de paramètres de liaison. */
static void co_sec_cb(struct ble_npl_event *ev)
{
    struct ble_gap_conn_desc d;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (ble_gap_conn_find(s_conn, &d) == 0 && d.sec_state.encrypted) return;   /* l'iPhone a devancé */
    int rc = ble_gap_security_initiate(s_conn);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "security_initiate rc=%d", rc);
}

static void co_readv_cb(struct ble_npl_event *ev)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) adv_start(true, ADV_FAST_MS);
}

static void co_rssi_cb(struct ble_npl_event *ev)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    int8_t rssi = 0;
    if (ble_gap_conn_rssi(s_conn, &rssi) == 0) ESP_LOGI(TAG, "RSSI %d dBm", rssi);
    ble_npl_callout_reset(&s_co_rssi, ble_npl_time_ms_to_ticks32(5000));
}

void ble_internal_ancs_ready(uint16_t conn)
{
    set_state(BLE_ST_READY);
    s_eco_tries = 0;
    ble_npl_callout_reset(&s_co_eco, ble_npl_time_ms_to_ticks32(ECO_DELAY_MS));
}

/* ------------------------------------------------------------------ */
/* Événements postés depuis d'autres tâches (exécutés dans l'hôte)      */
/* ------------------------------------------------------------------ */
static void ev_batt_cb(struct ble_npl_event *ev)
{
    if (s_batt_handle) ble_gatts_chr_updated(s_batt_handle);
}

static void ev_forget_cb(struct ble_npl_event *ev)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_store_clear();
    refresh_bonds();
    ESP_LOGW(TAG, "tous les appairages effacés");
}

static void ev_pair_cb(struct ble_npl_event *ev)
{
    adv_start(true, ADV_PAIRING_MS);
}

/* ------------------------------------------------------------------ */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_eco_pending = false;
            ble_npl_callout_stop(&s_co_adv);
            if (ble_gap_conn_find(s_conn, &desc) == 0) {
                const uint8_t *o = desc.peer_ota_addr.val, *i = desc.peer_id_addr.val;
                /* ota ≠ id : RPA de l'iPhone résolue par le contrôleur (bond retrouvé) */
                ESP_LOGI(TAG, "connecté (handle %d) ota=%02x:%02x:%02x:%02x:%02x:%02x/%d "
                         "id=%02x:%02x:%02x:%02x:%02x:%02x/%d bonded=%d", s_conn,
                         o[5], o[4], o[3], o[2], o[1], o[0], desc.peer_ota_addr.type,
                         i[5], i[4], i[3], i[2], i[1], i[0], desc.peer_id_addr.type,
                         desc.sec_state.bonded);
                /* b=… n'est connu qu'après chiffrement : voir la ligne « chiffré » */
                ble_diag("conn %02x%02x%s", o[1], o[0], memcmp(o, i, 6) ? " résolu" : "");
            }
            refresh_conn_params(s_conn);
#if BLE_DEBUG_RSSI
            ble_npl_callout_reset(&s_co_rssi, ble_npl_time_ms_to_ticks32(1000));
#endif
            set_state(BLE_ST_CONNECTED);
            ancs_on_connect(s_conn);
            /* demande de sécurité (chiffrement direct si déjà appairé, sinon
             * appairage passkey) envoyée seule après SEC_DELAY_MS ; la demande
             * de paramètres de liaison attend que le lien soit chiffré */
            s_conn_t0_us = esp_timer_get_time();
            s_cut_noenc = false;
            ble_npl_callout_reset(&s_co_sec, ble_npl_time_ms_to_ticks32(SEC_DELAY_MS));
            ble_npl_callout_reset(&s_co_enc, ble_npl_time_ms_to_ticks32(ENC_WATCHDOG_MS));
        } else {
            ESP_LOGW(TAG, "échec connexion status=%d", event->connect.status);
            ble_diag("échec conn st=%d", event->connect.status);
            /* laisser la pile libérer la connexion avortée avant de republier */
            ble_npl_callout_reset(&s_co_readv, ble_npl_time_ms_to_ticks32(300));
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "déconnecté, raison=0x%x", event->disconnect.reason);
        ble_diag("déco 0x%x", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_itvl = s_lat = s_tmo = 0;
        s_eco_pending = false;
        ble_npl_callout_stop(&s_co_eco);
        ble_npl_callout_stop(&s_co_rssi);
        ble_npl_callout_stop(&s_co_enc);
        ble_npl_callout_stop(&s_co_sec);
        ancs_on_disconnect();
        if (s_pairing_in_progress) {
            s_pairing_in_progress = false;
            app_post_simple(EV_PAIRING_DONE, -1);
        }
        if (s_cut_noenc) {          /* laisser l'iPhone oublier la tentative ratée */
            s_cut_noenc = false;
            ble_npl_callout_reset(&s_co_readv, ble_npl_time_ms_to_ticks32(READV_DELAY_MS));
        } else {
            adv_start(true, ADV_FAST_MS);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "lien chiffré");
            ble_npl_callout_stop(&s_co_enc);
            ble_npl_callout_stop(&s_co_sec);
            int ms = (int)((esp_timer_get_time() - s_conn_t0_us) / 1000);
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_diag("chiffré b=%d en %d.%d s", desc.sec_state.bonded, ms / 1000, (ms % 1000) / 100);
            } else {
                ble_diag("chiffré");
            }
            {
                /* supervision timeout 4 s au lieu des 720 ms d'iOS, pour la
                 * découverte GATT ; les paramètres éco viendront après ANCS */
                struct ble_gap_upd_params up = {
                    .itvl_min = 24, .itvl_max = 36, .latency = 0,
                    .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0,
                };
                int urc = ble_gap_update_params(event->enc_change.conn_handle, &up);
                ESP_LOGI(TAG, "demande timeout 4 s, rc=%d", urc);
            }
            refresh_bonds();
            if (s_pairing_in_progress) {
                s_pairing_in_progress = false;
                app_post_simple(EV_PAIRING_DONE, 0);
            }
            set_state(BLE_ST_SECURED);
            ancs_on_encrypted(event->enc_change.conn_handle);
        } else {
            int st = event->enc_change.status;
            ESP_LOGW(TAG, "échec chiffrement status=%d", st);
            ble_diag("ÉCHEC chiffr. st=%d", st);
            if (st == BLE_HS_ERR_HCI_BASE + BLE_ERR_PINKEY_MISSING &&
                ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                /* l'iPhone a oublié la montre : on efface notre bond et on
                 * relance un appairage propre */
                ble_store_util_delete_peer(&desc.peer_id_addr);
                refresh_bonds();
                ble_gap_security_initiate(event->enc_change.conn_handle);
            } else {
                if (s_pairing_in_progress) {
                    s_pairing_in_progress = false;
                    app_post_simple(EV_PAIRING_DONE, st);
                }
                if (st != BLE_HS_ENOTCONN) {    /* lien déjà perdu : rien à couper */
                    ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
                }
            }
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io pk = { 0 };
            pk.action = BLE_SM_IOACT_DISP;
            pk.passkey = esp_random() % 1000000;
            s_pairing_in_progress = true;
            app_post_simple(EV_PASSKEY, (int32_t)pk.passkey);
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &pk);
            ESP_LOGI(TAG, "passkey affiché, inject rc=%d", rc);
        } else {
            ESP_LOGW(TAG, "action passkey %d non gérée", event->passkey.params.action);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        ble_diag("ré-appairage demandé");
        /* le pair a perdu ses clés : on oublie l'ancien bond et on recommence */
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_RX:
        ancs_on_notify_rx(event->notify_rx.conn_handle, event->notify_rx.attr_handle,
                          event->notify_rx.om);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        bool was_eco = s_itvl >= ECO_ITVL_MIN && s_lat >= ECO_LATENCY;
        s_eco_pending = false;
        refresh_conn_params(event->conn_update.conn_handle);
        bool is_eco = s_itvl >= ECO_ITVL_MIN && s_lat >= ECO_LATENCY;
        if (is_eco && !was_eco) {
            ble_diag("éco %u ms lat %u (essai %d)", (unsigned)(s_itvl * 5 / 4), s_lat, s_eco_tries);
            s_eco_tries = 0;                         /* prochain retour en rapide : repartir de 5 s */
        } else if (!is_eco && s_state == BLE_ST_READY) {
            /* Collision (0x22A : l'iPhone change ses paramètres au même
             * moment) ou iOS qui repasse en rapide : on redemande plus tard. */
            uint32_t ms = event->conn_update.status ? eco_backoff_ms() : ECO_DELAY_MS;
            ESP_LOGW(TAG, "params non éco (status=%d), nouvel essai dans %lu s",
                     event->conn_update.status, (unsigned long)(ms / 1000));
            ble_npl_callout_reset(&s_co_eco, ble_npl_time_ms_to_ticks32(ms));
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        return 0;   /* on accepte les paramètres demandés par l'iPhone */

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_diag("fin pub. r=%d", event->adv_complete.reason);
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) adv_start(s_fast_adv, ADV_FAST_MS);
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) { ble_diag("ERREUR adresse rc=%d", rc); return; }   /* pas d'assert : pas de reboot */
    /* Adresse aléatoire STATIQUE dérivée de la MAC (stable entre les boots) :
     * une identité que l'iPhone n'a jamais vue, sans cache ni ancienne clé
     * liés à l'adresse publique utilisée par les firmwares précédents. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    uint8_t rnd[6];
    for (int i = 0; i < 6; i++) rnd[i] = mac[5 - i] ^ (uint8_t)(0x5A + 17 * i);
    rnd[5] |= 0xC0;                         /* 2 bits de poids fort = 11 : statique */
    rc = ble_hs_id_set_rnd(rnd);
    if (rc == 0) {
        s_own_addr_type = BLE_OWN_ADDR_RANDOM;
    } else {
        ESP_LOGW(TAG, "adresse statique refusée rc=%d, adresse publique", rc);
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
        if (rc) { ble_diag("ERREUR adresse auto rc=%d", rc); return; }
    }
    ble_hs_id_copy_addr(s_own_addr_type == BLE_OWN_ADDR_RANDOM ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC,
                        s_addr, NULL);
    ESP_LOGI(TAG, "adresse %02x:%02x:%02x:%02x:%02x:%02x", s_addr[5], s_addr[4], s_addr[3],
             s_addr[2], s_addr[1], s_addr[0]);
    refresh_bonds();
    adv_set_data();
    adv_start(true, ADV_FAST_MS);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "reset de l'hôte NimBLE, raison=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();              /* retourne après nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_start(void)
{
    if (s_running) return ESP_OK;
    if (sdr_mode_active()) return ESP_ERR_INVALID_STATE;   /* radio prêtée au SDR */
    ble_diag("démarrage BLE (bloc %u Ko)", (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        ble_diag("ERREUR init BLE 0x%x", err);
        return err;
    }

    /* Puissance d'émission max (+9 dBm) : défaut ESP32 = +3 dBm, antenne de
     * la T-Watch peu efficace. Coût négligeable en BLE (émission brève). */
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);
    ESP_LOGI(TAG, "puissance TX : niveau %d (7 = +9 dBm)", esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT));

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;   /* bonds pleins → plus ancien supprimé */
#if BLE_USE_PASSKEY
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;          /* la montre affiche le code */
    ble_hs_cfg.sm_mitm = 1;
#else
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;              /* iOS : simple bouton « Jumeler » */
    ble_hs_cfg.sm_mitm = 0;
#endif
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc) ESP_LOGE(TAG, "GATT server rc=%d", rc);
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_device_appearance_set(BLE_APPEARANCE);   /* « montre » (icône iOS ?) */
    ble_store_config_init();

    struct ble_npl_eventq *q = nimble_port_get_dflt_eventq();
    ble_npl_callout_init(&s_co_adv, q, co_adv_cb, NULL);
    ble_npl_callout_init(&s_co_eco, q, co_eco_cb, NULL);
    ble_npl_callout_init(&s_co_rssi, q, co_rssi_cb, NULL);
    ble_npl_callout_init(&s_co_enc, q, co_enc_cb, NULL);
    ble_npl_callout_init(&s_co_sec, q, co_sec_cb, NULL);
    ble_npl_callout_init(&s_co_readv, q, co_readv_cb, NULL);
    ble_npl_event_init(&s_ev_batt, ev_batt_cb, NULL);
    ble_npl_event_init(&s_ev_forget, ev_forget_cb, NULL);
    ble_npl_event_init(&s_ev_pair, ev_pair_cb, NULL);
    ancs_host_init();

    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_running = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void ble_stop(void)
{
    if (!s_running) return;
    ble_diag("arrêt BLE");
    s_stopping = true;
    int rc = nimble_port_stop();          /* termine les connexions, arrête la tâche hôte */
    if (rc == 0) {
        ble_npl_callout_deinit(&s_co_adv);
        ble_npl_callout_deinit(&s_co_eco);
        ble_npl_callout_deinit(&s_co_rssi);
        ble_npl_callout_deinit(&s_co_enc);
        ble_npl_callout_deinit(&s_co_sec);
        ble_npl_callout_deinit(&s_co_readv);
        ancs_host_deinit();
        nimble_port_deinit();             /* désactive et libère le contrôleur */
    } else {
        /* pile toujours en place : ne pas la croire arrêtée, sinon le prochain
         * nimble_port_init() repartirait sur une pile initialisée */
        ESP_LOGE(TAG, "nimble_port_stop rc=%d", rc);
        ble_diag("ERREUR arrêt BLE rc=%d", rc);
        s_stopping = false;
        return;
    }
    s_running = false;
    s_stopping = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_itvl = s_lat = s_tmo = 0;
    set_state(BLE_ST_OFF);
}

bool ble_running(void) { return s_running; }

void ble_get_info(ble_info_t *o)
{
    o->state = s_state;
    o->itvl_x125 = s_itvl;
    o->latency = s_lat;
    o->timeout_x10 = s_tmo;
    o->bonds = s_bonds;
    o->adv_active = s_running && ble_gap_adv_active();
    memcpy(o->addr, s_addr, 6);
}

void ble_set_battery(uint8_t pct)
{
    if (pct == s_batt) return;
    s_batt = pct;
    if (s_running) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_batt);
}

void ble_forget_all(void)
{
    if (s_running) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_forget);
}

void ble_pairing_mode(void)
{
    if (s_running) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_pair);
}
