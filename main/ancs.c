/*
 * ancs.c — client ANCS + CTS + AMS (lecture en cours) en NimBLE C pur.
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
#include "media.h"
#include <stdlib.h>
#include "esp_timer.h"
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
static const ble_uuid128_t UUID_AMS = BLE_UUID128_INIT(    /* 89D3502B-0F36-433A-8EF4-C502AD55F8DC */
    0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E, 0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89);
static const ble_uuid128_t UUID_AMS_RC = BLE_UUID128_INIT( /* 9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2 */
    0xC2, 0x51, 0xCA, 0xF7, 0x56, 0x0E, 0xDF, 0xB8, 0x8A, 0x4A, 0xB1, 0x57, 0xD8, 0x81, 0x3C, 0x9B);
static const ble_uuid128_t UUID_AMS_EU = BLE_UUID128_INIT( /* 2F7CABCE-808D-411F-9A0C-BB92BA96C102 */
    0x02, 0xC1, 0x96, 0xBA, 0x92, 0xBB, 0x0C, 0x9A, 0x1F, 0x41, 0x8D, 0x80, 0xCE, 0xAB, 0x7C, 0x2F);
static const ble_uuid128_t UUID_AMS_EA = BLE_UUID128_INIT( /* C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7 */
    0xD7, 0xD5, 0xBB, 0x70, 0xA8, 0xA3, 0xAB, 0xA6, 0xD8, 0x46, 0xAB, 0x23, 0x8C, 0xF3, 0xB2, 0xC6);
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
    SU_AMS_SVC, SU_AMS_CHR, SU_AMS_RC_DSC, SU_AMS_EU_DSC, SU_AMS_SUB_RC, SU_AMS_SUB_EU,
    SU_AMS_EU_PLAYER, SU_AMS_EU_TRACK,
} setup_t;

typedef struct { uint16_t def, val, end; } chr_h_t;

static struct {
    uint16_t conn;
    uint32_t gen;
    setup_t st;
    int retries;
    int auth_waits;
    bool cts_phase;          /* lecture CTS différée, après ANCS prêt */
    bool ams_phase;          /* découverte AMS, enchaînée après le CTS */
    uint16_t ancs_s, ancs_e, cts_s, cts_e, ams_s, ams_e;
    chr_h_t ns, cp, ds, ct, lti, rc, eu, ea;
    uint16_t ns_cccd, ds_cccd, ct_cccd, rc_cccd, eu_cccd;
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

typedef enum { PH_NONE = 0, PH_NOTIF, PH_APP, PH_ACTION, PH_EA } phase_t;

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
static void ams_done(bool ok);
static bool send_media_cmd(void);
static bool send_ea_fetch(void);

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
    if (c.ams_phase) {             /* AMS facultatif : ANCS reste opérationnel */
        ams_done(false);
        return;
    }
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
                c.st = SU_FAILED; ble_diag("ANCS ÉCHEC");
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
        c.st = SU_FAILED; ble_diag("ANCS ÉCHEC");
        ESP_LOGE(TAG, "ANCS indisponible sur ce lien");
    }
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (STALE(arg, conn)) return BLE_HS_EDONE;
    if (err->status == 0) {
        if (c.st == SU_ANCS_SVC) { c.ancs_s = svc->start_handle; c.ancs_e = svc->end_handle; }
        else if (c.st == SU_CTS_SVC) { c.cts_s = svc->start_handle; c.cts_e = svc->end_handle; }
        else if (c.st == SU_AMS_SVC) { c.ams_s = svc->start_handle; c.ams_e = svc->end_handle; }
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
    } else if (c.st == SU_AMS_SVC) {
        if (!c.ams_s) { ams_done(false); return 0; }
        c.st = SU_AMS_CHR;
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
            } else if (c.st == SU_AMS_CHR) {
                if (!ble_uuid_cmp(u, &UUID_AMS_RC.u)) k = &UUID_AMS_RC.u;
                else if (!ble_uuid_cmp(u, &UUID_AMS_EU.u)) k = &UUID_AMS_EU.u;
                else if (!ble_uuid_cmp(u, &UUID_AMS_EA.u)) k = &UUID_AMS_EA.u;
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
    uint16_t svc_end = (c.st == SU_ANCS_CHR) ? c.ancs_e : (c.st == SU_AMS_CHR) ? c.ams_e : c.cts_e;
    for (int i = 0; i < c.ntmp; i++) {
        c.tmp[i].end = (i + 1 < c.ntmp) ? c.tmp[i + 1].def - 1 : svc_end;
        const ble_uuid_t *k = c.tmp_uuid_kind[i];
        if (k == &UUID_NS.u) c.ns = c.tmp[i];
        else if (k == &UUID_CP.u) c.cp = c.tmp[i];
        else if (k == &UUID_DS.u) c.ds = c.tmp[i];
        else if (k == &UUID_CT.u) c.ct = c.tmp[i];
        else if (k == &UUID_LTI.u) c.lti = c.tmp[i];
        else if (k == &UUID_AMS_RC.u) c.rc = c.tmp[i];
        else if (k == &UUID_AMS_EU.u) c.eu = c.tmp[i];
        else if (k == &UUID_AMS_EA.u) c.ea = c.tmp[i];
    }
    c.ntmp = 0;
    if (c.st == SU_ANCS_CHR) {
        if (!c.ns.val || !c.cp.val || !c.ds.val) {
            setup_fail("chr ANCS manquantes", 0);
            return 0;
        }
        c.st = SU_NS_DSC;
    } else if (c.st == SU_AMS_CHR) {
        if (!c.rc.val || !c.eu.val) { ams_done(false); return 0; }
        c.st = SU_AMS_RC_DSC;
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
            else if (c.st == SU_AMS_RC_DSC) c.rc_cccd = dsc->handle;
            else if (c.st == SU_AMS_EU_DSC) c.eu_cccd = dsc->handle;
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
    case SU_AMS_RC_DSC: c.st = SU_AMS_EU_DSC; break;
    case SU_AMS_EU_DSC:
        if (!c.eu_cccd) { ams_done(false); return 0; }
        c.st = SU_AMS_SUB_RC;
        break;
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
        ble_diag("ANCS prêt");
        ble_internal_ancs_ready(c.conn);
        ble_npl_callout_reset(&s_co_cts, ble_npl_time_ms_to_ticks32(3000));   /* CTS plus tard */
        process_next();
        return 0;
    case SU_AMS_SUB_RC: c.st = SU_AMS_SUB_EU; break;
    case SU_AMS_SUB_EU: c.st = SU_AMS_EU_PLAYER; break;
    default: return 0;
    }
    setup_step();
    return 0;
}

