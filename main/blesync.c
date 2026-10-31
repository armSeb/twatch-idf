/*
 * blesync.c — voir blesync.h.
 *
 * Une relève : ble_start() → l'iPhone (lié) se reconnecte de lui-même à la
 * publicité → chiffrement → ANCS rejoue les notifications présentes (drapeau
 * PreExisting), dont celles arrivées pendant la coupure ; AMS et l'heure
 * (CTS) sont mis à jour au passage. Quand ANCS est au calme depuis quelques
 * secondes et qu'aucun écran ne retient la liaison : ble_stop().
 *
 * Les notifications PreExisting plus récentes que la fin de la relève
 * précédente sont traitées comme nouvelles (notif_store), et leurs alertes
 * regroupées en une seule. Celles qui ne sont plus rejouées (effacées sur
 * l'iPhone entre-temps) sont retirées de la montre.
 */
#include <stdio.h>
#include <time.h>
#include "blesync.h"
#include "ble.h"
#include "settings.h"
#include "notif_store.h"
#include "wclock.h"
#include "app_events.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "ota.h"

#define TAG "sync"

#define TICK_MS          1000
#define QUIET_MS         5000     /* ANCS calme depuis… avant de couper */
#define CONNECT_MAX_MS   45000    /* l'iPhone ne vient pas : on réessaiera plus tard */
#define SESSION_MAX_MS   120000   /* plafond d'une relève sans écran qui la retient */
#define HOLD_LINGER_MS   20000    /* garder la liaison un peu après la fermeture d'un écran */
#define FLUSH_MS         1500     /* regroupement des alertes */

extern bool ancs_idle(void);     /* ancs.c (ancs.h tire tout NimBLE) */
extern bool ancs_ready(void);

static esp_timer_handle_t s_period, s_tick, s_flush;
static bool     s_session;
static int64_t  s_t0, s_ready_t, s_last_notif_t, s_release_t;
static int64_t  s_last_ok_t = -1;
static int      s_holds;
static uint32_t s_pending_hash;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void post(int code) { app_post_simple(EV_SYNC, code); }
static void period_cb(void *a) { post(SYNC_EV_START); }
static void tick_cb(void *a)   { post(SYNC_EV_TICK); }
static void flush_cb(void *a)  { post(SYNC_EV_FLUSH); }

bool blesync_eco(void)
{
    const settings_t *s = settings_get();
    return s->ble_enabled && s->ble_eco;
}

bool blesync_sleeping(void) { return blesync_eco() && !ble_running(); }
bool blesync_in_session(void) { return blesync_eco() && s_session; }

int64_t blesync_last_sync_s(void)
{
    return s_last_ok_t < 0 ? -1 : (now_ms() - s_last_ok_t) / 1000;
}

static uint32_t interval_ms(void)
{
    uint8_t m = settings_get()->eco_min;
    if (m != 2 && m != 5 && m != 15) m = 5;
    return (uint32_t)m * 60u * 1000u;
}

