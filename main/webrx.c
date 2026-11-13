/*
 * webrx.c — voir webrx.h.
 *
 * Protocole OpenWebRX (htdocs/openwebrx.js, owrx/connection.py) :
 *  - WebSocket sur <chemin du site>/ws/ ; le client envoie
 *    « SERVER DE CLIENT client=… type=receiver », le serveur répond
 *    « CLIENT DE SERVER … » puis envoie du JSON {"type":…, "value":…}
 *    (config, profiles, bookmarks, smeter, sdr_error, backoff…) ;
 *  - le client règle ses débits (connectionproperties), choisit le profil
 *    (selectprofile) et pilote le démodulateur (dspcontrol : mod,
 *    offset_freq, low_cut, high_cut, squelch_level ; action start) ;
 *  - trames binaires : 1er octet = 1 FFT, 2 audio, 3 FFT secondaire,
 *    4 audio « HD » (WFM). Audio en IMA-ADPCM avec mots de synchronisation
 *    « SYNC » + index de pas + prédicteur tous les 1000 octets ; FFT en
 *    ADPCM remis à zéro à chaque trame, 10 échantillons de bourrage,
 *    valeurs en centièmes de dB.
 */
#include "webrx.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "mbedtls/base64.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "i18n.h"
#include "net.h"
#include "ble.h"
#include "power.h"
#include "sound.h"
#include "webrx_cfg.h"

static const char *TAG = "webrx";

#define RING      32768u                      /* échantillons (2,7 s), puissance de 2 */
#define PREBUF    (WRX_RATE * 4 / 10)         /* avance visée au (re)démarrage */
#define HIGH_MARK (WRX_RATE * 12 / 10)        /* au-delà : on rattrape (un échantillon sauté par bloc) */
#define FLUSH_MARK (WRX_RATE * 2)             /* au-delà : on jette l'excédent d'un coup */
#define LOW_MARK  (WRX_RATE * 15 / 100)       /* en deçà : on étire (un échantillon doublé) */
#define CHUNK     240                         /* 20 ms */
#define TXT_MAX   32768                       /* messages texte plus longs ignorés (liste des modes…) */
#define BIN_MAX   20480                       /* trame FFT (fft_size jusqu'à 32768) */
#define NROWS     4
#define CONNECT_MS 20000                      /* TLS + mise à niveau WebSocket (serveur lent) */

#define BIT_IP    BIT0
#define BIT_DISC  BIT1
#define BIT_QUIT  BIT2
#define BIT_NETEND BIT3
#define BIT_PLAYEND BIT4
#define BIT_RETRY BIT5

/* ------------------------------------------------------------------ */
/* état partagé (s_mx)                                                  */
/* ------------------------------------------------------------------ */
static SemaphoreHandle_t s_mx;
static EventGroupHandle_t s_evg;
static volatile bool s_run, s_quit;
static bool s_wifi_inited, s_ble_paused;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;
static volatile int s_disc_reason;

static wrx_info_t s_info;
static wrx_bm_t *s_bm;   static int s_nbm;
static wrx_prof_t *s_prof; static int s_nprof;
static wrx_mode_t *s_modes; static int s_nmodes;           /* analogiques (écoutables) */
#define DIGI_MAX 48
static struct { char mod[16]; char under[16]; int lo, hi; bool bp; } *s_digi;   /* numériques → BLU… */
static int s_ndigi;
static wrx_state_t s_st;                     /* réglages durables (NVS) */
static wrx_cfg_t *s_cfg_p;                   /* PSRAM */
#define s_cfg (*s_cfg_p)

enum { PEND_DSP = 1, PEND_PROFILE = 2, PEND_START = 4 };
static uint32_t s_pend;
static char s_pend_prof[72];

/* audio */
static int16_t *s_ring;
static volatile uint32_t s_wr, s_rd;
static volatile bool s_audio_on;

/* waterfall */
static volatile bool s_fft_on;
static volatile int s_zoom = 1;
static wrx_row_t *s_rows;
static uint32_t s_row_w, s_row_r;

/* session serveur (tâche wrx) */
static esp_transport_handle_t s_par, s_ws;
static volatile int s_sock = -1;
static char *s_txt;   static int s_txt_n;
static uint8_t *s_bin; static int s_bin_n;
static int s_fft_size = 0;
static bool s_adpcm = true, s_fft_adpcm = true, s_have_cfg, s_started, s_prof_tried;
static int64_t s_prof_wait_us;               /* > 0 : en attente de la config du profil demandé */
static int64_t s_start_freq;                 /* fréquence / mode de départ du profil */
static char s_start_mod[16];
static bool s_backoff;                       /* le serveur a refusé le client (« backoff ») */
static char s_last_type[20];                 /* type du dernier message texte reçu */

static inline void lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_mx); }

static void *ps_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void set_state(int st, const char *msg, const char *detail)
{
    lock();
    s_info.state = st;
    s_info.msg = msg;
    strlcpy(s_info.detail, detail ? detail : "", sizeof s_info.detail);
    s_info.retry_s = 0;
    unlock();
}

/* ------------------------------------------------------------------ */
/* IMA-ADPCM (comme lib/AudioEngine.js : le pas n'est pas recalculé à la */
/* synchronisation, seul l'index l'est)                                 */
/* ------------------------------------------------------------------ */
static const int8_t IDX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

typedef struct {
    int index, pred, step;
    int phase, sync, cnt;
    uint8_t sb[4]; int sbi;
} adpcm_t;

static inline int16_t nib(adpcm_t *c, int n)
{
    c->index += IDX[n];
    if (c->index < 0) c->index = 0;
    if (c->index > 88) c->index = 88;
    int d = c->step >> 3;
    if (n & 1) d += c->step >> 2;
    if (n & 2) d += c->step >> 1;
    if (n & 4) d += c->step;
    if (n & 8) d = -d;
    c->pred += d;
    if (c->pred > 32767) c->pred = 32767;
    if (c->pred < -32768) c->pred = -32768;
    c->step = STEP[c->index];
    return (int16_t)c->pred;
}

static adpcm_t s_dec;

/* flux audio avec synchronisation → échantillons (out : 2 par octet) */
static int adpcm_sync_decode(adpcm_t *c, const uint8_t *in, int n, int16_t *out)
{
    static const char W[4] = { 'S', 'Y', 'N', 'C' };
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint8_t b = in[i];
        switch (c->phase) {
        case 0:
            if (b != (uint8_t)W[c->sync++]) c->sync = 0;
            if (c->sync == 4) { c->sbi = 0; c->phase = 1; }
            break;
        case 1:
            c->sb[c->sbi++] = b;
            if (c->sbi == 4) {
                c->index = (int16_t)(c->sb[0] | c->sb[1] << 8);
                c->pred = (int16_t)(c->sb[2] | c->sb[3] << 8);
                if (c->index < 0) c->index = 0;
                if (c->index > 88) c->index = 88;
                c->cnt = 1000;
                c->phase = 2;
            }
            break;
        default:
            out[o++] = nib(c, b & 0x0F);
            out[o++] = nib(c, b >> 4);
            if (c->cnt-- == 0) { c->sync = 0; c->phase = 0; }
            break;
        }
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* tampon audio                                                         */
/* ------------------------------------------------------------------ */
static void ring_push(const int16_t *s, int n)
{
    uint32_t used = s_wr - s_rd;
    if (used + (uint32_t)n > RING) return;          /* plein : le lecteur rattrapera */
    for (int i = 0; i < n; i++) s_ring[(s_wr + i) & (RING - 1)] = s[i];
    s_wr += n;
}

static int16_t *s_pcm;                       /* 2048 échantillons décodés (PSRAM) */
static uint8_t *s_rb;                        /* lecture réseau, 2 Ko (PSRAM) */
#define RB_LEN 2048

static void audio_bytes(const uint8_t *p, int n)
{
    int16_t *tmp = s_pcm;
    static uint8_t odd; static bool has_odd;        /* PCM 16 bits : octet orphelin */
    while (n > 0) {
        int k = n > 1024 ? 1024 : n;
        int m;
        if (s_adpcm) {
            m = adpcm_sync_decode(&s_dec, p, k, tmp);
        } else {
            m = 0;
            int i = 0;
            if (has_odd) { tmp[m++] = (int16_t)(odd | p[0] << 8); i = 1; has_odd = false; }
            for (; i + 1 < k; i += 2) tmp[m++] = (int16_t)(p[i] | p[i + 1] << 8);
            if (i < k) { odd = p[i]; has_odd = true; }
        }
        ring_push(tmp, m);
        p += k;
        n -= k;
    }
}

/* Tonalité pour le petit haut-parleur, par paliers : passe-haut (le HP ne
 * rend pas le grave, autant garder la marge), bosse vers 600–800 Hz (corps
 * de la voix), passe-bas de plus en plus bas et raide (souffle FM, aigus
 * agressifs). Biquads RBJ, forme directe II transposée, en flottant (FPU). */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } bq_t;