static int on_ams_eu_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (STALE(arg, conn)) return 0;
    if (err->status != 0) { setup_fail("AMS entity update", err->status); return 0; }
    if (c.st == SU_AMS_EU_PLAYER) { c.st = SU_AMS_EU_TRACK; setup_step(); }
    else if (c.st == SU_AMS_EU_TRACK) ams_done(true);
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
        if (c.cts_phase) {          /* fin de la phase CTS différée : on enchaîne sur AMS */
            c.cts_phase = false;
            ESP_LOGI(TAG, "CTS terminé");
            c.ams_phase = true;
            c.st = SU_AMS_SVC;
            setup_step();
            return;
        }
        rc = ble_gattc_write_flat(c.conn, c.ds_cccd, on, 2, on_cccd, GEN_ARG());
        break;
    case SU_SUB_NS:
        rc = ble_gattc_write_flat(c.conn, c.ns_cccd, on, 2, on_cccd, GEN_ARG());
        break;
    case SU_AMS_SVC:
        c.ams_s = c.ams_e = 0;
        rc = ble_gattc_disc_svc_by_uuid(c.conn, &UUID_AMS.u, on_svc, GEN_ARG());
        break;
    case SU_AMS_CHR:
        c.ntmp = 0;
        rc = ble_gattc_disc_all_chrs(c.conn, c.ams_s, c.ams_e, on_chr, GEN_ARG());
        break;
    case SU_AMS_RC_DSC:
        if (c.rc.end > c.rc.val) rc = ble_gattc_disc_all_dscs(c.conn, c.rc.val, c.rc.end, on_dsc, GEN_ARG());
        else { c.st = SU_AMS_EU_DSC; setup_step(); return; }
        break;
    case SU_AMS_EU_DSC:
        if (c.eu.end > c.eu.val) rc = ble_gattc_disc_all_dscs(c.conn, c.eu.val, c.eu.end, on_dsc, GEN_ARG());
        else { ams_done(false); return; }
        break;
    case SU_AMS_SUB_RC:     /* liste des commandes disponibles (facultatif) */
        if (!c.rc_cccd) { c.st = SU_AMS_SUB_EU; setup_step(); return; }
        rc = ble_gattc_write_flat(c.conn, c.rc_cccd, on, 2, on_cccd, GEN_ARG());
        break;
    case SU_AMS_SUB_EU:
        rc = ble_gattc_write_flat(c.conn, c.eu_cccd, on, 2, on_cccd, GEN_ARG());
        break;
    case SU_AMS_EU_PLAYER: {  /* Player : Name, PlaybackInfo, Volume */
        static const uint8_t q[] = { 0, 0, 1, 2 };
        rc = ble_gattc_write_flat(c.conn, c.eu.val, q, sizeof(q), on_ams_eu_write, GEN_ARG());
        break;
    }
    case SU_AMS_EU_TRACK: {   /* Track : Artist, Album, Title, Duration */
        static const uint8_t q[] = { 2, 0, 1, 2, 3 };
        rc = ble_gattc_write_flat(c.conn, c.eu.val, q, sizeof(q), on_ams_eu_write, GEN_ARG());
        break;
    }
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
        memset(&c.rc, 0, sizeof(c.rc)); memset(&c.eu, 0, sizeof(c.eu)); memset(&c.ea, 0, sizeof(c.ea));
        c.ns_cccd = c.ds_cccd = c.ct_cccd = c.rc_cccd = c.eu_cccd = 0;
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
    if (send_media_cmd()) return;         /* puis les commandes de lecture */
    cmd_t cmd;
    while (f.ph == PH_NONE && q_pop(&cmd)) {
        send_notif_attr(&cmd);
    }
    if (f.ph == PH_NONE && !s_cp_inflight) send_ea_fetch();   /* textes AMS tronqués */
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
/* AMS : lecture en cours                                               */
/* Entity Update notifié : [EntityID][AttributeID][Flags][valeur UTF-8]  */
/* Flags bit 0 = valeur tronquée à la taille du MTU : le texte complet   */
/* se relit via Entity Attribute (écriture de la clé, puis lecture).     */
/* ------------------------------------------------------------------ */
#define AMS_ENT_PLAYER 0
#define AMS_ENT_TRACK  2