/* « depuis » au format des dates ANCS (yyyyMMddTHHmmSS, heure locale) */
static void set_since_now(int back_s)
{
    struct tm tm;
    wclock_local_tm(&tm);
    time_t t = mktime(&tm) - back_s;
    localtime_r(&t, &tm);
    char d[40];
    snprintf(d, sizeof(d), "%04d%02d%02dT%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    ns_set_alert_since(d);
}

static void session_begin(void)
{
    if (s_session || ota_active()) return;   /* la mise à jour Wi-Fi a coupé le Bluetooth */
    s_session = true;
    s_t0 = now_ms();
    s_ready_t = s_last_notif_t = 0;
    ESP_LOGI(TAG, "relève");
    ble_start();
    esp_timer_start_periodic(s_tick, TICK_MS * 1000);
}

static void session_end(bool ok)
{
    if (!s_session) return;
    esp_timer_stop(s_tick);
    s_session = false;
    if (ok) {
        /* replay terminé : ce qui n'a pas été rejoué a été effacé sur l'iPhone */
        int n = ns_prune_invalid();
        if (n) ESP_LOGI(TAG, "%d notif(s) effacée(s) sur l'iPhone retirée(s)", n);
        s_last_ok_t = now_ms();
    }
    set_since_now(120);            /* marge : décalage d'horloge, notif à cheval */
    ble_stop();
    ESP_LOGI(TAG, "fin de relève (%s, %lld ms)", ok ? "ok" : "iPhone absent", (long long)(now_ms() - s_t0));
    if (s_pending_hash) {          /* une alerte en attente : la servir tout de suite */
        esp_timer_stop(s_flush);
        post(SYNC_EV_FLUSH);
    }
}

static void tick(void)
{
    if (!s_session) return;
    int64_t t = now_ms();
    ble_info_t bi;
    ble_get_info(&bi);
    if (bi.state == BLE_ST_READY && !s_ready_t) s_ready_t = t;

    if (s_holds > 0 || (s_release_t && t - s_release_t < HOLD_LINGER_MS)) return;   /* écran en cours */
    if (!s_ready_t) {
        if (t - s_t0 > CONNECT_MAX_MS) session_end(false);
        return;
    }
    int64_t calm_since = s_last_notif_t > s_ready_t ? s_last_notif_t : s_ready_t;
    if ((ancs_idle() && t - calm_since > QUIET_MS) || t - s_t0 > SESSION_MAX_MS) session_end(true);
}

void blesync_apply(void)
{
    const settings_t *s = settings_get();
    esp_timer_stop(s_period);
    if (!s->ble_enabled) {                         /* mode avion */
        if (s_session) { esp_timer_stop(s_tick); s_session = false; }
        ns_set_alert_since(NULL);
        ble_stop();
        return;
    }
    if (!s->ble_eco) {                             /* liaison permanente */
        if (s_session) { esp_timer_stop(s_tick); s_session = false; }
        ns_set_alert_since(NULL);
        ble_start();
        return;
    }
    /* économie : relève maintenant, puis à intervalle régulier */
    esp_timer_start_periodic(s_period, (uint64_t)interval_ms() * 1000);
    if (ble_running() && !s_session) {
        /* on passait d'une liaison permanente à l'économie : clore proprement */
        s_session = true;
        s_t0 = now_ms();
        s_ready_t = ancs_ready() ? s_t0 : 0;
        esp_timer_start_periodic(s_tick, TICK_MS * 1000);
    } else if (!ble_running()) {
        set_since_now(0);
        session_begin();
    }
}

void blesync_init(void)
{
    const esp_timer_create_args_t a = { .callback = period_cb, .name = "sync" };
    const esp_timer_create_args_t b = { .callback = tick_cb, .name = "sync_tick" };
    const esp_timer_create_args_t c = { .callback = flush_cb, .name = "sync_flush" };
    esp_timer_create(&a, &s_period);
    esp_timer_create(&b, &s_tick);
    esp_timer_create(&c, &s_flush);
    blesync_apply();
}

void blesync_hold(bool on)
{
    if (on) s_holds++;
    else if (s_holds > 0) { s_holds--; if (!s_holds) s_release_t = now_ms(); }
    if (on && blesync_eco() && !s_session && !ble_running()) session_begin();
}

void blesync_note_notif(void)
{
    s_last_notif_t = now_ms();
}

bool blesync_defer_alert(uint32_t hash)
{
    if (!blesync_in_session()) return false;
    s_pending_hash = hash;                         /* la plus récente du lot */
    esp_timer_stop(s_flush);
    esp_timer_start_once(s_flush, FLUSH_MS * 1000);
    return true;
}

uint32_t blesync_take_pending(void)
{
    uint32_t h = s_pending_hash;
    s_pending_hash = 0;
    return h;
}

void blesync_on_event(int32_t code)
{
    switch (code) {
    case SYNC_EV_START:
        if (blesync_eco() && !s_session && !ble_running()) session_begin();
        break;
    case SYNC_EV_TICK:
        tick();
        break;
    default:
        break;
    }
}