static bq_t s_bq[5];
static int s_nbq;

static void bq_set(bq_t *q, float b0, float b1, float b2, float a0, float a1, float a2)
{
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0; q->a1 = a1 / a0; q->a2 = a2 / a0;
    q->z1 = q->z2 = 0;
}

static void bq_hp(bq_t *q, float f, float Q)
{
    float w = 2 * (float)M_PI * f / WRX_RATE, c = cosf(w), al = sinf(w) / (2 * Q);
    bq_set(q, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

static void bq_lp(bq_t *q, float f, float Q)
{
    float w = 2 * (float)M_PI * f / WRX_RATE, c = cosf(w), al = sinf(w) / (2 * Q);
    bq_set(q, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

static void bq_peak(bq_t *q, float f, float Q, float db)
{
    float A = powf(10, db / 40), w = 2 * (float)M_PI * f / WRX_RATE, c = cosf(w), al = sinf(w) / (2 * Q);
    bq_set(q, 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

/* palier → filtres. Passe-bas d'ordre 4 (deux étages de Butterworth,
 * Q 0,54 et 1,31) à partir du palier 3 */
static void filter_init(int tone)
{
    s_nbq = 0;
    switch (tone) {
    case 1:                                       /* claire */
        bq_hp(&s_bq[s_nbq++], 150, 0.7071f);
        bq_peak(&s_bq[s_nbq++], 800, 0.8f, 3);
        bq_lp(&s_bq[s_nbq++], 3200, 0.7071f);
        break;
    case 2:                                       /* voix (0.22.0) */
        bq_hp(&s_bq[s_nbq++], 180, 0.7071f);
        bq_peak(&s_bq[s_nbq++], 700, 0.8f, 5);
        bq_lp(&s_bq[s_nbq++], 2600, 0.7071f);
        break;
    case 3:                                       /* douce */
        bq_hp(&s_bq[s_nbq++], 180, 0.7071f);
        bq_peak(&s_bq[s_nbq++], 650, 0.8f, 5);
        bq_lp(&s_bq[s_nbq++], 2200, 0.5412f);
        bq_lp(&s_bq[s_nbq++], 2200, 1.3066f);
        break;
    case 4:                                       /* sourde */
        bq_hp(&s_bq[s_nbq++], 200, 0.7071f);
        bq_peak(&s_bq[s_nbq++], 600, 0.8f, 6);
        bq_lp(&s_bq[s_nbq++], 1700, 0.5412f);
        bq_lp(&s_bq[s_nbq++], 1700, 1.3066f);
        break;
    default:                                      /* neutre */
        break;
    }
}

static void filter_run(int16_t *x, int n)
{
    if (!s_nbq) return;
    for (int i = 0; i < n; i++) {
        float v = x[i];
        for (int k = 0; k < s_nbq; k++) {
            bq_t *q = &s_bq[k];
            float y = q->b0 * v + q->z1;
            q->z1 = q->b1 * v - q->a1 * y + q->z2;
            q->z2 = q->b2 * v - q->a2 * y;
            v = y;
        }
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        x[i] = (int16_t)v;
    }
}

static void play_task(void *arg)
{
    int16_t buf[CHUNK + 1];                      /* pile de la tâche (≈ 0,5 Ko) */
    bool open = false, buffering = true;
    int tone = -1;
    while (!s_quit) {
        if (!s_audio_on) {
            if (open) { sound_stream_close(); open = false; }
            s_rd = s_wr;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!open) {
            if (sound_stream_open(WRX_RATE) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
            open = true;
            buffering = true;
        }
        uint32_t avail = s_wr - s_rd;
        if (buffering) {
            if (avail >= PREBUF) buffering = false;
            else { sound_stream_write(NULL, CHUNK, 0); continue; }   /* silence : garde le rythme */
        }
        if (avail < CHUNK) {                       /* plus rien : on se recale */
            buffering = true;
            continue;
        }
        if (avail > FLUSH_MARK) { s_rd = s_wr - PREBUF; avail = PREBUF; }
        int take = CHUNK;
        if (avail > HIGH_MARK) take = CHUNK + 1;           /* léger rattrapage */
        else if (avail < LOW_MARK) take = CHUNK - 1;       /* léger étirement */
        for (int i = 0; i < take; i++) buf[i] = s_ring[(s_rd + i) & (RING - 1)];
        s_rd += take;
        if (take == CHUNK - 1) buf[CHUNK - 1] = buf[CHUNK - 2];
        else if (take == CHUNK + 1) {               /* deux échantillons en un */
            buf[CHUNK - 1] = (int16_t)(((int)buf[CHUNK - 1] + buf[CHUNK]) / 2);
        }
        if (s_info.tone != tone) { tone = s_info.tone; filter_init(tone); }
        filter_run(buf, CHUNK);
        int v = s_info.volume;
        int gain = v * v * 8192 / 10000;           /* courbe douce, ×2 à 100 % */
        sound_stream_write(buf, CHUNK, gain);
        lock();
        s_info.buf_ms = (int)((s_wr - s_rd) * 1000 / WRX_RATE);
        s_info.buffering = buffering;
        unlock();
    }
    if (open) sound_stream_close();
    xEventGroupSetBits(s_evg, BIT_PLAYEND);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Wi-Fi                                                                */
/* ------------------------------------------------------------------ */
static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_disc_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        xEventGroupClearBits(s_evg, BIT_IP);
        xEventGroupSetBits(s_evg, BIT_DISC);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_evg, BIT_IP);
    }
}

static bool wifi_connect(void)
{
    set_state(WRX_WIFI, N_("Recherche du Wi-Fi…"), "");
    bool seen[WRX_NETS] = { 0 };
    int nseen = 0;
    wifi_scan_config_t sc = { .show_hidden = true };
    sc.scan_time.active.min = 40;
    sc.scan_time.active.max = 100;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 24;
        wifi_ap_record_t *rec = ps_malloc(n * sizeof *rec);
        if (rec && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
            for (int i = 0; i < n; i++)
                for (int k = 0; k < WRX_NETS; k++)
                    if (s_cfg.ssid[k][0] && !strcmp((const char *)rec[i].ssid, s_cfg.ssid[k]) && !seen[k]) {
                        seen[k] = true;
                        nseen++;
                    }
        } else {
            esp_wifi_clear_ap_list();
        }
        free(rec);
    }
    if (s_quit) return false;
    /* réseaux vus, dans l'ordre de préférence ; aucun vu : tous (réseau masqué) */
    int order[WRX_NETS], no = 0;
    for (int k = 0; k < WRX_NETS; k++) if (s_cfg.ssid[k][0] && (seen[k] || !nseen)) order[no++] = k;
    bool bad_pass = false;
    int last_reason = 0;
    for (int j = 0; j < no && !s_quit; j++) {
        int k = order[j];
        set_state(WRX_WIFI, N_("Connexion au Wi-Fi"), s_cfg.ssid[k]);
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, s_cfg.ssid[k], sizeof wc.sta.ssid);
        strlcpy((char *)wc.sta.password, s_cfg.pass[k], sizeof wc.sta.password);
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wc.sta.listen_interval = 3;              /* modem sleep : réveil toutes les 3 balises */
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        xEventGroupClearBits(s_evg, BIT_IP | BIT_DISC);
        s_disc_reason = 0;
        esp_wifi_connect();
        EventBits_t b = xEventGroupWaitBits(s_evg, BIT_IP | BIT_DISC | BIT_QUIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        if (b & BIT_IP) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { lock(); s_info.rssi = ap.rssi; unlock(); }
            ESP_LOGI(TAG, "Wi-Fi « %s » connecté", s_cfg.ssid[k]);
            return true;
        }
        int r = s_disc_reason;
        last_reason = r;
        if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE) bad_pass = true;
        ESP_LOGW(TAG, "Wi-Fi « %s » : échec (raison %d)", s_cfg.ssid[k], r);
        ble_diag("WRX wifi %.16s r%d int %u Ko", s_cfg.ssid[k], r,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_evg, BIT_DISC | BIT_QUIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
    }
    if (s_quit) return false;
    if (bad_pass) set_state(WRX_FAIL, N_("Mot de passe Wi-Fi refusé"), "");
    else if (!nseen) set_state(WRX_FAIL, N_("Aucun réseau connu à portée"), "");
    else {
        char d[32];
        if (last_reason) snprintf(d, sizeof d, _("raison %d"), last_reason);
        else snprintf(d, sizeof d, "%s", _("pas de réponse du réseau"));
        set_state(WRX_FAIL, N_("Connexion Wi-Fi impossible"), d);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Adresse du serveur                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    bool tls;
    char host[96];
    int  port;
    char path[160];
    char auth[128];          /* « Basic … » ou "" */
} url_t;

static bool url_parse(const char *u, url_t *o)
{
    memset(o, 0, sizeof *o);
    if (!strncmp(u, "https://", 8)) { o->tls = true; u += 8; o->port = 443; }
    else if (!strncmp(u, "http://", 7)) { u += 7; o->port = 80; }
    else return false;
    const char *end = u + strcspn(u, "/?#");
    const char *at = memchr(u, '@', end - u);
    if (at) {                                        /* identifiants (authentification HTTP) */
        char cred[96];
        size_t l = at - u;
        if (l >= sizeof cred) return false;
        memcpy(cred, u, l);
        cred[l] = 0;
        size_t olen = 0;
        strcpy(o->auth, "Basic ");
        if (mbedtls_base64_encode((unsigned char *)o->auth + 6, sizeof o->auth - 6, &olen,
                                  (const unsigned char *)cred, l)) return false;
        o->auth[6 + olen] = 0;
        u = at + 1;
    }
    const char *colon = NULL;
    if (*u == '[') {                                 /* IPv6 littérale */
        const char *rb = memchr(u, ']', end - u);
        if (!rb) return false;
        if (rb + 1 < end && rb[1] == ':') colon = rb + 1;
        size_t l = rb - u - 1;
        if (l >= sizeof o->host) return false;
        memcpy(o->host, u + 1, l);
    } else {
        colon = memchr(u, ':', end - u);
        size_t l = (colon ? colon : end) - u;
        if (!l || l >= sizeof o->host) return false;
        memcpy(o->host, u, l);
    }
    if (colon) {
        o->port = atoi(colon + 1);
        if (o->port <= 0 || o->port > 65535) return false;
    }
    /* chemin du site (OpenWebRX derrière un proxy), puis « ws/ » */
    size_t pl = strcspn(end, "?#");
    if (pl + 5 >= sizeof o->path) return false;
    if (pl == 0) strcpy(o->path, "/");
    else { memcpy(o->path, end, pl); o->path[pl] = 0; }
    if (o->path[strlen(o->path) - 1] != '/') strcat(o->path, "/");
    strcat(o->path, "ws/");
    return true;
}

/* ------------------------------------------------------------------ */
/* Session WebSocket                                                    */
/* ------------------------------------------------------------------ */
static void server_close(void)
{
    lock();
    s_sock = -1;
    unlock();
    if (s_ws) { esp_transport_close(s_ws); esp_transport_destroy(s_ws); s_ws = NULL; }
    if (s_par) { esp_transport_destroy(s_par); s_par = NULL; }
}

/* Message texte en UNE écriture (en-tête + clé de masque + charge masquée).
 * esp_transport_ws envoie l'en-tête et la charge en deux écritures, donc deux
 * enregistrements TLS / segments TCP : le serveur OpenWebRX, qui lit son
 * socket non bloquant par morceaux exacts, perd alors la trame et ferme la
 * connexion aussitôt (« lecture impossible », 0 octet reçu). */
static bool ws_text(const char *s)
{
    static uint8_t *fr;
    if (!fr) fr = ps_malloc(520);
    if (!s_ws || !s_par || !fr) return false;
    size_t n = strlen(s);
    if (n > 500) return false;
    size_t h = 0;
    fr[h++] = 0x81;                                    /* FIN + texte */
    if (n < 126) fr[h++] = 0x80 | (uint8_t)n;
    else { fr[h++] = 0x80 | 126; fr[h++] = (uint8_t)(n >> 8); fr[h++] = (uint8_t)n; }
    uint32_t key = esp_random();
    memcpy(fr + h, &key, 4);
    const uint8_t *m = fr + h;
    h += 4;
    for (size_t i = 0; i < n; i++) fr[h + i] = (uint8_t)s[i] ^ m[i & 3];
    int len = (int)(h + n);
    return esp_transport_write(s_par, (const char *)fr, len, 10000) == len;
}

static bool server_connect(void)
{
    url_t u;
    if (!url_parse(s_cfg.srv[s_st.server].url, &u)) { set_state(WRX_FAIL, N_("Adresse du serveur invalide"), ""); return false; }
    set_state(WRX_SERVER, N_("Connexion au serveur"), u.host);
    if (u.tls) {
        s_par = esp_transport_ssl_init();
        if (s_par) esp_transport_ssl_crt_bundle_attach(s_par, esp_crt_bundle_attach);
    } else {
        s_par = esp_transport_tcp_init();
    }
    s_ws = s_par ? esp_transport_ws_init(s_par) : NULL;
    if (!s_ws) { server_close(); set_state(WRX_FAIL, N_("Mémoire insuffisante"), ""); return false; }
    esp_transport_ws_set_path(s_ws, u.path);
    esp_transport_ws_set_user_agent(s_ws, "T-Watch-WebRX");
    if (u.auth[0]) esp_transport_ws_set_auth(s_ws, u.auth);
    int r = esp_transport_connect(s_ws, u.host, u.port, CONNECT_MS);
    if (r < 0) {
        int code = esp_transport_ws_get_upgrade_request_status(s_ws);
        char d[48];
        server_close();
        if (s_quit) return false;
        if (code == 401 || code == 403) {
            snprintf(d, sizeof d, "HTTP %d", code);
            set_state(WRX_FAIL, N_("Accès refusé par le serveur"), d);
        } else if (code > 0) {
            snprintf(d, sizeof d, "HTTP %d", code);
            set_state(WRX_FAIL, N_("Pas de WebSocket OpenWebRX à cette adresse"), d);
        } else {
            set_state(WRX_FAIL, u.tls ? N_("Serveur injoignable ou certificat refusé") : N_("Serveur injoignable"), u.host);
        }
        ESP_LOGW(TAG, "connexion à %s:%d%s : échec (HTTP %d)", u.host, u.port, u.path, code);
        ble_diag("WRX %.18s HTTP %d int %u Ko", u.host, code,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        return false;
    }
    lock();
    s_sock = esp_transport_get_socket(s_ws);
    unlock();
    ESP_LOGI(TAG, "connecté à %s:%d%s", u.host, u.port, u.path);
    if (!ws_text("SERVER DE CLIENT client=twatch type=receiver")) {
        server_close();
        set_state(WRX_FAIL, N_("Le serveur ne répond pas"), u.host);
        return false;
    }
    char b[96];
    snprintf(b, sizeof b, "{\"type\":\"connectionproperties\",\"params\":{\"output_rate\":%d,\"hd_output_rate\":%d}}",
             WRX_RATE, WRX_RATE);
    if (!ws_text(b)) {
        server_close();
        set_state(WRX_FAIL, N_("Le serveur ne répond pas"), u.host);
        return false;
    }
    return true;
}

/* largeur de bande des modes de base (owrx/modes.py), tant que le serveur
 * n'a pas envoyé sa liste ; il ne l'applique pas de lui-même quand le client
 * change de mode */
static const struct { const char *mod; int lo, hi; } BPF[] = {
    { "nfm", -4000, 4000 }, { "wfm", -75000, 75000 }, { "am", -4000, 4000 },
    { "lsb", -3000, -300 }, { "usb", 300, 3000 }, { "cw", 700, 900 },
    { "dmr", -6250, 6250 }, { "dstar", -3250, 3250 }, { "nxdn", -3250, 3250 },
    { "ysf", -6250, 6250 }, { "m17", -6250, 6250 }, { "sam", -4000, 4000 },
};

/* bande passante d'un mode (appelant verrouillé) ; false si inconnue */
static bool mode_bp(const char *mod, int *lo, int *hi)
{
    for (int i = 0; i < s_nmodes; i++)
        if (!strcmp(s_modes[i].mod, mod)) { *lo = s_modes[i].lo; *hi = s_modes[i].hi; return s_modes[i].bp; }
    if (s_nmodes) return false;
    for (size_t i = 0; i < sizeof BPF / sizeof BPF[0]; i++)
        if (!strcmp(mod, BPF[i].mod)) { *lo = BPF[i].lo; *hi = BPF[i].hi; return true; }
    return false;
}

/* mode d'un signet → démodulateur écoutable (appelant verrouillé) : mode
 * analogique du serveur tel quel, mode numérique (FT8, packet…) par son mode
 * sous-jacent, sinon une valeur raisonnable */
static void norm_mod(const char *in, char *out, size_t cap)
{
    for (int i = 0; i < s_nmodes; i++)
        if (!strcmp(in, s_modes[i].mod)) { strlcpy(out, in, cap); return; }
    for (int i = 0; i < s_ndigi; i++)
        if (!strcmp(in, s_digi[i].mod) && s_digi[i].under[0]) { strlcpy(out, s_digi[i].under, cap); return; }
    if (!s_nmodes)
        for (size_t i = 0; i < sizeof BPF / sizeof BPF[0]; i++)
            if (!strcmp(in, BPF[i].mod)) { strlcpy(out, in, cap); return; }
    if (!strcmp(in, "packet") || !strcmp(in, "pocsag") || !strcmp(in, "ais")) strlcpy(out, "nfm", cap);
    else if (in[0]) strlcpy(out, "usb", cap);          /* ft8, wspr, js8, bpsk… */
    else strlcpy(out, "nfm", cap);
}

static void send_dsp(bool start)
{
    char b[256];
    lock();
    int64_t off = s_info.freq - s_info.center;
    char mod[16];
    strlcpy(mod, s_info.mod, sizeof mod);
    int sq = s_info.squelch;
    bool ok = s_info.freq && s_info.center;
    int lo = 0, hi = 0;
    bool bp = mode_bp(mod, &lo, &hi);
    unlock();
    if (!ok) return;
    int n = snprintf(b, sizeof b, "{\"type\":\"dspcontrol\",\"params\":{\"mod\":\"%s\",\"offset_freq\":%lld,"
                     "\"squelch_level\":%d", mod, (long long)off, sq);
    if (bp) n += snprintf(b + n, sizeof b - n, ",\"low_cut\":%d,\"high_cut\":%d", lo, hi);
    snprintf(b + n, sizeof b - n, "}%s}", start ? ",\"action\":\"start\"" : "");
    ws_text(b);
}

/* fréquence ramenée dans la bande reçue par le profil */
static int64_t clamp_band(int64_t f, int64_t center, int32_t rate)
{
    int64_t half = rate / 2 - 2000;
    if (half < 0) half = 0;
    if (f < center - half) f = center - half;
    if (f > center + half) f = center + half;
    return f;
}

/* -------------------------- messages texte -------------------------- */
static const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool jnum(cJSON *o, const char *k, double *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valuedouble;
    return true;
}

static int cmp_bm(const void *a, const void *b)
{
    int64_t x = ((const wrx_bm_t *)a)->freq, y = ((const wrx_bm_t *)b)->freq;
    return x < y ? -1 : x > y;
}

/* première écoute (ou nouveau profil) : fréquence mémorisée si elle est
 * dans la bande, sinon celle de départ du profil */
static void tune_start(bool first)
{
    s_have_cfg = true;
    s_prof_wait_us = 0;
    lock();
    int64_t lo = s_info.center - s_info.samp_rate / 2, hi = s_info.center + s_info.samp_rate / 2;
    int64_t f = s_info.freq;
    if (!f || f <= lo || f >= hi) {
        bool saved = first && s_st.freq > lo && s_st.freq < hi;
        f = saved ? s_st.freq : s_start_freq > lo && s_start_freq < hi ? s_start_freq : s_info.center;
        s_info.mod[0] = 0;
        if (saved && s_st.mod[0]) strlcpy(s_info.mod, s_st.mod, sizeof s_info.mod);
        else if (s_start_mod[0]) norm_mod(s_start_mod, s_info.mod, sizeof s_info.mod);
        s_info.station[0] = 0;
    }
    if (!s_info.mod[0]) strlcpy(s_info.mod, "nfm", sizeof s_info.mod);
    s_info.freq = clamp_band(f, s_info.center, s_info.samp_rate);
    s_info.state = WRX_LIVE;
    s_info.msg = NULL;
    s_info.detail[0] = 0;
    unlock();
    send_dsp(!s_started);
    s_started = true;
    s_audio_on = true;
}

static void on_config(cJSON *v)
{
    double d;
    const char *s;
    lock();
    int64_t old_center = s_info.center;
    if (jnum(v, "center_freq", &d)) s_info.center = (int64_t)d;
    if (jnum(v, "samp_rate", &d)) s_info.samp_rate = (int32_t)d;
    if (jnum(v, "fft_size", &d)) s_fft_size = (int)d;
    if ((s = jstr(v, "audio_compression"))) s_adpcm = !strcmp(s, "adpcm");
    if ((s = jstr(v, "fft_compression"))) s_fft_adpcm = !strcmp(s, "adpcm");
    if (jnum(v, "start_freq", &d)) s_start_freq = (int64_t)d;
    if ((s = jstr(v, "start_mod"))) strlcpy(s_start_mod, s, sizeof s_start_mod);
    const char *sdr = jstr(v, "sdr_id"), *prof = jstr(v, "profile_id");
    if (sdr && prof) snprintf(s_info.profile, sizeof s_info.profile, "%s|%s", sdr, prof);
    bool ready = s_info.center && s_info.samp_rate;
    bool moved = s_info.center != old_center || !s_info.freq;
    bool other = s_st.profile[0] && strcmp(s_st.profile, s_info.profile);
    unlock();
    if (!ready) return;
    if (!s_have_cfg) {
        /* profil mémorisé différent de l'actuel : on le redemande une fois
         * (le serveur renvoie alors une config) */
        if (other && !s_prof_tried) {
            char b[160];
            snprintf(b, sizeof b, "{\"type\":\"selectprofile\",\"params\":{\"profile\":\"%s\"}}", s_st.profile);
            s_prof_tried = true;
            s_prof_wait_us = esp_timer_get_time() + 10000000;
            ws_text(b);
            return;
        }
        if (other && s_prof_wait_us && esp_timer_get_time() < s_prof_wait_us) return;
        tune_start(true);
    } else if (moved) {
        tune_start(false);
    }
}

static void on_profiles(cJSON *v)
{
    if (!cJSON_IsArray(v)) return;
    lock();
    s_nprof = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, v) {
        if (s_nprof >= WRX_PROF_MAX) break;
        const char *id = jstr(it, "id"), *name = jstr(it, "name");
        if (!id) continue;
        strlcpy(s_prof[s_nprof].id, id, sizeof s_prof[0].id);
        strlcpy(s_prof[s_nprof].name, name ? name : id, sizeof s_prof[0].name);
        s_nprof++;
    }
    s_info.prof_ver++;
    unlock();
}

static void on_modes(cJSON *v)
{
    if (!cJSON_IsArray(v)) return;
    lock();
    s_nmodes = s_ndigi = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, v) {
        const char *mod = jstr(it, "modulation"), *name = jstr(it, "name"), *type = jstr(it, "type");
        if (!mod || !mod[0] || strlen(mod) >= sizeof s_modes[0].mod) continue;
        cJSON *bpo = cJSON_GetObjectItemCaseSensitive(it, "bandpass");
        double lo = 0, hi = 0;
        bool bp = cJSON_IsObject(bpo) && jnum(bpo, "low_cut", &lo) && jnum(bpo, "high_cut", &hi);
        if (type && !strcmp(type, "analog")) {
            if (s_nmodes >= WRX_MODE_MAX) continue;
            wrx_mode_t *m = &s_modes[s_nmodes++];
            strlcpy(m->mod, mod, sizeof m->mod);
            strlcpy(m->name, name ? name : mod, sizeof m->name);
            m->lo = (int)lo; m->hi = (int)hi; m->bp = bp;
        } else if (s_ndigi < DIGI_MAX) {
            cJSON *u = cJSON_GetObjectItemCaseSensitive(it, "underlying");
            cJSON *u0 = cJSON_IsArray(u) ? cJSON_GetArrayItem(u, 0) : NULL;
            strlcpy(s_digi[s_ndigi].mod, mod, sizeof s_digi[0].mod);
            strlcpy(s_digi[s_ndigi].under, cJSON_IsString(u0) ? u0->valuestring : "", sizeof s_digi[0].under);
            s_ndigi++;
        }
    }
    /* signets déjà reçus : leurs modes sont réinterprétés avec la vraie liste */
    s_info.mode_ver++;
    unlock();
}

static void on_bookmarks(cJSON *v)
{
    if (!cJSON_IsArray(v)) return;
    lock();
    s_nbm = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, v) {
        if (s_nbm >= WRX_BM_MAX) break;
        double f;
        const char *name = jstr(it, "name"), *mod = jstr(it, "modulation");
        if (!jnum(it, "frequency", &f)) continue;
        wrx_bm_t *b = &s_bm[s_nbm++];
        strlcpy(b->name, name ? name : "", sizeof b->name);
        b->freq = (int64_t)f;
        strlcpy(b->mod, mod ? mod : "", sizeof b->mod);   /* brut : traduit à l'écoute */
    }
    qsort(s_bm, s_nbm, sizeof *s_bm, cmp_bm);
    s_info.bm_ver++;
    unlock();
}

static void on_text(char *t, int n)
{
    t[n] = 0;
    if (!strncmp(t, "CLIENT DE SERVER", 16)) return;
    /* type lu sans tout analyser : les gros messages inutiles sont écartés */
    char *ty = strstr(t, "\"type\"");
    if (!ty || ty - t > 8) return;
    ty = strchr(ty + 6, '"');
    if (!ty) return;
    ty++;
    {
        size_t k = 0;
        while (k + 1 < sizeof s_last_type && ty[k] && ty[k] != '"') { s_last_type[k] = ty[k]; k++; }
        s_last_type[k] = 0;
    }
    if (!strncmp(ty, "smeter\"", 7)) {
        char *v = strstr(ty, "\"value\"");
        if (v && (v = strchr(v, ':'))) {
            float lin = strtof(v + 1, NULL);
            lock();
            s_info.level_db = lin > 0 ? 10.0f * log10f(lin) : -150.0f;
            unlock();
        }
        return;
    }
    static const char *const WANT[] = { "config\"", "profiles\"", "bookmarks\"", "modes\"", "sdr_error\"", "backoff\"",
                                        "metadata\"", "demodulator_error\"" };
    bool want = false;
    for (size_t i = 0; i < sizeof WANT / sizeof WANT[0]; i++) want |= !strncmp(ty, WANT[i], strlen(WANT[i]));
    if (!want) return;
    cJSON *j = cJSON_ParseWithLength(t, n);
    if (!j) return;
    const char *type = jstr(j, "type");
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "value");
    if (!type) { cJSON_Delete(j); return; }
    if (!strcmp(type, "config") && cJSON_IsObject(v)) on_config(v);
    else if (!strcmp(type, "profiles")) on_profiles(v);
    else if (!strcmp(type, "bookmarks")) on_bookmarks(v);
    else if (!strcmp(type, "modes")) on_modes(v);
    else if (!strcmp(type, "metadata") && cJSON_IsObject(v)) {
        const char *ps = jstr(v, "ps");              /* RDS : nom de la station */
        if (ps && ps[0]) {
            char t[16];
            strlcpy(t, ps, sizeof t);
            for (int i = (int)strlen(t) - 1; i >= 0 && t[i] == ' '; i--) t[i] = 0;
            lock(); strlcpy(s_info.station, t, sizeof s_info.station); unlock();
        }
    } else if (!strcmp(type, "backoff")) {
        /* refus du serveur : trop d'auditeurs, adresse bannie (anti-robot
         * d'OpenWebRX+)… le message dit pourquoi ; pas de reconnexion auto */
        const char *why = cJSON_IsString(v) ? v->valuestring : "";
        s_backoff = true;
        set_state(WRX_FAIL, N_("Refusé par le serveur"), why);
        ble_diag("WRX refus : %.34s", why);
    } else if (!strcmp(type, "sdr_error") || !strcmp(type, "demodulator_error")) {
        lock();
        strlcpy(s_info.detail, cJSON_IsString(v) ? v->valuestring : "", sizeof s_info.detail);
        s_info.msg = N_("Erreur du récepteur");
        unlock();
    }
    cJSON_Delete(j);
}

/* -------------------------- FFT → waterfall ------------------------- */
static void on_fft(const uint8_t *p, int n)
{
    if (!s_fft_on || s_fft_size <= 0) return;
    static float *db;
    static int dbn;
    int cnt = 0;
    if (!db || dbn < s_fft_size) {
        free(db);
        db = ps_malloc(s_fft_size * sizeof(float));
        dbn = db ? s_fft_size : 0;
        if (!db) return;
    }
    if (s_fft_adpcm) {
        adpcm_t c = { 0 };
        for (int i = 0; i < n && cnt < s_fft_size; i++) {
            for (int h = 0; h < 2 && cnt < s_fft_size; h++) {
                int16_t s = nib(&c, h ? p[i] >> 4 : p[i] & 0x0F);
                int k = 2 * i + h - 10;                   /* 10 échantillons de bourrage */
                if (k >= 0) db[cnt++] = s / 100.0f;
            }
        }
    } else {
        for (int i = 0; i + 3 < n && cnt < s_fft_size; i += 4) {
            float f;
            memcpy(&f, p + i, 4);
            db[cnt++] = f;
        }
    }
    if (cnt < s_fft_size / 2) return;
    lock();
    int64_t center = s_info.center, freq = s_info.freq;
    int32_t rate = s_info.samp_rate;
    unlock();
    if (!rate) return;
    int z = s_zoom;
    int32_t span = rate / (z > 0 ? z : 1);
    int64_t band_lo = center - rate / 2;
    int64_t lo = z <= 1 ? band_lo : freq - span / 2;
    if (lo < band_lo) lo = band_lo;
    if (lo + span > band_lo + rate) lo = band_lo + rate - span;
    wrx_row_t *r = &s_rows[s_row_w % NROWS];
    double bin_hz = (double)rate / cnt;
    for (int x = 0; x < WRX_FFT_W; x++) {
        double f0 = (double)(lo - band_lo) + (double)span * x / WRX_FFT_W;
        double f1 = (double)(lo - band_lo) + (double)span * (x + 1) / WRX_FFT_W;
        int b0 = (int)(f0 / bin_hz), b1 = (int)(f1 / bin_hz);
        if (b1 <= b0) b1 = b0 + 1;
        float m = -200.0f;
        for (int b = b0; b < b1 && b < cnt; b++) if (b >= 0 && db[b] > m) m = db[b];
        r->db[x] = m;
    }
    r->lo = lo;
    r->span = span;
    lock();
    s_row_w++;
    if (s_row_w - s_row_r > NROWS) s_row_r = s_row_w - NROWS;
    unlock();
}

/* ------------------------- boucle de réception ------------------------ */
static void handle_pending(void)
{
    lock();
    uint32_t p = s_pend;
    s_pend = 0;
    char prof[72];
    strlcpy(prof, s_pend_prof, sizeof prof);
    unlock();
    if (p & PEND_PROFILE) {
        char b[160];
        snprintf(b, sizeof b, "{\"type\":\"selectprofile\",\"params\":{\"profile\":\"%s\"}}", prof);
        ws_text(b);
    }
    if ((p & PEND_DSP) && s_have_cfg) send_dsp(false);
}

static volatile int s_switch = -1;          /* serveur demandé (webrx_server), -1 = aucun */
static const char *s_lost;                   /* cause de la dernière coupure (N_) */
static uint32_t s_frames, s_bytes;

/* true : fin normale (arrêt demandé) ; false : connexion perdue (s_lost) */
static bool session(void)
{
    uint8_t *rb = s_rb;
    int left = 0, opcode = 0;                    /* trame en cours */
    bool binary_type_read = false;
    int btype = 0;
    int64_t last_rx = esp_timer_get_time();
    memset(&s_dec, 0, sizeof s_dec);
    s_have_cfg = s_started = s_prof_tried = false;
    s_prof_wait_us = 0;
    s_start_freq = 0;
    s_start_mod[0] = 0;
    lock();
    s_info.center = 0;
    s_info.samp_rate = 0;
    unlock();
    while (!s_quit) {
        if (xEventGroupGetBits(s_evg) & BIT_DISC) { s_lost = N_("Wi-Fi perdu"); return false; }
        if (s_switch >= 0) return false;            /* changement de serveur */
        handle_pending();
        if (!s_have_cfg && s_prof_wait_us && esp_timer_get_time() > s_prof_wait_us) {
            lock(); bool ok = s_info.center && s_info.samp_rate; unlock();
            if (ok) tune_start(true);              /* profil demandé jamais venu : on écoute l'actuel */
        }
        /* délai large : avec le modem sleep et les petits paquets du serveur
         * (1 Ko) d'un serveur parfois lent, un en-tête WebSocket peut arriver
         * en deux fois à plusieurs secondes d'écart ; esp_transport_ws traite
         * un en-tête ou une trame incomplets à l'échéance comme une erreur
         * fatale */
        int n = esp_transport_read(s_ws, (char *)rb, RB_LEN, 8000);
        if (n < 0) {
            ESP_LOGW(TAG, "lecture : %d", n);
            esp_tls_error_handle_t eh = s_par ? esp_transport_get_error_handle(s_par) : NULL;
            ble_diag("WRX lect %d errno %d tls %x/%x", n, s_par ? esp_transport_get_errno(s_par) : 0,
                     eh ? (unsigned)eh->last_error : 0u, eh ? (unsigned)-eh->esp_tls_error_code : 0u);
            s_lost = left ? N_("trame coupée") : N_("lecture impossible");
            return false;
        }
        if (n == 0) {
            if (esp_timer_get_time() - last_rx > 30LL * 1000000) {      /* serveur muet */
                ESP_LOGW(TAG, "plus rien du serveur");
                s_lost = N_("plus rien du serveur");
                return false;
            }
            continue;
        }
        last_rx = esp_timer_get_time();
        s_bytes += n;
        if (left == 0) {                         /* début de trame */
            opcode = esp_transport_ws_get_read_opcode(s_ws) & 0x0F;
            left = esp_transport_ws_get_read_payload_len(s_ws);
            binary_type_read = false;
            if (opcode == WS_TRANSPORT_OPCODES_CLOSE) { s_lost = N_("fermée par le serveur"); return false; }
            s_frames++;
            s_txt_n = 0;
            s_bin_n = 0;
        }
        left -= n;
        if (left < 0) left = 0;
        if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
            if (s_txt_n >= 0 && s_txt_n + n < TXT_MAX) { memcpy(s_txt + s_txt_n, rb, n); s_txt_n += n; }
            else s_txt_n = -1;                    /* trop long : ignoré */
            if (left == 0 && s_txt_n > 0) on_text(s_txt, s_txt_n);
        } else if (opcode == WS_TRANSPORT_OPCODES_BINARY) {
            const uint8_t *p = rb;
            int m = n;
            if (!binary_type_read) { btype = p[0]; p++; m--; binary_type_read = true; }
            if (btype == 2 || btype == 4) {
                if (s_audio_on) audio_bytes(p, m);
            } else if (btype == 1 && s_fft_on) {
                if (s_bin_n >= 0 && s_bin_n + m <= BIN_MAX) { memcpy(s_bin + s_bin_n, p, m); s_bin_n += m; }
                else s_bin_n = -1;
                if (left == 0 && s_bin_n > 0) on_fft(s_bin, s_bin_n);
            }
        }
    }
    return true;
}

