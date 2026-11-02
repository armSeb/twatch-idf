/*
 * test_sync.c — relèves de l'« économie d'énergie » Bluetooth (blesync.c)
 * avec une fausse pile BLE, de faux timers et le vrai notif_store.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../main/blesync.c"

int64_t g_fake_time_us;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- esp_timer simulé ---- */
struct esp_timer { esp_timer_cb_t cb; int64_t due, period; bool armed; };
static struct esp_timer T[8]; static int TN;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { T[TN].cb = a->callback; *h = &T[TN++]; return 0; }
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) { h->due = g_fake_time_us + us; h->period = 0; h->armed = true; return 0; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us) { h->due = g_fake_time_us + us; h->period = us; h->armed = true; return 0; }
esp_err_t esp_timer_stop(esp_timer_handle_t h) { h->armed = false; return 0; }

/* ---- bus d'événements : traité comme main.c ---- */
static int32_t Q[64]; static int QN;
bool app_post(const app_event_t *ev) { if (ev->type == EV_SYNC && QN < 64) Q[QN++] = ev->a; return true; }
static int FLUSHED;
static void pump(void)
{
    for (int i = 0; i < QN; i++) {
        if (Q[i] == SYNC_EV_FLUSH) { if (blesync_take_pending()) FLUSHED++; }
        else blesync_on_event(Q[i]);
    }
    QN = 0;
}

/* ---- fausse pile BLE ---- */
static uint32_t FAILS;          /* échecs d'établissement simulés */
static bool RUN; static int STARTS, STOPS; static ble_state_t ST = BLE_ST_OFF;
static bool IPHONE = true;      /* l'iPhone se reconnecte-t-il ? */
static int64_t RUN_T;
esp_err_t ble_start(void) { if (!RUN) { RUN = true; STARTS++; FAILS = 0; RUN_T = g_fake_time_us; ST = BLE_ST_ADVERTISING; } return 0; }
void ble_stop(void) { if (RUN) { RUN = false; STOPS++; ST = BLE_ST_OFF; ns_invalidate_uids(); } }
bool ble_running(void) { return RUN; }
uint32_t ble_conn_fails(void) { return FAILS; }
void ble_diag(const char *fmt, ...) { (void)fmt; }
void ble_get_info(ble_info_t *o) { memset(o, 0, sizeof(*o)); o->state = ST; }
static bool IDLE = true;
bool ancs_idle(void) { return ST == BLE_ST_READY && IDLE; }
bool ancs_ready(void) { return ST == BLE_ST_READY; }
static bool OTA;
bool ota_active(void) { return OTA; }

static settings_t S = { .ble_enabled = true, .ble_eco = true, .eco_min = 5 };
settings_t *settings_get(void) { return &S; }
void wclock_local_tm(struct tm *o) { time_t t = 1793430000 + g_fake_time_us / 1000000; gmtime_r(&t, o); }

/* avance le temps par pas de 100 ms en déclenchant timers et événements */
static void run_ms(int ms)
{
    for (int k = 0; k < ms / 100; k++) {
        g_fake_time_us += 100000;
        /* l'iPhone se connecte ~3 s après le début de la publicité */
        if (RUN && IPHONE && ST == BLE_ST_ADVERTISING && g_fake_time_us - RUN_T > 3000000) ST = BLE_ST_READY;
        for (int i = 0; i < TN; i++) {
            if (T[i].armed && g_fake_time_us >= T[i].due) {
                if (T[i].period) T[i].due += T[i].period; else T[i].armed = false;
                T[i].cb(NULL);
            }
        }
        pump();
    }
}

