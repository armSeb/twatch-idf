/*
 * radio.c — voir radio.h.
 *
 * Flux : esp_http_client (TLS avec le paquet de racines d'ESP-IDF, redirections,
 * « Icy-MetaData: 1 » pour les titres), listes .m3u/.pls suivies (3 niveaux),
 * HLS refusé. Le codec vient du Content-Type, sinon des premiers octets.
 * Décodage Helix (MP3 ; AAC-LC et HE-AAC/SBR en ADTS), sortie mono.
 */
#include "radio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_pm.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mp3dec.h"
#include "aacdec.h"
#include "i18n.h"
#include "ble.h"
#include "power.h"
#include "sound.h"
#include "wifi_sta.h"
#include "audio_fx.h"

static const char *TAG = "radio";

#define RING      (128 * 1024u)       /* tampon compressé, puissance de 2 */
#define INBUF     4096                /* fenêtre d'entrée du décodeur */
#define PCM_MAX   (2 * 2048)          /* HE-AAC stéréo : 2 × 2048 échantillons */
#define PLS_MAX   8192
#define META_MAX  512

enum { C_NONE = 0, C_MP3, C_AAC };

#define BIT_QUIT    BIT0
#define BIT_RETRY   BIT1
#define BIT_NETEND  BIT2
#define BIT_PLAYEND BIT3

static SemaphoreHandle_t s_mx;
static EventGroupHandle_t s_evg;
static volatile bool s_run, s_quit;
static rad_info_t s_info;
static rad_station_t *s_st;           /* stations (PSRAM) */
static int s_nst;
static rad_state_t s_state;
static wrx_cfg_t *s_cfg;              /* réseaux Wi-Fi (PSRAM) */
static volatile int s_switch = -1;
static volatile bool s_screen = true;

/* tampon compressé */
static uint8_t *s_ring;
static volatile uint32_t s_wr, s_rd;
static volatile uint32_t s_gen;       /* change à chaque nouveau flux */
static volatile int s_codec;          /* codec du flux en cours */
static volatile bool s_streaming;     /* le flux arrive */
static volatile uint32_t s_prebuf = 32768;

static esp_pm_lock_handle_t s_boost_lock;

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

static uint32_t ring_used(void) { return s_wr - s_rd; }

/* ------------------------------------------------------------------ */
/* Décodage et lecture                                                  */
/* ------------------------------------------------------------------ */
static void boost(bool on)
{
    if (!s_boost_lock || on == s_info.boost) return;
    if (on) esp_pm_lock_acquire(s_boost_lock);
    else esp_pm_lock_release(s_boost_lock);
    lock(); s_info.boost = on; unlock();
}