/* changement de serveur : état du serveur quitté retenu, celui du nouveau
 * chargé, listes vidées */
static bool apply_switch(void)
{
    int to = s_switch;
    if (to < 0) return false;
    s_switch = -1;
    lock();
    if (s_info.freq) s_st.freq = s_info.freq;
    if (s_info.mod[0]) strlcpy(s_st.mod, s_info.mod, sizeof s_st.mod);
    s_st.squelch = s_info.squelch;
    s_st.tone = s_info.tone;
    unlock();
    wrx_state_save(&s_st);
    uint8_t tone = s_st.tone;
    wrx_state_load_srv(&s_st, to);
    s_st.tone = tone;
    wrx_state_save(&s_st);                       /* mémorise le serveur choisi */
    lock();
    s_info.server = (uint8_t)to;
    wrx_srv_label(&s_cfg, to, s_info.server_name, sizeof s_info.server_name);
    s_info.center = 0;
    s_info.samp_rate = 0;
    s_info.freq = 0;
    s_info.mod[0] = 0;
    s_info.profile[0] = 0;
    s_info.station[0] = 0;
    s_info.squelch = s_st.squelch;
    s_info.level_db = -200.0f;
    s_nbm = s_nprof = s_nmodes = s_ndigi = 0;
    s_info.bm_ver++;
    s_info.prof_ver++;
    s_info.mode_ver++;
    unlock();
    return true;
}

