/*
 * ancs.c — client ANCS + CTS en NimBLE C pur.
 *
 * Points clés de robustesse :
 *  - un seul contexte d'exécution : la tâche hôte NimBLE (callbacks GATT,
 *    notifications, callouts). Le reste du firmware ne touche pas cet état.
 *  - une seule procédure ATT en vol à la fois (découverte, lecture, écriture
 *    du Control Point) : pas de BLE_HS_EBUSY / EALREADY, pas de requêtes
 *    entrelacées que iOS finit par sanctionner.
 *  - chaque callback porte un numéro de génération : un callback tardif
 *    d'une connexion précédente est ignoré.
 *  - les réponses du Data Source sont réassemblées octet par octet (une
 *    réponse peut être fragmentée sur plusieurs notifications, n'importe où).
 *  - le replay iOS après abonnement est identifié par EventFlagPreExisting ;
 *    la déduplication par contenu reste côté UI (UID iOS changent à chaque
 *    reconnexion).
 */
#include "ancs.h"
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "ble.h"
#include "app_events.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"

static const char *TAG = "ancs";

QueueHandle_t g_notif_queue;

/* ------------------------------------------------------------------ */
/* UUID (octets little-endian)                                          */
/* ------------------------------------------------------------------ */
static const ble_uuid128_t UUID_ANCS = BLE_UUID128_INIT(   /* 7905F431-B5CE-4E99-A40F-4B1E122D00D0 */
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
static const ble_uuid128_t UUID_NS = BLE_UUID128_INIT(     /* 9FBF120D-6301-42D9-8C58-25E699A21DBD */
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
static const ble_uuid128_t UUID_CP = BLE_UUID128_INIT(     /* 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9 */
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
static const ble_uuid128_t UUID_DS = BLE_UUID128_INIT(     /* 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB */
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);
static const ble_uuid16_t UUID_CTS  = BLE_UUID16_INIT(0x1805);
static const ble_uuid16_t UUID_CT   = BLE_UUID16_INIT(0x2A2B);
static const ble_uuid16_t UUID_LTI  = BLE_UUID16_INIT(0x2A0F);
static const ble_uuid16_t UUID_CCCD = BLE_UUID16_INIT(0x2902);

/* Command IDs / attributs ANCS */
#define CMD_GET_NOTIF_ATTR  0
#define CMD_GET_APP_ATTR    1
#define CMD_PERFORM_ACTION  2
#define ATTR_APP_ID   0
#define ATTR_TITLE    1
#define ATTR_MESSAGE  3
#define ATTR_DATE     5
#define ATTR_POS_LBL  6
#define ATTR_NEG_LBL  7
#define N_NOTIF_ATTRS 6

#define CMD_TIMEOUT_MS   5000
#define SETUP_RETRY_MS   3000
#define ENC_SETTLE_MS    400

/* ------------------------------------------------------------------ */
/* État de connexion / découverte                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    SU_NONE = 0, SU_ANCS_SVC, SU_ANCS_CHR, SU_NS_DSC, SU_DS_DSC,
    SU_CTS_SVC, SU_CTS_CHR, SU_CT_DSC, SU_READ_LTI, SU_READ_CT,
    SU_SUB_CT, SU_SUB_DS, SU_SUB_NS, SU_READY, SU_FAILED,
} setup_t;

typedef struct { uint16_t def, val, end; } chr_h_t;

static struct {
    uint16_t conn;
    uint32_t gen;
    setup_t st;
    int retries;
    int auth_waits;
    bool cts_phase;          /* lecture CTS différée, après ANCS prêt */
    uint16_t ancs_s, ancs_e, cts_s, cts_e;
    chr_h_t ns, cp, ds, ct, lti;
    uint16_t ns_cccd, ds_cccd, ct_cccd;
    int32_t tz_min;          /* INT32_MIN = inconnu */
    /* collecte temporaire des caractéristiques pendant la découverte */
    chr_h_t tmp[10];
    const ble_uuid_t *tmp_uuid_kind[10];
    int ntmp;
} c = { .conn = BLE_HS_CONN_HANDLE_NONE };

/* ------------------------------------------------------------------ */
/* File de commandes ANCS                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t uid;
    uint8_t cat;
    uint8_t flags;
    bool modified;
    bool valid;
} cmd_t;

#define QLEN 32
static cmd_t s_q[QLEN];
static int s_qh, s_qn;

typedef enum { PH_NONE = 0, PH_NOTIF, PH_APP, PH_ACTION } phase_t;

static struct {
    phase_t ph;
    cmd_t cmd;
    uint8_t buf[700];
    uint16_t len;
    ancs_notif_t n;
} f;

/* action demandée par l'UI (autre tâche) */
static portMUX_TYPE s_amux = portMUX_INITIALIZER_UNLOCKED;
static bool s_act_pending;
static uint32_t s_act_uid;
static uint8_t s_act_id;

static struct ble_npl_callout s_co_cmd, s_co_setup, s_co_cts;
static struct ble_npl_event s_ev_action;
static bool s_host_inited;
/* une seule écriture CP en vol : la suivante attend la réponse ATT */
static bool s_cp_inflight;

/* cache appId → nom affiché (Get App Attributes) */
#define APPCACHE 12
static struct { char id[ANCS_APPID_MAX + 1]; char name[ANCS_APPNAME_MAX + 1]; } s_app[APPCACHE];
static int s_app_next;

static void setup_step(void);
static void process_next(void);
static void finish_cmd(void);

#define GEN_ARG()   ((void *)(uintptr_t)c.gen)
#define STALE(arg, conn) ((uint32_t)(uintptr_t)(arg) != c.gen || (conn) != c.conn)

/* ------------------------------------------------------------------ */
/* Utilitaires                                                          */
/* ------------------------------------------------------------------ */
static const char *cat_name(uint8_t cat)
{
    static const char *const names[] = {
        "Notification", "Appel", "Appel manqué", "Messagerie", "Social", "Agenda",
        "E-mail", "Actualités", "Santé", "Finance", "Lieu", "Divertissement",
    };
    return cat < sizeof(names) / sizeof(names[0]) ? names[cat] : "Notification";
}

static void fallback_app_name(const char *id, uint8_t cat, char *out, size_t osz)
{
    static const struct { const char *id, *name; } known[] = {
        {"com.apple.MobileSMS", "Messages"}, {"com.apple.mobilephone", "Téléphone"},
        {"com.apple.mobilemail", "Mail"}, {"com.apple.mobilecal", "Calendrier"},
        {"com.apple.reminders", "Rappels"}, {"com.apple.facetime", "FaceTime"},
        {"net.whatsapp.WhatsApp", "WhatsApp"}, {"ph.telegra.Telegraph", "Telegram"},
        {"org.whispersystems.signal", "Signal"}, {"com.facebook.Messenger", "Messenger"},
        {"com.google.Gmail", "Gmail"}, {"com.burbn.instagram", "Instagram"},
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(id, known[i].id) == 0) { strlcpy(out, known[i].name, osz); return; }
    }
    const char *dot = strrchr(id, '.');
    if (dot && dot[1]) {
        strlcpy(out, dot + 1, osz);
        out[0] = toupper((unsigned char)out[0]);
    } else {
        strlcpy(out, cat_name(cat), osz);
    }
}

static const char *app_cache_get(const char *id)
{
    for (int i = 0; i < APPCACHE; i++) {
        if (s_app[i].id[0] && strcmp(s_app[i].id, id) == 0) return s_app[i].name;
    }
    return NULL;
}

static void app_cache_put(const char *id, const char *name)
{
    strlcpy(s_app[s_app_next].id, id, sizeof(s_app[0].id));
    strlcpy(s_app[s_app_next].name, name, sizeof(s_app[0].name));
    s_app_next = (s_app_next + 1) % APPCACHE;
}

static bool is_auth_error(int status)
{
    return status == BLE_HS_ERR_ATT_BASE + BLE_ATT_ERR_INSUFFICIENT_AUTHEN ||
           status == BLE_HS_ERR_ATT_BASE + BLE_ATT_ERR_INSUFFICIENT_ENC;
}

/* ------------------------------------------------------------------ */
/* Découverte / abonnement                                              */
/* ------------------------------------------------------------------ */
static void setup_fail(const char *what, int status)
{
    ESP_LOGW(TAG, "setup '%s' échoué, status=%d", what, status);
    if (c.conn == BLE_HS_CONN_HANDLE_NONE) return;
    /* le CTS est facultatif : une erreur dans sa découverte ne bloque pas ANCS */
    if (!is_auth_error(status) &&
        (c.st == SU_CTS_SVC || c.st == SU_CTS_CHR || c.st == SU_CT_DSC ||
         c.st == SU_READ_LTI || c.st == SU_READ_CT)) {
        c.st = SU_SUB_DS;
        setup_step();
        return;
    }
    if (is_auth_error(status)) {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(c.conn, &d) == 0 && d.sec_state.encrypted) {
            /* Lien déjà chiffré : iOS attend que l'utilisateur accepte
             * « Autoriser les notifications ». On réessaie pendant ~1 min. */
            if (c.auth_waits++ < 20) {
                c.st = SU_NONE;
                ble_npl_callout_reset(&s_co_setup, ble_npl_time_ms_to_ticks32(SETUP_RETRY_MS));
            } else {
                c.st = SU_FAILED;
                ESP_LOGE(TAG, "accès ANCS refusé par l'iPhone");
            }
        } else {
            c.st = SU_NONE;
            ble_gap_security_initiate(c.conn);   /* on_encrypted relancera le setup */
        }
        return;
    }
    if (c.retries++ < 2) {
        c.st = SU_NONE;
        ble_npl_callout_reset(&s_co_setup, ble_npl_time_ms_to_ticks32(SETUP_RETRY_MS));
    } else {
        c.st = SU_FAILED;
        ESP_LOGE(TAG, "ANCS indisponible sur ce lien");
    }
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (STALE(arg, conn)) return BLE_HS_EDONE;
    if (err->status == 0) {
        if (c.st == SU_ANCS_SVC) { c.ancs_s = svc->start_handle; c.ancs_e = svc->end_handle; }
        else if (c.st == SU_CTS_SVC) { c.cts_s = svc->start_handle; c.cts_e = svc->end_handle; }
        return 0;
    }
    if (err->status != BLE_HS_EDONE && err->status != BLE_HS_ERR_ATT_BASE + BLE_ATT_ERR_ATTR_NOT_FOUND) {
        setup_fail("svc", err->status);
        return 0;
    }
    if (c.st == SU_ANCS_SVC) {
        if (!c.ancs_s) {
            ESP_LOGW(TAG, "pas de service ANCS (pas un iPhone, ou pas encore exposé)");
            setup_fail("ancs absent", err->status);
            return 0;
        }
        c.st = SU_ANCS_CHR;
    } else {
        c.st = c.cts_s ? SU_CTS_CHR : SU_SUB_DS;   /* CTS facultatif */
    }
    setup_step();
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (STALE(arg, conn)) return BLE_HS_EDONE;
    if (err->status == 0) {
        if (c.ntmp < 10) {
            c.tmp[c.ntmp].def = chr->def_handle;
            c.tmp[c.ntmp].val = chr->val_handle;
            c.tmp[c.ntmp].end = 0;
            const ble_uuid_t *u = &chr->uuid.u;
            const ble_uuid_t *k = NULL;
            if (c.st == SU_ANCS_CHR) {
                if (!ble_uuid_cmp(u, &UUID_NS.u)) k = &UUID_NS.u;
                else if (!ble_uuid_cmp(u, &UUID_CP.u)) k = &UUID_CP.u;
                else if (!ble_uuid_cmp(u, &UUID_DS.u)) k = &UUID_DS.u;
            } else {
                if (!ble_uuid_cmp(u, &UUID_CT.u)) k = &UUID_CT.u;
                else if (!ble_uuid_cmp(u, &UUID_LTI.u)) k = &UUID_LTI.u;
            }
            c.tmp_uuid_kind[c.ntmp] = k;
            c.ntmp++;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        setup_fail("chr", err->status);
        return 0;
    }
    /* fin de chaque caractéristique = début de la suivante - 1 */
    uint16_t svc_end = (c.st == SU_ANCS_CHR) ? c.ancs_e : c.cts_e;
    for (int i = 0; i < c.ntmp; i++) {
        c.tmp[i].end = (i + 1 < c.ntmp) ? c.tmp[i + 1].def - 1 : svc_end;
        const ble_uuid_t *k = c.tmp_uuid_kind[i];
        if (k == &UUID_NS.u) c.ns = c.tmp[i];
        else if (k == &UUID_CP.u) c.cp = c.tmp[i];
        else if (k == &UUID_DS.u) c.ds = c.tmp[i];
        else if (k == &UUID_CT.u) c.ct = c.tmp[i];
        else if (k == &UUID_LTI.u) c.lti = c.tmp[i];
    }
    c.ntmp = 0;
    if (c.st == SU_ANCS_CHR) {
        if (!c.ns.val || !c.cp.val || !c.ds.val) {
            setup_fail("chr ANCS manquantes", 0);
            return 0;
        }
        c.st = SU_NS_DSC;
    } else {
        c.st = c.ct.val ? SU_CT_DSC : (c.lti.val ? SU_READ_LTI : SU_SUB_DS);
    }
    setup_step();
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (STALE(arg, conn)) return BLE_HS_EDONE;
    if (err->status == 0) {
        if (!ble_uuid_cmp(&dsc->uuid.u, &UUID_CCCD.u)) {
            if (c.st == SU_NS_DSC) c.ns_cccd = dsc->handle;
            else if (c.st == SU_DS_DSC) c.ds_cccd = dsc->handle;
            else if (c.st == SU_CT_DSC) c.ct_cccd = dsc->handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        setup_fail("dsc", err->status);
        return 0;
    }
    switch (c.st) {
    case SU_NS_DSC: c.st = SU_DS_DSC; break;
    case SU_DS_DSC:
        if (!c.ns_cccd || !c.ds_cccd) { setup_fail("cccd ANCS", 0); return 0; }
        c.st = SU_SUB_DS;          /* abonnements ANCS d'abord, CTS après */
        break;
    case SU_CT_DSC: c.st = c.lti.val ? SU_READ_LTI : SU_READ_CT; break;
    default: return 0;
    }
    setup_step();
    return 0;
}

static void handle_ct(const uint8_t *b, int len)
{
    if (len < 7) return;
    struct tm tm = {
        .tm_year = (b[0] | (b[1] << 8)) - 1900,
        .tm_mon = b[2] - 1,
        .tm_mday = b[3],
        .tm_hour = b[4],
        .tm_min = b[5],
        .tm_sec = b[6],
    };
    if (tm.tm_year < 124 || tm.tm_mon < 0 || tm.tm_mon > 11) return;
    time_t local = mktime(&tm);     /* TZ système = UTC → « heure locale vue comme UTC » */
    app_event_t ev = { .type = EV_TIME_SYNC, .a = c.tz_min, .t = local };
    app_post(&ev);
    ESP_LOGI(TAG, "CTS : %04d-%02d-%02d %02d:%02d:%02d locale, tz=%ld min",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
             (long)(c.tz_min == INT32_MIN ? -1 : c.tz_min));
}

static int on_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (STALE(arg, conn)) return 0;
    if (err->status == 0 && attr) {
        uint8_t b[12];
        uint16_t n = 0;
        ble_hs_mbuf_to_flat(attr->om, b, sizeof(b), &n);
        if (c.st == SU_READ_LTI && n >= 2) {
            int8_t tz = (int8_t)b[0];
            uint8_t dst = b[1];
            if (tz != -128) c.tz_min = tz * 15 + (dst != 255 ? dst * 15 : 0);
        } else if (c.st == SU_READ_CT) {
            handle_ct(b, n);
        }
    } else {
        ESP_LOGW(TAG, "lecture CTS status=%d (ignoré)", err->status);
    }
    c.st = (c.st == SU_READ_LTI) ? SU_READ_CT : (c.ct_cccd ? SU_SUB_CT : SU_SUB_DS);
    setup_step();
    return 0;
}

static int on_cccd(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (STALE(arg, conn)) return 0;
    if (err->status != 0) {
        if (c.st == SU_SUB_CT) {          /* CTS non bloquant */
            c.st = SU_SUB_DS;
            setup_step();
        } else {
            setup_fail("abonnement", err->status);
        }
        return 0;
    }
    switch (c.st) {
    case SU_SUB_CT: c.st = SU_SUB_DS; break;
    case SU_SUB_DS: c.st = SU_SUB_NS; break;
    case SU_SUB_NS:
        c.st = SU_READY;
        c.retries = 0;
        ESP_LOGI(TAG, "ANCS prêt");
        ble_internal_ancs_ready(c.conn);
        ble_npl_callout_reset(&s_co_cts, ble_npl_time_ms_to_ticks32(3000));   /* CTS plus tard */
        process_next();
        return 0;
    default: return 0;
    }
    setup_step();
    return 0;
}

static void setup_step(void)
{
    static const uint8_t on[2] = { 0x01, 0x00 };
    int rc = 0;
    switch (c.st) {
    case SU_ANCS_SVC:
        c.ancs_s = c.ancs_e = 0;
        rc = ble_gattc_disc_svc_by_uuid(c.conn, &UUID_ANCS.u, on_svc, GEN_ARG());
        break;
    case SU_ANCS_CHR:
        c.ntmp = 0;
        rc = ble_gattc_disc_all_chrs(c.conn, c.ancs_s, c.ancs_e, on_chr, GEN_ARG());
        break;
    case SU_NS_DSC:
        if (c.ns.end > c.ns.val) rc = ble_gattc_disc_all_dscs(c.conn, c.ns.val, c.ns.end, on_dsc, GEN_ARG());
        else { setup_fail("NS sans descripteur", 0); return; }
        break;
    case SU_DS_DSC:
        if (c.ds.end > c.ds.val) rc = ble_gattc_disc_all_dscs(c.conn, c.ds.val, c.ds.end, on_dsc, GEN_ARG());
        else { setup_fail("DS sans descripteur", 0); return; }
        break;
    case SU_CTS_SVC:
        c.cts_s = c.cts_e = 0;
        rc = ble_gattc_disc_svc_by_uuid(c.conn, &UUID_CTS.u, on_svc, GEN_ARG());
        break;
    case SU_CTS_CHR:
        c.ntmp = 0;
        rc = ble_gattc_disc_all_chrs(c.conn, c.cts_s, c.cts_e, on_chr, GEN_ARG());
        break;
    case SU_CT_DSC:
        if (c.ct.end > c.ct.val) {
            rc = ble_gattc_disc_all_dscs(c.conn, c.ct.val, c.ct.end, on_dsc, GEN_ARG());
        } else {
            c.st = c.lti.val ? SU_READ_LTI : SU_READ_CT;
            setup_step();
            return;
        }
        break;
    case SU_READ_LTI:
        rc = ble_gattc_read(c.conn, c.lti.val, on_read, GEN_ARG());
        break;
    case SU_READ_CT:
        if (!c.ct.val) { c.st = SU_SUB_DS; setup_step(); return; }
        rc = ble_gattc_read(c.conn, c.ct.val, on_read, GEN_ARG());
        break;
    case SU_SUB_CT:
        rc = ble_gattc_write_flat(c.conn, c.ct_cccd, on, 2, on_cccd, GEN_ARG());
        break;
    case SU_SUB_DS:     /* Data Source AVANT Notification Source (recommandation Apple) */
        if (c.cts_phase) {          /* fin de la phase CTS différée : retour à l'état prêt */
            c.cts_phase = false;
            c.st = SU_READY;
            ESP_LOGI(TAG, "CTS terminé");
            process_next();
            return;
        }
        rc = ble_gattc_write_flat(c.conn, c.ds_cccd, on, 2, on_cccd, GEN_ARG());
        break;
    case SU_SUB_NS:
        rc = ble_gattc_write_flat(c.conn, c.ns_cccd, on, 2, on_cccd, GEN_ARG());
        break;
    default:
        return;
    }
    if (rc) setup_fail("gattc", rc);
}

static void co_cts_cb(struct ble_npl_event *ev)
{
    if (c.conn == BLE_HS_CONN_HANDLE_NONE || c.st != SU_READY) return;
    if (f.ph != PH_NONE || s_cp_inflight) {     /* ANCS occupé : on repasse plus tard */
        ble_npl_callout_reset(&s_co_cts, ble_npl_time_ms_to_ticks32(2000));
        return;
    }
    c.cts_phase = true;
    c.st = SU_CTS_SVC;
    setup_step();
}

static void co_setup_cb(struct ble_npl_event *ev)
{
    if (c.conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (c.st == SU_NONE || c.st == SU_FAILED) {
        memset(&c.ns, 0, sizeof(c.ns)); memset(&c.cp, 0, sizeof(c.cp)); memset(&c.ds, 0, sizeof(c.ds));
        memset(&c.ct, 0, sizeof(c.ct)); memset(&c.lti, 0, sizeof(c.lti));
        c.ns_cccd = c.ds_cccd = c.ct_cccd = 0;
        c.st = SU_ANCS_SVC;
        setup_step();
    }
}

/* ------------------------------------------------------------------ */
/* Commandes : Get Notification Attributes / Get App Attributes / Action */
/* ------------------------------------------------------------------ */
static void q_push(const cmd_t *cmd)
{
    if (s_qn >= QLEN) {
        ESP_LOGW(TAG, "file pleine, UID %lu ignoré", (unsigned long)cmd->uid);
        return;
    }
    s_q[(s_qh + s_qn) % QLEN] = *cmd;
    s_q[(s_qh + s_qn) % QLEN].valid = true;
    s_qn++;
}

static bool q_pop(cmd_t *out)
{
    while (s_qn > 0) {
        cmd_t cmd = s_q[s_qh];
        s_qh = (s_qh + 1) % QLEN;
        s_qn--;
        if (cmd.valid) { *out = cmd; return true; }
    }
    return false;
}

static void q_cancel_uid(uint32_t uid)
{
    for (int i = 0; i < s_qn; i++) {
        cmd_t *cmd = &s_q[(s_qh + i) % QLEN];
        if (cmd->valid && cmd->uid == uid) cmd->valid = false;
    }
}

static void q_clear(void) { s_qh = s_qn = 0; }

bool ancs_idle(void) { return c.st == SU_READY && f.ph == PH_NONE && s_qn == 0; }
bool ancs_ready(void) { return c.st == SU_READY; }

static void post_current(void)
{
    if (!f.n.app_name[0]) fallback_app_name(f.n.app_id, f.n.category, f.n.app_name, sizeof(f.n.app_name));
    if (xQueueSend(g_notif_queue, &f.n, 0) == pdTRUE) {
        app_post_simple(EV_NOTIF, 0);
    } else {
        ESP_LOGW(TAG, "file UI pleine, notification perdue");
    }
}

static int on_cp_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (STALE(arg, conn)) return 0;
    s_cp_inflight = false;
    if (err->status != 0) {
        /* 0xA2 = paramètre invalide : la notif a disparu entre-temps (normal) */
        ESP_LOGD(TAG, "CP status=%d (phase %d)", err->status, f.ph);
        if (f.ph == PH_APP) post_current();          /* on publie avec le nom de repli */
        if (is_auth_error(err->status)) ble_gap_security_initiate(c.conn);
        finish_cmd();
        return 0;
    }
    if (f.ph == PH_ACTION) finish_cmd();               /* pas de réponse DS pour une action */
    else if (f.ph == PH_NONE) process_next();          /* DS déjà complet avant la réponse */
    return 0;
}

static void send_notif_attr(const cmd_t *cmd)
{
    /* 6 attributs demandés (= N_NOTIF_ATTRS) ; Title et Message exigent une
     * longueur max sur 2 octets, les autres non. */
    uint8_t q[16];
    int i = 0;
    q[i++] = CMD_GET_NOTIF_ATTR;
    q[i++] = cmd->uid; q[i++] = cmd->uid >> 8; q[i++] = cmd->uid >> 16; q[i++] = cmd->uid >> 24;
    q[i++] = ATTR_APP_ID;
    q[i++] = ATTR_TITLE;   q[i++] = ANCS_TITLE_MAX & 0xFF; q[i++] = ANCS_TITLE_MAX >> 8;
    q[i++] = ATTR_MESSAGE; q[i++] = ANCS_MSG_MAX & 0xFF;   q[i++] = ANCS_MSG_MAX >> 8;
    q[i++] = ATTR_DATE;
    q[i++] = ATTR_POS_LBL;
    q[i++] = ATTR_NEG_LBL;

    f.ph = PH_NOTIF;
    f.cmd = *cmd;
    f.len = 0;
    memset(&f.n, 0, sizeof(f.n));
    f.n.uid = cmd->uid;
    f.n.category = cmd->cat;
    f.n.flags = cmd->flags;
    f.n.modified = cmd->modified;
    int rc = ble_gattc_write_flat(c.conn, c.cp.val, q, i, on_cp_write, GEN_ARG());
    if (rc) {
        ESP_LOGW(TAG, "write CP rc=%d", rc);
        f.ph = PH_NONE;
        return;
    }
    s_cp_inflight = true;
    ble_npl_callout_reset(&s_co_cmd, ble_npl_time_ms_to_ticks32(CMD_TIMEOUT_MS));
}

static void send_app_attr(void)
{
    uint8_t p[ANCS_APPID_MAX + 4];
    size_t n = strlen(f.n.app_id);
    p[0] = CMD_GET_APP_ATTR;
    memcpy(&p[1], f.n.app_id, n);
    p[1 + n] = 0;          /* AppIdentifier terminé par NUL */
    p[2 + n] = 0;          /* AppAttributeIDDisplayName */
    f.ph = PH_APP;
    f.len = 0;
    if (s_cp_inflight) {
        /* la réponse à l'écriture précédente n'est pas encore là : on publie
         * avec le nom de repli plutôt que d'entrelacer deux requêtes ATT */
        post_current();
        finish_cmd();
        return;
    }
    int rc = ble_gattc_write_flat(c.conn, c.cp.val, p, n + 3, on_cp_write, GEN_ARG());
    if (rc) {
        post_current();
        finish_cmd();
        return;
    }
    s_cp_inflight = true;
    ble_npl_callout_reset(&s_co_cmd, ble_npl_time_ms_to_ticks32(CMD_TIMEOUT_MS));
}

static bool send_action(void)
{
    bool pending;
    uint32_t uid;
    uint8_t act;
    portENTER_CRITICAL(&s_amux);
    pending = s_act_pending;
    uid = s_act_uid;
    act = s_act_id;
    s_act_pending = false;
    portEXIT_CRITICAL(&s_amux);
    if (!pending) return false;
    uint8_t p[6] = { CMD_PERFORM_ACTION, uid, uid >> 8, uid >> 16, uid >> 24, act };
    f.ph = PH_ACTION;
    int rc = ble_gattc_write_flat(c.conn, c.cp.val, p, sizeof(p), on_cp_write, GEN_ARG());
    if (rc) {
        f.ph = PH_NONE;
        return false;
    }
    s_cp_inflight = true;
    ESP_LOGI(TAG, "action %u sur UID %lu", act, (unsigned long)uid);
    ble_npl_callout_reset(&s_co_cmd, ble_npl_time_ms_to_ticks32(CMD_TIMEOUT_MS));
    return true;
}

static void process_next(void)
{
    if (c.st != SU_READY || f.ph != PH_NONE || s_cp_inflight) return;
    if (send_action()) return;            /* l'action utilisateur est prioritaire */
    cmd_t cmd;
    while (f.ph == PH_NONE && q_pop(&cmd)) {
        send_notif_attr(&cmd);
    }
}

static void finish_cmd(void)
{
    ble_npl_callout_stop(&s_co_cmd);
    f.ph = PH_NONE;
    f.len = 0;
    process_next();
}

static void co_cmd_cb(struct ble_npl_event *ev)
{
    ESP_LOGW(TAG, "timeout commande (phase %d, UID %lu)", f.ph, (unsigned long)f.cmd.uid);
    if (f.ph == PH_APP) post_current();
    finish_cmd();
}

static void ev_action_cb(struct ble_npl_event *ev) { process_next(); }

/* ------------------------------------------------------------------ */
/* Réassemblage Data Source                                             */
/* retour : 1 complet, 0 incomplet, -1 erreur                           */
/* ------------------------------------------------------------------ */
static void on_data_source(struct os_mbuf *om)
{
    if (f.ph != PH_NOTIF && f.ph != PH_APP) {
        ESP_LOGD(TAG, "DS hors requête, ignoré");
        return;
    }
    uint16_t n = OS_MBUF_PKTLEN(om);
    if (f.len + n > sizeof(f.buf)) {
        ESP_LOGW(TAG, "réponse DS trop longue, abandon");
        finish_cmd();
        return;
    }
    os_mbuf_copydata(om, 0, n, &f.buf[f.len]);
    f.len += n;

    int r;
    if (f.ph == PH_NOTIF) {
        r = ancs_parse_notif_attrs(f.buf, f.len, f.cmd.uid, N_NOTIF_ATTRS, &f.n);
    } else {
        r = ancs_parse_app_attrs(f.buf, f.len, f.n.app_name, sizeof(f.n.app_name));
        if (r == 1 && f.n.app_name[0]) app_cache_put(f.n.app_id, f.n.app_name);
    }
    if (r < 0) {
        ESP_LOGW(TAG, "réponse DS incohérente, abandon");
        finish_cmd();
        return;
    }
    if (r == 0) return;   /* attendre la suite */

    if (f.ph == PH_NOTIF) {
        const char *name = f.n.app_id[0] ? app_cache_get(f.n.app_id) : NULL;
        if (name || !f.n.app_id[0]) {
            if (name) strlcpy(f.n.app_name, name, sizeof(f.n.app_name));
            post_current();
            finish_cmd();
        } else {
            ble_npl_callout_stop(&s_co_cmd);
            send_app_attr();
        }
    } else {
        post_current();
        finish_cmd();
    }
}

static void on_notification_source(struct os_mbuf *om)
{
    uint8_t b[8];
    if (OS_MBUF_PKTLEN(om) < 8) return;
    os_mbuf_copydata(om, 0, 8, b);
    uint8_t evt = b[0], flags = b[1], cat = b[2];
    uint32_t uid = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);

    switch (evt) {
    case 0:   /* Added */
    case 1: { /* Modified */
        cmd_t cmd = { .uid = uid, .cat = cat, .flags = flags, .modified = (evt == 1) };
        if (evt == 1) q_cancel_uid(uid);
        q_push(&cmd);
        process_next();
        break;
    }
    case 2:   /* Removed */
        q_cancel_uid(uid);
        app_post_simple(EV_NOTIF_REMOVED, (int32_t)uid);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* API appelée par ble.c                                                */
/* ------------------------------------------------------------------ */
void ancs_module_init(void)
{
    g_notif_queue = xQueueCreate(6, sizeof(ancs_notif_t));
    assert(g_notif_queue);
}

void ancs_host_init(void)
{
    struct ble_npl_eventq *q = nimble_port_get_dflt_eventq();
    ble_npl_callout_init(&s_co_cmd, q, co_cmd_cb, NULL);
    ble_npl_callout_init(&s_co_setup, q, co_setup_cb, NULL);
    ble_npl_callout_init(&s_co_cts, q, co_cts_cb, NULL);
    ble_npl_event_init(&s_ev_action, ev_action_cb, NULL);
    s_host_inited = true;
}

void ancs_host_deinit(void)
{
    s_host_inited = false;
    ble_npl_callout_deinit(&s_co_cmd);
    ble_npl_callout_deinit(&s_co_setup);
    ble_npl_callout_deinit(&s_co_cts);
    ancs_on_disconnect();
}

void ancs_on_connect(uint16_t conn)
{
    c.conn = conn;
    c.gen++;
    c.st = SU_NONE;
    c.retries = 0;
    c.auth_waits = 0;
    c.cts_phase = false;
    c.tz_min = INT32_MIN;
    /* pas d'échange MTU à l'initiative de la montre : iOS le fait lui-même */
}

void ancs_on_encrypted(uint16_t conn)
{
    if (conn != c.conn) return;
    if (c.st != SU_NONE && c.st != SU_FAILED) return;   /* déjà en cours ou prêt */
    c.st = SU_NONE;
    /* petit délai : iOS expose ANCS juste après le chiffrement */
    ble_npl_callout_reset(&s_co_setup, ble_npl_time_ms_to_ticks32(ENC_SETTLE_MS));
}

void ancs_on_disconnect(void)
{
    c.conn = BLE_HS_CONN_HANDLE_NONE;
    c.gen++;
    c.st = SU_NONE;
    if (s_host_inited) {
        ble_npl_callout_stop(&s_co_cmd);
        ble_npl_callout_stop(&s_co_setup);
        ble_npl_callout_stop(&s_co_cts);
    }
    f.ph = PH_NONE;
    f.len = 0;
    s_cp_inflight = false;
    q_clear();
    portENTER_CRITICAL(&s_amux);
    s_act_pending = false;
    portEXIT_CRITICAL(&s_amux);
}

void ancs_on_notify_rx(uint16_t conn, uint16_t attr, struct os_mbuf *om)
{
    if (conn != c.conn) return;
    if (attr == c.ds.val && c.ds.val) on_data_source(om);
    else if (attr == c.ns.val && c.ns.val) on_notification_source(om);
    else if (attr == c.ct.val && c.ct.val) {
        uint8_t b[10];
        uint16_t n = OS_MBUF_PKTLEN(om) > 10 ? 10 : OS_MBUF_PKTLEN(om);
        os_mbuf_copydata(om, 0, n, b);
        handle_ct(b, n);
    }
}

void ancs_request_action(uint32_t uid, uint8_t action)
{
    if (!ble_running()) return;
    portENTER_CRITICAL(&s_amux);
    s_act_pending = true;
    s_act_uid = uid;
    s_act_id = action;
    portEXIT_CRITICAL(&s_amux);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_action);
}