static void play_task(void *arg)
{
    uint8_t *in = heap_caps_malloc(INBUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(PCM_MAX * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!in) in = ps_malloc(INBUF);
    if (!pcm) pcm = ps_malloc(PCM_MAX * sizeof(int16_t));
    HMP3Decoder mp3 = NULL;
    HAACDecoder aac = NULL;
    int codec = C_NONE, in_len = 0, rate = 0, tone = -1, errors = 0;
    uint32_t gen = ~0u;
    bool buffering = true, open = false;
    float load = 0;
    tone_fx_t fx;
    memset(&fx, 0, sizeof fx);
    int64_t last_info = 0;

    while (!s_quit && in && pcm) {
        /* nouveau flux : décodeur remis à neuf */
        if (gen != s_gen) {
            gen = s_gen;
            in_len = 0;
            buffering = true;
            errors = 0;
            if (codec != s_codec) {
                if (mp3) { MP3FreeDecoder(mp3); mp3 = NULL; }
                if (aac) { AACFreeDecoder(aac); aac = NULL; }
                codec = s_codec;
                if (codec == C_MP3) mp3 = MP3InitDecoder();
                if (codec == C_AAC) aac = AACInitDecoder();
                if ((codec == C_MP3 && !mp3) || (codec == C_AAC && !aac)) {
                    ESP_LOGE(TAG, "décodeur : mémoire insuffisante");
                    codec = C_NONE;
                }
            } else if (aac) {
                AACFlushCodec(aac);
            }
        }
        if (codec == C_NONE || !s_streaming) {
            if (open) { sound_stream_close(); open = false; rate = 0; }
            boost(false);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (buffering) {
            if (ring_used() >= s_prebuf) {
                buffering = false;
                lock();
                if (s_info.state == RAD_BUFFER) { s_info.state = RAD_PLAY; s_info.msg = NULL; s_info.detail[0] = 0; }
                unlock();
            } else {
                lock();
                if (s_info.state == RAD_PLAY || s_info.state == RAD_CONNECT) {
                    s_info.state = RAD_BUFFER;
                    s_info.msg = N_("Mise en mémoire…");
                }
                s_info.buf_pct = (int)(ring_used() * 100 / RING);
                s_info.fill_pct = s_prebuf ? (int)(ring_used() * 100 / s_prebuf) : 0;
                unlock();
                if (open) sound_stream_write(NULL, 480, 0);       /* silence : garde le rythme */
                else vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
        }
        /* fenêtre d'entrée */
        if (in_len < INBUF) {
            uint32_t avail = ring_used(), want = INBUF - in_len;
            uint32_t n = avail < want ? avail : want;
            for (uint32_t i = 0; i < n; i++) in[in_len + i] = s_ring[(s_rd + i) & (RING - 1)];
            s_rd += n;
            in_len += n;
        }
        if (in_len < 1024 && ring_used() == 0) {      /* plus rien : on remet en mémoire */
            buffering = true;
            continue;
        }
        int off = codec == C_MP3 ? MP3FindSyncWord(in, in_len) : AACFindSyncWord(in, in_len);
        if (off < 0) {                                /* rien d'utile : on garde la fin */
            memmove(in, in + in_len - 3, 3);
            in_len = 3;
            continue;
        }
        if (off > 0) { memmove(in, in + off, in_len - off); in_len -= off; }
        unsigned char *p = in;
        int left = in_len;
        int64_t t0 = esp_timer_get_time();
        int err = codec == C_MP3 ? MP3Decode(mp3, &p, &left, pcm, 0) : AACDecode(aac, &p, &left, pcm);
        int64_t dt = esp_timer_get_time() - t0;
        int used = in_len - left;
        if (err) {
            if ((codec == C_MP3 && err == ERR_MP3_INDATA_UNDERFLOW) || (codec == C_AAC && err == ERR_AAC_INDATA_UNDERFLOW)) {
                if (ring_used() == 0) buffering = true;
                else if (in_len >= INBUF) { memmove(in, in + 1, in_len - 1); in_len--; }   /* trame impossible */
                continue;
            }
            if (codec == C_MP3 && err == ERR_MP3_MAINDATA_UNDERFLOW) {   /* début de flux : normal */
                memmove(in, in + used, in_len - used);
                in_len -= used;
                continue;
            }
            /* trame abîmée : on avance d'un octet et on se recale */
            if (++errors % 50 == 1) ESP_LOGW(TAG, "décodage : erreur %d", err);
            int skip = used > 0 ? used : 1;
            memmove(in, in + skip, in_len - skip);
            in_len -= skip;
            continue;
        }
        memmove(in, in + used, in_len - used);
        in_len -= used;

        int ch = 1, sr = 0, ns = 0, kbps = 0;
        const char *cname = "MP3";
        if (codec == C_MP3) {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(mp3, &fi);
            ch = fi.nChans; sr = fi.samprate; ns = fi.outputSamps; kbps = fi.bitrate / 1000;
        } else {
            AACFrameInfo fi;
            AACGetLastFrameInfo(aac, &fi);
            ch = fi.nChans; sr = fi.sampRateOut; ns = fi.outputSamps; kbps = fi.bitRate / 1000;
            cname = fi.sampRateOut != fi.sampRateCore ? "HE-AAC" : "AAC";
        }
        if (ch < 1 || ch > 2 || sr < 8000 || ns <= 0) continue;
        int n = ns / ch;
        if (ch == 2) for (int i = 0; i < n; i++) pcm[i] = (int16_t)(((int)pcm[2 * i] + pcm[2 * i + 1]) / 2);

        /* charge du décodage → CPU relevé si l'on ne suit plus */
        float l = (float)dt / (1e6f * n / sr);
        load = load * 0.95f + l * 0.05f;
        if (load > 0.6f) boost(true);
        else if (load < 0.25f) boost(false);

        if (sr != rate) {
            if (open) sound_stream_close();
            open = sound_stream_open(sr) == ESP_OK;
            rate = sr;
            tone = -1;
        }
        if (s_info.tone != tone) { tone = s_info.tone; tone_fx_init(&fx, tone, sr); }
        tone_fx_run(&fx, pcm, n);
        int v = s_info.volume;
        int gain = v * v * 8192 / 10000;              /* courbe douce, ×2 à 100 % */
        if (open) sound_stream_write(pcm, n, gain);
        else vTaskDelay(pdMS_TO_TICKS(1000 * n / sr));

        int64_t now = esp_timer_get_time();
        if (now - last_info > 500000) {
            last_info = now;
            lock();
            s_info.rate = sr;
            s_info.chans = ch;
            if (kbps > 0) s_info.kbps = kbps;
            strlcpy(s_info.codec, cname, sizeof s_info.codec);
            s_info.buf_pct = (int)(ring_used() * 100 / RING);
            s_info.load = (int)(load * 100);
            unlock();
        }
    }
    if (open) sound_stream_close();
    boost(false);
    if (mp3) MP3FreeDecoder(mp3);
    if (aac) AACFreeDecoder(aac);
    free(in);
    free(pcm);
    xEventGroupSetBits(s_evg, BIT_PLAYEND);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Flux HTTP                                                            */
/* ------------------------------------------------------------------ */
static char s_ctype[64], s_icy_name[40];
static int s_metaint, s_icy_br;

static esp_err_t http_ev(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_HEADER || !e->header_key || !e->header_value) return ESP_OK;
    if (!strcasecmp(e->header_key, "Content-Type")) strlcpy(s_ctype, e->header_value, sizeof s_ctype);
    else if (!strcasecmp(e->header_key, "icy-metaint")) s_metaint = atoi(e->header_value);
    else if (!strcasecmp(e->header_key, "icy-name")) strlcpy(s_icy_name, e->header_value, sizeof s_icy_name);
    else if (!strcasecmp(e->header_key, "icy-br")) s_icy_br = atoi(e->header_value);
    return ESP_OK;
}

static bool ctype_has(const char *s) { return strcasestr(s_ctype, s) != NULL; }

static bool url_ends(const char *url, const char *ext)
{
    size_t l = strcspn(url, "?#"), e = strlen(ext);
    return l >= e && !strncasecmp(url + l - e, ext, e);
}

/* première adresse d'une liste .m3u / .pls ; false si aucune (ou HLS) */
static bool playlist_pick(char *txt, char *out, size_t cap, bool *hls)
{
    *hls = strstr(txt, "#EXT-X-") != NULL;
    if (*hls) return false;
    for (char *line = strtok(txt, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') line++;
        char *u = line;
        if (!strncasecmp(line, "File", 4)) {                /* .pls : FileN=… */
            char *eq = strchr(line, '=');
            if (!eq) continue;
            u = eq + 1;
        }
        if (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) {
            size_t l = strcspn(u, " \t");
            if (l >= cap) l = cap - 1;
            memcpy(out, u, l);
            out[l] = 0;
            return true;
        }
    }
    return false;
}

/* titre ICY : texte brut, converti en UTF-8 s'il est en Latin-1 */
static bool utf8_ok(const unsigned char *s)
{
    while (*s) {
        if (*s < 0x80) { s++; continue; }
        int n = (*s & 0xE0) == 0xC0 ? 1 : (*s & 0xF0) == 0xE0 ? 2 : (*s & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return false;
        for (int i = 1; i <= n; i++) if ((s[i] & 0xC0) != 0x80) return false;
        s += n + 1;
    }
    return true;
}

static void on_meta(char *m)
{
    char *t = strstr(m, "StreamTitle='");
    if (!t) return;
    t += 13;
    char *end = strstr(t, "';");
    if (!end) end = t + strlen(t);
    *end = 0;
    char out[sizeof s_info.title];
    if (utf8_ok((unsigned char *)t)) {
        strlcpy(out, t, sizeof out);
    } else {
        size_t o = 0;
        for (unsigned char *c = (unsigned char *)t; *c && o + 3 < sizeof out; c++) {
            if (*c < 0x80) out[o++] = (char)*c;
            else { out[o++] = (char)(0xC0 | (*c >> 6)); out[o++] = (char)(0x80 | (*c & 0x3F)); }
        }
        out[o] = 0;
    }
    lock();
    strlcpy(s_info.title, out, sizeof s_info.title);
    unlock();
}

static bool ring_put(const uint8_t *p, int n)
{
    while (n > 0) {
        if (s_quit || s_switch >= 0) return false;
        uint32_t free_ = RING - ring_used();
        if (free_ == 0) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }   /* plein : le TCP freine le serveur */
        uint32_t k = (uint32_t)n < free_ ? (uint32_t)n : free_;
        for (uint32_t i = 0; i < k; i++) s_ring[(s_wr + i) & (RING - 1)] = p[i];
        s_wr += k;
        p += k;
        n -= (int)k;
    }
    return true;
}

enum { ST_QUIT = 0, ST_LOST, ST_FAIL };

static int stream(const char *start_url)
{
    static char url[256];
    strlcpy(url, start_url, sizeof url);
    uint8_t *buf = ps_malloc(PLS_MAX + 1);
    char *meta = ps_malloc(META_MAX + 1);
    if (!buf || !meta) { free(buf); free(meta); set_state(RAD_FAIL, N_("Mémoire insuffisante"), ""); return ST_FAIL; }
    int ret = ST_FAIL;
    esp_http_client_handle_t c = NULL;

    for (int depth = 0; depth < 4; depth++) {
        s_ctype[0] = s_icy_name[0] = 0;
        s_metaint = s_icy_br = 0;
        esp_http_client_config_t cfg = {
            .url = url,
            .event_handler = http_ev,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            /* navigateur : les serveurs Shoutcast répondent alors en HTTP et pas « ICY 200 » */
            .user_agent = "Mozilla/5.0 (T-Watch 2020; ESP32) WebRadio",
            .max_redirection_count = 5,
        };
        c = esp_http_client_init(&cfg);
        if (!c) { set_state(RAD_FAIL, N_("Mémoire insuffisante"), ""); goto out; }
        esp_http_client_set_header(c, "Icy-MetaData", "1");
        esp_http_client_set_header(c, "Accept", "*/*");
        int status = 0;
        for (int redir = 0; redir < 5; redir++) {
            if (esp_http_client_open(c, 0) != ESP_OK) { status = -1; break; }
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            if (status >= 300 && status < 400 && esp_http_client_set_redirection(c) == ESP_OK) {
                esp_http_client_close(c);
                continue;
            }
            break;
        }
        if (s_quit || s_switch >= 0) { ret = ST_QUIT; goto out; }
        if (status < 0) {
            char host[48];
            rad_station_t tmp = { 0 };
            strlcpy(tmp.url, url, sizeof tmp.url);
            rad_label(&tmp, host, sizeof host);
            set_state(RAD_FAIL, N_("Flux injoignable"), host);
            ble_diag("RAD ouverture impossible");
            goto out;
        }
        if (status != 200) {
            char d[24];
            snprintf(d, sizeof d, "HTTP %d", status);
            set_state(RAD_FAIL, N_("Flux refusé par le serveur"), d);
            ble_diag("RAD HTTP %d", status);
            goto out;
        }
        /* liste de lecture ? */
        bool pls = ctype_has("mpegurl") || ctype_has("scpls") || ctype_has("pls+xml") ||
                   url_ends(url, ".m3u") || url_ends(url, ".m3u8") || url_ends(url, ".pls");
        if (pls) {
            int n = 0, r;
            while (n < PLS_MAX && (r = esp_http_client_read(c, (char *)buf + n, PLS_MAX - n)) > 0) n += r;
            buf[n] = 0;
            bool hls = false;
            static char next[256];
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            c = NULL;
            if (!playlist_pick((char *)buf, next, sizeof next, &hls)) {
                set_state(RAD_FAIL, hls ? N_("Flux HLS non pris en charge") : N_("Liste de lecture vide"), "");
                goto out;
            }
            strlcpy(url, next, sizeof url);
            ESP_LOGI(TAG, "liste → %s", url);
            continue;
        }
        if (ctype_has("ogg") || ctype_has("opus") || ctype_has("flac")) {
            set_state(RAD_FAIL, N_("Format non pris en charge"), s_ctype);
            goto out;
        }
        break;
    }
    if (!c) { set_state(RAD_FAIL, N_("Liste de lecture vide"), ""); goto out; }

    /* codec : type annoncé, sinon premiers octets */
    int codec = ctype_has("aac") ? C_AAC : (ctype_has("mpeg") || ctype_has("mp3")) ? C_MP3 : C_NONE;
    int n = esp_http_client_read(c, (char *)buf, 1024);
    if (n <= 0) { set_state(RAD_FAIL, N_("Flux muet"), ""); ret = ST_LOST; goto out; }
    if (codec == C_NONE) {
        if (n >= 3 && !memcmp(buf, "ID3", 3)) codec = C_MP3;
        else for (int i = 0; i + 1 < n && codec == C_NONE; i++)
            if (buf[i] == 0xFF && (buf[i + 1] & 0xF0) == 0xF0) codec = (buf[i + 1] & 0x06) == 0 ? C_AAC : C_MP3;
        if (codec == C_NONE) { set_state(RAD_FAIL, N_("Format non pris en charge"), s_ctype); goto out; }
    }
    lock();
    s_info.title[0] = 0;
    s_info.kbps = s_icy_br;
    s_info.codec[0] = 0;
    if (!s_st[s_state.station].name[0] && s_icy_name[0]) strlcpy(s_info.name, s_icy_name, sizeof s_info.name);
    unlock();
    /* environ 2 s d'avance (8 s au plus) avant de jouer */
    uint32_t pb = s_icy_br > 0 ? (uint32_t)s_icy_br * 1000 / 8 * 2 : 32768;
    if (pb < 8192) pb = 8192;
    if (pb > RING / 2) pb = RING / 2;
    s_prebuf = pb;
    s_rd = s_wr;                                      /* tampon vidé */
    s_codec = codec;
    s_gen++;
    s_streaming = true;
    set_state(RAD_BUFFER, N_("Mise en mémoire…"), "");
    ESP_LOGI(TAG, "flux %s, %s, metaint %d, %d kbit/s", url, codec == C_MP3 ? "MP3" : "AAC", s_metaint, s_icy_br);

    /* lecture : audio vers le tampon, métadonnées ICY à part */
    int audio_left = s_metaint, meta_left = 0, meta_len = 0, idle = 0;
    for (;;) {
        const uint8_t *p = buf;
        int m = n;
        while (m > 0) {
            if (s_metaint <= 0) {
                if (!ring_put(p, m)) { ret = ST_QUIT; goto out; }
                m = 0;
            } else if (meta_left > 0) {
                int k = m < meta_left ? m : meta_left;
                if (meta_len + k <= META_MAX) memcpy(meta + meta_len, p, k);
                meta_len += k;
                meta_left -= k;
                p += k; m -= k;
                if (meta_left == 0) {
                    meta[meta_len < META_MAX ? meta_len : META_MAX] = 0;
                    on_meta(meta);
                    audio_left = s_metaint;
                }
            } else if (audio_left == 0) {
                meta_left = *p * 16;                    /* longueur des métadonnées / 16 */
                meta_len = 0;
                p++; m--;
                if (meta_left == 0) audio_left = s_metaint;
            } else {
                int k = m < audio_left ? m : audio_left;
                if (!ring_put(p, k)) { ret = ST_QUIT; goto out; }
                audio_left -= k;
                p += k; m -= k;
            }
        }
        if (s_quit || s_switch >= 0) { ret = ST_QUIT; goto out; }
        if (wsta_lost()) { set_state(RAD_CONNECT, N_("Wi-Fi perdu"), ""); ret = ST_LOST; goto out; }
        n = esp_http_client_read(c, (char *)buf, 2048);
        if (n == -ESP_ERR_HTTP_EAGAIN || n == 0) {
            if (n == 0 && esp_http_client_is_complete_data_received(c)) {
                set_state(RAD_CONNECT, N_("Flux terminé, reprise…"), "");
                ret = ST_LOST;
                goto out;
            }
            if (++idle > 2) {                           /* ~20 s sans rien */
                set_state(RAD_CONNECT, N_("Flux interrompu, reprise…"), "");
                ble_diag("RAD flux muet");
                ret = ST_LOST;
                goto out;
            }
            n = 0;
            continue;
        }
        if (n < 0) {
            set_state(RAD_CONNECT, N_("Flux interrompu, reprise…"), "");
            ble_diag("RAD lecture %d", n);
            ret = ST_LOST;
            goto out;
        }
        idle = 0;
        /* puissance d'émission réduite écran éteint si le signal le permet */
        static int64_t last_pw;
        int64_t now = esp_timer_get_time();
        if (now - last_pw > 2000000) {
            last_pw = now;
            int rssi = wsta_rssi();
            lock(); s_info.rssi = rssi; unlock();
            static int8_t cur = 0;
            int8_t want = (!s_screen && rssi > -65) ? 44 : 78;    /* 11 dBm / 19,5 dBm */
            if (want != cur && esp_wifi_set_max_tx_power(want) == ESP_OK) cur = want;
        }
    }
out:
    s_streaming = false;
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    free(buf);
    free(meta);
    return ret;
}

/* ------------------------------------------------------------------ */
static void wifi_status(bool fail, const char *msg, const char *detail)
{
    set_state(fail ? RAD_FAIL : RAD_WIFI, msg, detail);
}

static void apply_switch(void)
{
    int to = s_switch;
    if (to < 0) return;
    s_switch = -1;
    s_state.station = (uint8_t)to;
    lock();
    s_info.station = (uint8_t)to;
    rad_label(&s_st[to], s_info.name, sizeof s_info.name);
    s_info.title[0] = 0;
    s_info.codec[0] = 0;
    s_info.kbps = 0;
    unlock();
}

static void net_task(void *arg)
{
    int fails = 0;
    power_set_cpu_max(160);
    /* modem sleep : 128 kbit/s tiennent largement dans les réveils ; le
     * tampon de plusieurs secondes absorbe la latence */
    if (!wsta_start(WIFI_PS_MIN_MODEM)) set_state(RAD_FAIL, N_("Erreur Wi-Fi"), "");
    else while (!s_quit) {
        apply_switch();
        bool ok = wsta_up() || wsta_connect(s_cfg, &s_quit, wifi_status);
        if (s_quit) break;
        int r = ST_FAIL;
        if (ok) {
            set_state(RAD_CONNECT, N_("Connexion au flux"), s_info.name);
            r = stream(s_st[s_state.station].url);
        }
        if (s_quit) break;
        if (r == ST_QUIT) continue;                 /* autre station : tout de suite */
        /* coupure : 2 s ; échec : 5, 10, 20, 30, 60 s, puis toutes les 60 s */
        int wait;
        if (r == ST_LOST) { wait = 2; fails = 0; }
        else { static const int W[] = { 5, 10, 20, 30, 60 }; wait = W[fails < 4 ? fails : 4]; fails++; }
        for (int t = wait; t > 0 && !s_quit && s_switch < 0; t--) {
            lock(); s_info.retry_s = t; unlock();
            if (xEventGroupWaitBits(s_evg, BIT_RETRY | BIT_QUIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000)) & BIT_RETRY) break;
        }
        lock(); s_info.retry_s = 0; unlock();
    }
    s_streaming = false;
    wsta_stop();
    xEventGroupSetBits(s_evg, BIT_NETEND);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */
static bool alloc(void)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    if (!s_evg) s_evg = xEventGroupCreate();
    if (!s_ring) s_ring = heap_caps_malloc(RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_st) s_st = ps_malloc(RAD_MAX * sizeof *s_st);
    if (!s_cfg) s_cfg = ps_malloc(sizeof *s_cfg);
    if (!s_boost_lock) esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "radio", &s_boost_lock);
    return s_mx && s_evg && s_ring && s_st && s_cfg;
}

esp_err_t radio_start(void)
{
    if (s_run) return ESP_OK;
    if (!alloc()) return ESP_ERR_NO_MEM;
    wrx_cfg_load(s_cfg);
    s_nst = rad_cfg_load(s_st);
    rad_state_load(&s_state);
    if (s_state.station >= s_nst) s_state.station = 0;
    if (!s_nst) return ESP_ERR_NOT_FOUND;
    lock();
    memset(&s_info, 0, sizeof s_info);
    s_info.volume = s_state.volume;          /* dernier volume retenu */
    s_info.tone = s_state.tone;
    s_info.station = s_state.station;
    s_info.nstations = (uint8_t)s_nst;
    rad_label(&s_st[s_state.station], s_info.name, sizeof s_info.name);
    unlock();
    s_wr = s_rd = 0;
    s_quit = false;
    s_switch = -1;
    s_streaming = false;
    s_codec = C_NONE;
    xEventGroupClearBits(s_evg, 0xFF);
    esp_err_t e = wsta_init("RAD");
    if (e != ESP_OK) return e;
    s_run = true;
    set_state(RAD_WIFI, N_("Recherche du Wi-Fi…"), "");
    if (xTaskCreatePinnedToCore(net_task, "rad", 7168, NULL, 5, NULL, 0) != pdPASS) {
        s_run = false;
        wsta_deinit();
        return ESP_ERR_NO_MEM;
    }
    /* décodeur : pile assez grande pour le HE-AAC (SBR) */
    if (xTaskCreatePinnedToCore(play_task, "radplay", 8192, NULL, 6, NULL, 1) != pdPASS)
        xEventGroupSetBits(s_evg, BIT_PLAYEND);
    return ESP_OK;
}

void radio_stop(void)
{
    if (!s_mx) return;
    if (s_run) {
        s_quit = true;
        xEventGroupSetBits(s_evg, BIT_QUIT);
        EventBits_t b = xEventGroupWaitBits(s_evg, BIT_NETEND | BIT_PLAYEND, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
        if ((b & (BIT_NETEND | BIT_PLAYEND)) != (BIT_NETEND | BIT_PLAYEND))
            ESP_LOGE(TAG, "arrêt incomplet (0x%x)", (unsigned)b);
        s_run = false;
        lock();
        s_state.tone = s_info.tone;
        s_state.volume = s_info.volume;          /* 0 : non retenu (VOL_DEF au départ) */
        s_info.state = RAD_OFF;
        s_info.msg = NULL;
        unlock();
        rad_state_save(&s_state);
    }
    wsta_deinit();
    power_set_cpu_max(0);
}

bool radio_running(void) { return s_run; }
bool radio_ble_paused(void) { return wsta_ble_paused(); }
void radio_retry(void) { if (s_run) xEventGroupSetBits(s_evg, BIT_RETRY); }
void radio_screen(bool on) { s_screen = on; }

void radio_info(rad_info_t *out)
{
    if (!s_mx) {
        memset(out, 0, sizeof *out);
        rad_state_t st;
        rad_state_load(&st);
        out->volume = st.volume;
        out->tone = st.tone;
        out->station = st.station;
        return;
    }
    lock();
    *out = s_info;
    unlock();
}

void radio_station(int idx)
{
    if (idx < 0 || idx >= RAD_MAX) return;
    if (s_run) {
        if (idx >= s_nst || idx == s_info.station) return;
        s_switch = idx;
        xEventGroupSetBits(s_evg, BIT_RETRY);
        return;
    }
    rad_state_t st;
    rad_state_load(&st);
    st.station = (uint8_t)idx;
    rad_state_save(&st);
    if (s_mx) { lock(); s_info.station = (uint8_t)idx; unlock(); s_state.station = (uint8_t)idx; }
}

void radio_volume(int pct)
{
    if (!s_mx) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lock(); s_info.volume = (uint8_t)pct; unlock();
}

void radio_tone(int level)
{
    if (level < 0 || level >= TONE_LEVELS) return;
    if (!s_mx) { rad_state_t st; rad_state_load(&st); st.tone = (uint8_t)level; rad_state_save(&st); return; }
    lock(); s_info.tone = (uint8_t)level; s_state.tone = (uint8_t)level; unlock();
}