static void net_task(void *arg)
{
    int fails = 0, drops = 0;
    power_set_cpu_max(160);
    esp_wifi_set_mode(WIFI_MODE_STA);
    /* pas d'économie d'énergie du modem : le serveur envoie en continu
     * (audio + FFT) et, s'il ne peut plus écrire pendant 10 s parce que la
     * montre ne reçoit pas assez vite, il coupe la connexion */
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (esp_wifi_start() != ESP_OK) set_state(WRX_FAIL, N_("Erreur Wi-Fi"), "");
    else while (!s_quit) {
        if (apply_switch()) { fails = drops = 0; }
        bool ok = false;
        if (!(xEventGroupGetBits(s_evg) & BIT_IP)) ok = wifi_connect();
        else ok = true;
        if (ok && !s_quit && server_connect()) {
            fails = 0;
            s_lost = NULL;
            s_backoff = false;
            s_last_type[0] = 0;
            s_frames = s_bytes = 0;
            int64_t t0 = esp_timer_get_time();
            bool done = session();
            long dur = (long)((esp_timer_get_time() - t0) / 1000000);
            if (!done && s_switch < 0) {
                /* lignes courtes : le journal garde 47 caractères par ligne */
                wifi_ap_record_t ap = { 0 };
                esp_wifi_sta_get_ap_info(&ap);
                ble_diag("WRX coupé : %s", s_lost ? s_lost : "?");
                ble_diag("WRX %lu tr %lu o %lds RSSI %d", (unsigned long)s_frames, (unsigned long)s_bytes, dur, ap.rssi);
                ble_diag("WRX dernier : %s, int %u Ko", s_last_type[0] ? s_last_type : "-",
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
            }
            s_audio_on = false;
            server_close();
            if (done) break;
            if (s_quit) break;
            if (s_switch >= 0) continue;           /* autre serveur : tout de suite */
            if (s_backoff) {                       /* refusé : on attend le bouton */
                xEventGroupWaitBits(s_evg, BIT_RETRY | BIT_QUIT, pdTRUE, pdFALSE, portMAX_DELAY);
                continue;
            }
            /* reprises espacées : OpenWebRX+ bannit 12 h une adresse qui se
             * reconnecte trop souvent (« robot ») ; après 4 coupures rapides
             * de suite, on attend le bouton */
            drops = dur >= 60 ? 1 : drops + 1;
            if (drops > 4) {
                set_state(WRX_FAIL, N_("Connexion perdue"), s_lost ? _(s_lost) : "");
                xEventGroupWaitBits(s_evg, BIT_RETRY | BIT_QUIT, pdTRUE, pdFALSE, portMAX_DELAY);
                drops = 0;
                continue;
            }
            int wait = drops <= 1 ? 2 : 10 << (drops - 2);           /* 2, 10, 20, 40 s */
            for (int t = wait; t > 0 && !s_quit; t--) {
                set_state(WRX_SERVER, N_("Connexion perdue, reprise…"), s_lost ? _(s_lost) : "");
                lock(); s_info.retry_s = t; unlock();
                if (xEventGroupWaitBits(s_evg, BIT_RETRY | BIT_QUIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000)) & BIT_RETRY) break;
            }
            lock(); s_info.retry_s = 0; unlock();
            continue;
        }
        s_audio_on = false;
        if (s_quit) break;
        /* échec : nouvel essai après 5, 10, 20, 40 s ; puis on attend le bouton */
        fails++;
        int wait = fails <= 4 ? 5 << (fails - 1) : 0;
        if (s_info.state != WRX_FAIL) set_state(WRX_FAIL, N_("Connexion impossible"), "");
        if (!wait) {
            lock(); s_info.retry_s = 0; unlock();
            xEventGroupWaitBits(s_evg, BIT_RETRY | BIT_QUIT, pdTRUE, pdFALSE, portMAX_DELAY);
            fails = 0;
            continue;
        }
        for (int t = wait; t > 0 && !s_quit; t--) {
            lock(); s_info.retry_s = t; unlock();
            if (xEventGroupWaitBits(s_evg, BIT_RETRY | BIT_QUIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000)) & BIT_RETRY) break;
        }
        lock(); s_info.retry_s = 0; unlock();
    }
    s_audio_on = false;
    server_close();
    esp_wifi_disconnect();
    esp_wifi_stop();
    xEventGroupSetBits(s_evg, BIT_NETEND);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */
static void *json_malloc(size_t n) { return ps_malloc(n); }

static bool alloc_buffers(void)
{
    if (!s_ring) s_ring = ps_malloc(RING * sizeof(int16_t));
    if (!s_txt) s_txt = ps_malloc(TXT_MAX + 1);
    if (!s_bin) s_bin = ps_malloc(BIN_MAX);
    if (!s_rows) s_rows = ps_malloc(NROWS * sizeof(wrx_row_t));
    if (!s_bm) s_bm = ps_malloc(WRX_BM_MAX * sizeof(wrx_bm_t));
    if (!s_prof) s_prof = ps_malloc(WRX_PROF_MAX * sizeof(wrx_prof_t));
    if (!s_cfg_p) s_cfg_p = ps_malloc(sizeof(wrx_cfg_t));
    if (!s_modes) s_modes = ps_malloc(WRX_MODE_MAX * sizeof(wrx_mode_t));
    if (!s_digi) s_digi = ps_malloc(DIGI_MAX * sizeof *s_digi);
    if (!s_pcm) s_pcm = ps_malloc(2 * 1024 * sizeof(int16_t));
    if (!s_rb) s_rb = ps_malloc(RB_LEN);
    return s_cfg_p && s_modes && s_digi && s_ring && s_txt && s_bin && s_rows && s_bm && s_prof && s_pcm && s_rb;
}