static media_info_t s_media = { .volume = -1 };
static portMUX_TYPE s_mmux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_media_ev_posted;
#define MCMD_Q 4
static uint8_t s_mcmd[MCMD_Q];
static int s_mcmd_n;
#define EA_Q 4
static uint16_t s_ea_q[EA_Q];     /* (entité << 8) | attribut */
static int s_ea_n;
static uint16_t s_ea_cur;
static uint8_t s_ea_buf[MEDIA_TXT_MAX];
static uint16_t s_ea_len;

static void media_notify_app(void)
{
    if (!s_media_ev_posted) {
        s_media_ev_posted = true;
        app_post_simple(EV_MEDIA, 0);
    }
}

static void media_reset(bool available)
{
    portENTER_CRITICAL(&s_mmux);
    uint32_t seq = s_media.seq;
    memset(&s_media, 0, sizeof(s_media));
    s_media.volume = -1;
    s_media.available = available;
    s_media.seq = seq + 1;
    portEXIT_CRITICAL(&s_mmux);
    media_notify_app();
}

static void ea_push(uint8_t ent, uint8_t attr)
{
    uint16_t key = (uint16_t)(ent << 8 | attr);
    for (int i = 0; i < s_ea_n; i++) if (s_ea_q[i] == key) return;
    if (s_ea_n < EA_Q) s_ea_q[s_ea_n++] = key;
}