static void notif(uint32_t uid, const char *title, bool pre, struct tm *at)
{
    ancs_notif_t n; memset(&n, 0, sizeof(n));
    n.uid = uid; n.category = 4; n.flags = pre ? ANCS_FLAG_PREEXISTING : 0;
    strcpy(n.app_id, "app"); strcpy(n.app_name, "App"); strcpy(n.title, title); strcpy(n.message, "m");
    char d[48];
    snprintf(d, sizeof(d), "%04d%02d%02dT%02d%02d%02d", at->tm_year + 1900, at->tm_mon + 1,
             at->tm_mday, at->tm_hour, at->tm_min, at->tm_sec);
    strncpy(n.date, d, sizeof(n.date) - 1);
    bool alert;
    notif_entry_t *e = ns_add(&n, &alert);
    blesync_note_notif();
    if (alert) blesync_defer_alert(e->hash);
}


int main(void)
{
    g_fake_time_us = 1000000;
    blesync_init();
    CHECK(RUN && STARTS == 1 && blesync_in_session());         /* relève immédiate */
    run_ms(15000);
    CHECK(!RUN && STOPS == 1 && !blesync_in_session());        /* coupée une fois au calme */
    CHECK(blesync_sleeping() && blesync_last_sync_s() >= 0);

    /* pendant la coupure, deux notifs arrivent sur l'iPhone */
    run_ms(60000);
    struct tm t1; wclock_local_tm(&t1);
    while (STARTS < 2) run_ms(100);                             /* la relève périodique part */
    CHECK(RUN && g_fake_time_us < 6LL * 60 * 1000000);
    while (ST != BLE_ST_READY) run_ms(100);                     /* iPhone reconnecté, replay iOS */
    notif(50, "Pendant 1", true, &t1);
    notif(51, "Pendant 2", true, &t1);
    CHECK(FLUSHED == 0);
    run_ms(2000);
    CHECK(FLUSHED == 1);                                        /* une seule alerte pour le lot */
    run_ms(10000);
    CHECK(!RUN && STOPS == 2);

    /* un écran qui a besoin de la liaison : relève immédiate, tenue tant qu'il est ouvert */
    blesync_hold(true);
    CHECK(RUN && STARTS == 3);
    run_ms(60000);
    CHECK(RUN);
    blesync_hold(false);
    run_ms(15000);
    CHECK(RUN);                                                 /* délai de grâce de 20 s */
    run_ms(15000);
    CHECK(!RUN && STOPS == 3);

    /* iPhone absent : abandon après 45 s, nouvel essai à la période suivante */
    IPHONE = false;
    run_ms(5 * 60000);
    CHECK(STARTS == 4);
    run_ms(50000);
    CHECK(!RUN && STOPS == 4);
    IPHONE = true;

    /* passage en liaison permanente puis en mode avion */
    S.ble_eco = false; blesync_apply();
    CHECK(RUN);
    run_ms(10 * 60000);
    CHECK(RUN);                                                 /* plus de coupure */
    S.ble_enabled = false; blesync_apply();
    CHECK(!RUN);
    run_ms(10 * 60000);
    CHECK(!RUN);                                                /* mode avion : jamais de relève */
    S.ble_enabled = true; S.ble_eco = true; blesync_apply();
    CHECK(RUN);
    run_ms(15000);
    CHECK(!RUN);

    /* l'iPhone échoue en boucle à établir la liaison : une seule relance de la pile */
    run_ms(5 * 60000);
    {
        int st0 = STARTS, sp0 = STOPS;
        IPHONE = false;
        run_ms(5 * 60000);                         /* nouvelle relève */
        CHECK(RUN && STARTS == st0 + 1);
        FAILS = 6; run_ms(1500);
        CHECK(RUN && STARTS == st0 + 2 && STOPS == sp0 + 1);  /* relancée */
        FAILS = 9; run_ms(3000);
        CHECK(RUN && STARTS == st0 + 2);           /* pas une deuxième fois */
        IPHONE = true; run_ms(15000);
        CHECK(!RUN);                               /* puis relève normale */
    }

    /* mise à jour Wi-Fi en cours : pas de relève */
    OTA = true;
    int s0 = STARTS;
    run_ms(10 * 60000);
    CHECK(STARTS == s0 && !RUN);

    printf(fails ? "\nSYNC : %d ÉCHEC(S)\n" : "SYNC : TOUS LES TESTS PASSENT\n", fails);
    return fails != 0;
}