esp_err_t webrx_start(void)
{
    if (s_run) return ESP_OK;
    if (!s_mx) {
        s_mx = xSemaphoreCreateMutex();
        s_evg = xEventGroupCreate();
        static cJSON_Hooks h = { .malloc_fn = json_malloc, .free_fn = free };
        cJSON_InitHooks(&h);                    /* listes JSON en PSRAM */
    }
    if (!s_mx || !s_evg || !alloc_buffers()) return ESP_ERR_NO_MEM;
    wrx_cfg_load(&s_cfg);
    wrx_state_load(&s_st);
    lock();
    memset(&s_info, 0, sizeof s_info);
    s_info.level_db = -200.0f;
    s_info.volume = WRX_VOL_START;           /* chaque écoute démarre bas */
    s_info.tone = s_st.tone;
    s_info.nservers = (uint8_t)wrx_cfg_servers(&s_cfg);
    if (s_st.server >= s_info.nservers) s_st.server = 0;
    s_info.server = s_st.server;
    wrx_srv_label(&s_cfg, s_st.server, s_info.server_name, sizeof s_info.server_name);
    s_info.squelch = s_st.squelch;
    s_nbm = s_nprof = s_nmodes = s_ndigi = 0;
    s_pend = 0;
    s_row_w = s_row_r = 0;
    unlock();
    s_wr = s_rd = 0;
    s_quit = false;
    xEventGroupClearBits(s_evg, 0xFF);

    if (net_base_init() != ESP_OK || !net_sta_netif()) return ESP_FAIL;
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    wc.nvs_enable = 0;
    esp_err_t e = esp_wifi_init(&wc);
    if (e != ESP_OK && ble_running()) {
        /* pas assez de RAM interne à côté du Bluetooth : suspendu le temps
         * de l'écoute */
        ESP_LOGW(TAG, "esp_wifi_init: %s, Bluetooth suspendu", esp_err_to_name(e));
        ble_diag("WebRX : Bluetooth suspendu (RAM)");
        ble_stop();
        if (!ble_running()) {
            s_ble_paused = true;
            esp_wifi_deinit();
            e = esp_wifi_init(&wc);
        }
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_init: %s (interne libre %u, bloc %u)", esp_err_to_name(e),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (s_ble_paused) { s_ble_paused = false; ble_start(); }
        return e;
    }
    s_wifi_inited = true;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_net_event, NULL, &s_h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL, &s_h_ip);
    s_run = true;
    set_state(WRX_WIFI, N_("Recherche du Wi-Fi…"), "");
    /* pile assez grande pour la poignée de main TLS (mbedTLS en PSRAM) */
    if (xTaskCreatePinnedToCore(net_task, "wrx", 7168, NULL, 5, NULL, 0) != pdPASS) {
        s_run = false;
        webrx_stop();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(play_task, "wrxplay", 3584, NULL, 6, NULL, 1) != pdPASS) {
        xEventGroupSetBits(s_evg, BIT_PLAYEND);
    }
    return ESP_OK;
}