static void store_attr(uint8_t ent, uint8_t attr, const uint8_t *v, int n)
{
    char t[MEDIA_TXT_MAX];
    if (n > (int)sizeof(t) - 1) n = sizeof(t) - 1;
    if (n < 0) n = 0;
    memcpy(t, v, n);
    t[n] = 0;

    if (ent == AMS_ENT_PLAYER && attr == 1) {          /* PlaybackInfo : "état,vitesse,écoulé" */
        char *p = t, *e;
        long st = strtol(p, &e, 10);
        float rate = 0, el = 0;
        if (*e == ',') { rate = strtof(e + 1, &e); }
        if (*e == ',') { el = strtof(e + 1, &e); }
        portENTER_CRITICAL(&s_mmux);
        s_media.state = (int)st;
        s_media.rate = rate;
        s_media.elapsed_s = el;
        s_media.elapsed_at_us = esp_timer_get_time();
        s_media.seq++;
        portEXIT_CRITICAL(&s_mmux);
    } else if (ent == AMS_ENT_PLAYER && attr == 2) {   /* Volume "0.0".."1.0" */
        float vol = n ? strtof(t, NULL) : -1;
        portENTER_CRITICAL(&s_mmux);
        s_media.volume = vol;
        s_media.seq++;
        portEXIT_CRITICAL(&s_mmux);
    } else if (ent == AMS_ENT_TRACK && attr == 3) {    /* Duration en secondes */
        float d = n ? strtof(t, NULL) : 0;
        portENTER_CRITICAL(&s_mmux);
        s_media.duration_s = d;
        s_media.seq++;
        portEXIT_CRITICAL(&s_mmux);
    } else {
        char *dst = NULL;
        size_t dsz = 0;
        portENTER_CRITICAL(&s_mmux);
        if (ent == AMS_ENT_PLAYER && attr == 0) { dst = s_media.player; dsz = sizeof(s_media.player); }
        else if (ent == AMS_ENT_TRACK && attr == 0) { dst = s_media.artist; dsz = sizeof(s_media.artist); }
        else if (ent == AMS_ENT_TRACK && attr == 1) { dst = s_media.album; dsz = sizeof(s_media.album); }
        else if (ent == AMS_ENT_TRACK && attr == 2) { dst = s_media.title; dsz = sizeof(s_media.title); }
        if (dst) { strlcpy(dst, t, dsz); s_media.seq++; }
        portEXIT_CRITICAL(&s_mmux);
        if (!dst) return;
    }
    media_notify_app();
}

static void on_entity_update(struct os_mbuf *om)
{
    uint8_t b[3 + MEDIA_TXT_MAX];
    uint16_t n = OS_MBUF_PKTLEN(om);
    if (n < 3) return;
    if (n > sizeof(b)) n = sizeof(b);
    os_mbuf_copydata(om, 0, n, b);
    store_attr(b[0], b[1], &b[3], n - 3);
    if ((b[2] & 0x01) && b[0] == AMS_ENT_TRACK && b[1] <= 2 && c.ea.val) {
        ea_push(b[0], b[1]);           /* titre/artiste/album tronqué : relire en entier */
        process_next();
    }
}

static void on_remote_cmds(struct os_mbuf *om)
{
    uint8_t b[16];
    uint16_t n = OS_MBUF_PKTLEN(om);
    if (n > sizeof(b)) n = sizeof(b);
    os_mbuf_copydata(om, 0, n, b);
    uint16_t mask = 0;
    for (int i = 0; i < n; i++) if (b[i] < 16) mask |= 1u << b[i];
    portENTER_CRITICAL(&s_mmux);
    s_media.cmds = mask;
    s_media.seq++;
    portEXIT_CRITICAL(&s_mmux);
    media_notify_app();
}

static void ams_done(bool ok)
{
    c.ams_phase = false;
    c.st = SU_READY;
    if (ok) {
        portENTER_CRITICAL(&s_mmux);
        s_media.available = true;
        s_media.seq++;
        portEXIT_CRITICAL(&s_mmux);
        media_notify_app();
        ESP_LOGI(TAG, "AMS prêt");
        ble_diag("AMS prêt");
    } else {
        ESP_LOGW(TAG, "AMS indisponible (ANCS non affecté)");
        ble_diag("AMS absent");
    }
    process_next();
}

static bool send_media_cmd(void)
{
    if (!c.rc.val) return false;
    uint8_t cmd;
    portENTER_CRITICAL(&s_amux);
    bool have = s_mcmd_n > 0;
    if (have) {
        cmd = s_mcmd[0];
        memmove(s_mcmd, s_mcmd + 1, --s_mcmd_n);
    }
    portEXIT_CRITICAL(&s_amux);
    if (!have) return false;
    f.ph = PH_ACTION;                  /* même chemin qu'une action ANCS : réponse d'écriture */
    int rc = ble_gattc_write_flat(c.conn, c.rc.val, &cmd, 1, on_cp_write, GEN_ARG());
    if (rc) { f.ph = PH_NONE; return false; }
    s_cp_inflight = true;
    ESP_LOGI(TAG, "AMS commande %u", cmd);
    ble_npl_callout_reset(&s_co_cmd, ble_npl_time_ms_to_ticks32(CMD_TIMEOUT_MS));
    return true;
}