void webrx_stop(void)
{
    if (!s_mx) return;
    if (s_run) {
        s_quit = true;
        s_audio_on = false;
        xEventGroupSetBits(s_evg, BIT_QUIT);
        /* débloque une lecture/écriture réseau en cours */
        lock();
        if (s_sock >= 0) shutdown(s_sock, SHUT_RDWR);
        unlock();
        EventBits_t b = xEventGroupWaitBits(s_evg, BIT_NETEND | BIT_PLAYEND, pdFALSE, pdTRUE, pdMS_TO_TICKS(12000));
        if ((b & (BIT_NETEND | BIT_PLAYEND)) != (BIT_NETEND | BIT_PLAYEND))
            ESP_LOGE(TAG, "arrêt incomplet (0x%x)", (unsigned)b);
        s_run = false;
        /* état durable */
        lock();
        /* le profil n'est retenu que s'il a été choisi sur la montre : un
         * serveur partagé n'est pas rebasculé sur un profil vu en passant */
        if (s_info.freq) s_st.freq = s_info.freq;
        if (s_info.mod[0]) strlcpy(s_st.mod, s_info.mod, sizeof s_st.mod);
        s_st.squelch = s_info.squelch;
        s_st.tone = s_info.tone;
        s_info.state = WRX_OFF;
        s_info.msg = NULL;
        unlock();
        wrx_state_save(&s_st);
    }
    if (s_wifi_inited) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_h_wifi);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
        esp_wifi_deinit();
        s_wifi_inited = false;
    }
    power_set_cpu_max(0);
    if (s_ble_paused) {
        s_ble_paused = false;
        ble_start();
    }
}