static int on_ea_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (STALE(arg, conn)) return BLE_HS_EDONE;
    if (f.ph != PH_EA) return BLE_HS_EDONE;   /* commande déjà abandonnée (timeout) */
    if (err->status == 0 && attr) {           /* morceau d'une lecture longue */
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (s_ea_len + n > sizeof(s_ea_buf)) n = sizeof(s_ea_buf) - s_ea_len;   /* tronqué, on lit jusqu'au bout */
        if (n) os_mbuf_copydata(attr->om, 0, n, s_ea_buf + s_ea_len);
        s_ea_len += n;
        return 0;
    }
    s_cp_inflight = false;
    if (err->status == 0 || err->status == BLE_HS_EDONE) {
        store_attr(s_ea_cur >> 8, s_ea_cur & 0xFF, s_ea_buf, s_ea_len);
    }
    finish_cmd();
    return 0;
}

static int on_ea_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (STALE(arg, conn)) return 0;
    if (f.ph != PH_EA) { s_cp_inflight = false; return 0; }
    if (err->status != 0) { s_cp_inflight = false; finish_cmd(); return 0; }
    s_ea_len = 0;
    if (ble_gattc_read_long(c.conn, c.ea.val, 0, on_ea_read, GEN_ARG()) != 0) {
        s_cp_inflight = false;
        finish_cmd();
    }
    return 0;
}

static bool send_ea_fetch(void)
{
    if (!s_ea_n || !c.ea.val) return false;
    s_ea_cur = s_ea_q[0];
    memmove(s_ea_q, s_ea_q + 1, --s_ea_n * sizeof(s_ea_q[0]));
    uint8_t key[2] = { (uint8_t)(s_ea_cur >> 8), (uint8_t)(s_ea_cur & 0xFF) };
    f.ph = PH_EA;
    if (ble_gattc_write_flat(c.conn, c.ea.val, key, 2, on_ea_write, GEN_ARG()) != 0) {
        f.ph = PH_NONE;
        return false;
    }
    s_cp_inflight = true;
    ble_npl_callout_reset(&s_co_cmd, ble_npl_time_ms_to_ticks32(CMD_TIMEOUT_MS));
    return true;
}

void media_get(media_info_t *out)
{
    s_media_ev_posted = false;
    portENTER_CRITICAL(&s_mmux);
    *out = s_media;
    portEXIT_CRITICAL(&s_mmux);
}

float media_position_s(const media_info_t *m)
{
    float p = m->elapsed_s;
    if (m->state == MEDIA_PLAYING && m->elapsed_at_us) {
        float r = m->rate > 0 ? m->rate : 1.0f;
        p += r * (float)(esp_timer_get_time() - m->elapsed_at_us) / 1e6f;
    }
    if (m->duration_s > 0 && p > m->duration_s) p = m->duration_s;
    return p < 0 ? 0 : p;
}

void media_command(uint8_t cmd)
{
    if (!ble_running()) return;
    portENTER_CRITICAL(&s_amux);
    if (s_mcmd_n < MCMD_Q) s_mcmd[s_mcmd_n++] = cmd;
    portEXIT_CRITICAL(&s_amux);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_action);
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
    c.ams_phase = false;
    c.tz_min = INT32_MIN;
    s_ea_n = 0;
    media_reset(false);
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
    c.ams_phase = false;
    q_clear();
    s_ea_n = 0;
    portENTER_CRITICAL(&s_amux);
    s_act_pending = false;
    s_mcmd_n = 0;
    portEXIT_CRITICAL(&s_amux);
    media_reset(false);
}

void ancs_on_notify_rx(uint16_t conn, uint16_t attr, struct os_mbuf *om)
{
    if (conn != c.conn) return;
    if (attr == c.ds.val && c.ds.val) on_data_source(om);
    else if (attr == c.ns.val && c.ns.val) on_notification_source(om);
    else if (attr == c.eu.val && c.eu.val) on_entity_update(om);
    else if (attr == c.rc.val && c.rc.val) on_remote_cmds(om);
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