bool webrx_running(void) { return s_run; }
bool webrx_ble_paused(void) { return s_ble_paused; }

void webrx_retry(void)
{
    if (s_run) xEventGroupSetBits(s_evg, BIT_RETRY);
}

void webrx_info(wrx_info_t *out)
{
    if (!s_mx) { memset(out, 0, sizeof *out); out->volume = WRX_VOL_START; out->squelch = -150; out->tone = WRX_TONE_DEF; return; }
    lock();
    *out = s_info;
    if (!s_run) {
        if (!out->freq) out->freq = s_st.freq;
        if (!out->mod[0]) strlcpy(out->mod, s_st.mod, sizeof out->mod);
        out->volume = WRX_VOL_START;
        out->squelch = s_st.squelch;
        out->tone = s_st.tone;
    }
    unlock();
}

int webrx_bookmarks(wrx_bm_t *out, int max)
{
    if (!s_mx) return 0;
    lock();
    int n = s_nbm < max ? s_nbm : max;
    memcpy(out, s_bm, n * sizeof *out);
    unlock();
    return n;
}

int webrx_modes(wrx_mode_t *out, int max)
{
    if (!s_mx) return 0;
    lock();
    int n = s_nmodes < max ? s_nmodes : max;
    memcpy(out, s_modes, n * sizeof *out);
    unlock();
    return n;
}

int webrx_profiles(wrx_prof_t *out, int max)
{
    if (!s_mx) return 0;
    lock();
    int n = s_nprof < max ? s_nprof : max;
    memcpy(out, s_prof, n * sizeof *out);
    unlock();
    return n;
}

void webrx_tune(int64_t hz)
{
    if (!s_mx) return;
    lock();
    if (s_info.samp_rate) {
        s_info.freq = clamp_band(hz, s_info.center, s_info.samp_rate);
        s_info.station[0] = 0;
        s_pend |= PEND_DSP;
    }
    unlock();
}

void webrx_set_mod(const char *mod)
{
    if (!s_mx) return;
    lock();
    strlcpy(s_info.mod, mod, sizeof s_info.mod);
    s_pend |= PEND_DSP;
    unlock();
}

void webrx_select(const wrx_bm_t *bm)
{
    if (!s_mx) return;
    lock();
    if (s_info.samp_rate) {
        s_info.freq = clamp_band(bm->freq, s_info.center, s_info.samp_rate);
        norm_mod(bm->mod, s_info.mod, sizeof s_info.mod);
        strlcpy(s_info.station, bm->name, sizeof s_info.station);
        s_pend |= PEND_DSP;
    }
    unlock();
}

void webrx_profile(const char *id)
{
    if (!s_mx) return;
    lock();
    strlcpy(s_pend_prof, id, sizeof s_pend_prof);
    strlcpy(s_st.profile, id, sizeof s_st.profile);
    s_info.freq = 0;                           /* fréquence de départ du nouveau profil */
    s_info.mod[0] = 0;
    s_st.freq = 0;
    s_st.mod[0] = 0;
    s_info.station[0] = 0;
    s_nbm = 0;
    s_info.bm_ver++;
    s_pend |= PEND_PROFILE;
    unlock();
}

void webrx_volume(int pct)
{
    if (!s_mx) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lock();
    s_info.volume = (uint8_t)pct;
    unlock();
}

void webrx_tone(int level)
{
    if (!s_mx || level < 0 || level >= WRX_TONES) return;
    lock();
    s_info.tone = (uint8_t)level;
    s_st.tone = (uint8_t)level;
    unlock();
}

void webrx_squelch(int db)
{
    if (!s_mx) return;
    lock();
    s_info.squelch = (int16_t)db;
    s_st.squelch = (int16_t)db;
    s_pend |= PEND_DSP;
    unlock();
}

void webrx_fft_enable(bool on, int zoom)
{
    s_zoom = zoom < 1 ? 1 : zoom;
    s_fft_on = on;
}

bool webrx_fft_pop(wrx_row_t *out)
{
    if (!s_mx || !s_rows) return false;
    lock();
    bool ok = s_row_r != s_row_w;
    if (ok) {
        /* la plus récente seulement : l'affichage suit le rythme de l'écran */
        s_row_r = s_row_w;
        *out = s_rows[(s_row_w - 1) % NROWS];
    }
    unlock();
    return ok;
}

void webrx_server(int idx)
{
    if (idx < 0 || idx >= WRX_SRV) return;
    if (s_mx && s_run) {
        if (idx == s_info.server || idx >= s_info.nservers) return;
        s_switch = idx;
        xEventGroupSetBits(s_evg, BIT_RETRY);     /* sort d'une attente */
        return;
    }
    /* arrêté : retenu pour la prochaine écoute */
    wrx_state_t cur, st;
    wrx_state_load(&cur);
    wrx_state_load_srv(&st, idx);
    st.tone = cur.tone;
    wrx_state_save(&st);
    if (s_mx) {
        lock();
        s_info.server = (uint8_t)idx;
        s_info.freq = 0;
        s_info.mod[0] = 0;
        unlock();
        s_st = st;
    }
}
