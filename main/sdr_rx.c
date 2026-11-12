/*
 * sdr_rx.c — capture I/Q brute par la radio Wi-Fi de l'ESP32, FFT, lignes de
 * spectre. Voir sdr_rx.h.
 *
 * Principe (mis au jour par le projet ESP-SDR / ESPARGOS, 2026 ; ce fichier
 * est une réalisation indépendante) : le MAC Wi-Fi de l'ESP32 possède un mode
 * « dump » qui recopie les échantillons du convertisseur de réception dans la
 * SRAM interne, à 80, 40 ou 16 MS/s. Les registres :
 *   DUMP_CTRL   0x60033D90  nb d'échantillons [14:0], horloge (b15 = 40 MS/s,
 *                           b16 = 16 MS/s), source [21:20], armement b31,
 *                           déclenchement b19 (impulsion), fin b18
 *   DUMP_STATUS 0x60033D94  nb d'échantillons écrits [14:0]
 *   DUMP_BYTES  0x60033DC4  ordre des octets (0x03020100)
 *   DPORT_IRAM_DRAM_AHB_SEL.MAC_DUMP_MODE = 3 : bancs SRAM 0x3FFE8000–0x3FFF8000
 *                           confiés au MAC le temps de la capture
 *   ADC_CTRL    0x3FF460A0  b8|b9 : force l'ADC de réception allumé
 *   RX_GAIN     0x3FF5C02C  b23 = gain manuel, [30:24] code, [14:8] code max
 * Chaque échantillon est un mot de 32 bits : I [9:0], Q [19:10] (signés),
 * gain AGC [31:20].
 *
 * Accord : canaux Wi-Fi normaux (esp_wifi_set_channel), puis remise de la
 * chaîne radio en réception pure (fonctions ROM pbus). Pas d'émission.
 */
#include "sdr_rx.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/dport_reg.h"
#include "soc/dport_access.h"
#include "sdr_mode.h"
#include "power.h"

static const char *TAG = "sdr";

#define DUMP_CTRL    0x60033d90u
#define DUMP_STATUS  0x60033d94u
#define DUMP_BYTES   0x60033dc4u
#define ADC_CTRL     0x3ff460a0u
#define ADC_ON_FORCE (BIT(9) | BIT(8))
#define RX_GAIN      0x3ff5c02cu

extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(int);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(int);

#define FS_MHZ     40.0f             /* 40 MS/s complexes */
#define N_CAP      4096              /* 102 µs par capture */
#define NFFT_BAND  256               /* 156 kHz par case */
#define NFFT_ZOOM  512               /* 78 kHz par case */
#define NFFT_MAX   512

/* vue bande : 2401–2483 MHz, 5 accords (canaux 1, 4, 7, 10, 13) */
#define BAND_F0    2401.0f
#define BAND_SPAN  82.0f
#define ZOOM_SPAN  20.0f
static const uint8_t SEG_CH[5] = { 1, 4, 7, 10, 13 };
#define NSEG 5

static inline float ch_mhz(int ch) { return ch == 14 ? 2484.0f : 2407.0f + 5.0f * ch; }

/* ------------------------------------------------------------------ */
/* état partagé                                                         */
/* ------------------------------------------------------------------ */
static volatile int s_view = SDR_VIEW_BAND, s_ch = 6, s_gain = 3;
static volatile bool s_run, s_pause;
static TaskHandle_t s_task;
static const char *s_err;
static unsigned s_gain_max = 100;
static float s_rate;

#define RING 8
static sdr_row_t *s_ring;            /* RING lignes */
static volatile uint32_t s_head, s_tail;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* DSP */
static float *s_re, *s_im;           /* NFFT_MAX */
static float *s_cos, *s_sin;         /* NFFT_MAX/2 */
static float *s_win_band, *s_win_zoom;
static float s_norm_band, s_norm_zoom;   /* 1 / (511·Σw)² */
static float *s_acc;                 /* NSEG × NFFT_BAND, ou NFFT_ZOOM */
static uint8_t s_map_seg[SDR_W];
static uint16_t s_map_k0[SDR_W], s_map_k1[SDR_W];

static volatile uint32_t *const IQ = (volatile uint32_t *)SDR_DUMP_START;

/* ------------------------------------------------------------------ */
/* radio                                                                */
/* ------------------------------------------------------------------ */
static void apply_gain(int g)
{
    uint32_t r = REG_READ(RX_GAIN);
    if (g <= 0) {
        REG_CLR_BIT(RX_GAIN, BIT(23));           /* AGC matériel */
    } else {
        static const uint8_t PCT[5] = { 0, 35, 55, 75, 95 };
        unsigned code = s_gain_max * PCT[g > 4 ? 4 : g] / 100;
        REG_WRITE(RX_GAIN, (r & 0x7fffffu) | (code << 24) | BIT(23));
    }
}

static int s_tuned = -1;

static void tune(int ch, int gain)
{
    if (ch != s_tuned) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        s_tuned = ch;
    }
    /* réception pure : la chaîne d'émission coupée, horloge RX forcée */
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    apply_gain(gain);                            /* après pbus, qui relâche le gain forcé */
}

static bool capture(unsigned n)
{
    REG_WRITE(DUMP_CTRL, 0);
    uint32_t adc = DPORT_REG_READ(ADC_CTRL) & ADC_ON_FORCE;
    DPORT_REG_WRITE(ADC_CTRL, DPORT_REG_READ(ADC_CTRL) | ADC_ON_FORCE);
    esp_rom_delay_us(2);
    uint32_t owner = DPORT_REG_READ(DPORT_IRAM_DRAM_AHB_SEL_REG);
    uint32_t bsel = REG_READ(DUMP_BYTES);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG, (owner & ~DPORT_MAC_DUMP_MODE_M) | (3u << DPORT_MAC_DUMP_MODE_S));
    REG_WRITE(DUMP_BYTES, 0x03020100u);
    REG_WRITE(DUMP_CTRL, n | BIT(15) | BIT(31));     /* 40 MS/s, source 0, armé */
    int64_t t0 = esp_timer_get_time();
    REG_SET_BIT(DUMP_CTRL, BIT(19));
    REG_CLR_BIT(DUMP_CTRL, BIT(19));
    while (!(REG_READ(DUMP_CTRL) & BIT(18)) && esp_timer_get_time() - t0 < 20000) {}
    uint32_t done = REG_READ(DUMP_CTRL), st = REG_READ(DUMP_STATUS);
    REG_WRITE(DUMP_CTRL, 0);
    esp_rom_delay_us(10);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG, owner);
    REG_WRITE(DUMP_BYTES, bsel);
    DPORT_REG_WRITE(ADC_CTRL, (DPORT_REG_READ(ADC_CTRL) & ~ADC_ON_FORCE) | adc);
    return (done & BIT(18)) && (st & 0x7fffu) == n;
}

/* ------------------------------------------------------------------ */
/* DSP                                                                  */
/* ------------------------------------------------------------------ */
static void fft(float *re, float *im, int n)
{
    /* permutation par inversion des bits */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    /* papillons radix 2 ; tables pour NFFT_MAX, pas de NFFT_MAX/n */
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = NFFT_MAX / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = s_cos[k * step], wi = -s_sin[k * step];
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
        }
    }
}

/* capture → puissance moyenne par case (ordre centré : DC en n/2) ; renvoie
 * l'amplitude crête vue (saturation à 511) */
static int spectrum_of_capture(float *acc, int nfft, const float *win)
{
    /* composante continue (fuite de l'oscillateur local) retirée */
    int32_t si = 0, sq = 0, peak = 0;
    for (int j = 0; j < N_CAP; j++) {
        uint32_t w = IQ[j];
        int32_t i = (int32_t)(w << 22) >> 22, q = (int32_t)(w << 12) >> 22;
        si += i; sq += q;
        int32_t a = i < 0 ? -i : i, b = q < 0 ? -q : q;
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    float mi = (float)si / N_CAP, mq = (float)sq / N_CAP;
    memset(acc, 0, nfft * sizeof(float));
    int nb = N_CAP / nfft;
    for (int b = 0; b < nb; b++) {
        const volatile uint32_t *p = IQ + b * nfft;
        for (int j = 0; j < nfft; j++) {
            uint32_t w = p[j];
            s_re[j] = ((float)((int32_t)(w << 22) >> 22) - mi) * win[j];
            s_im[j] = ((float)((int32_t)(w << 12) >> 22) - mq) * win[j];
        }
        fft(s_re, s_im, nfft);
        int h = nfft / 2;
        for (int k = 0; k < nfft; k++) {
            int d = (k + h) & (nfft - 1);        /* centrage */
            acc[d] += s_re[k] * s_re[k] + s_im[k] * s_im[k];
        }
    }
    float inv = 1.0f / nb;
    for (int k = 0; k < nfft; k++) acc[k] *= inv;
    /* raie résiduelle de l'oscillateur local au centre : interpolée */
    int c = nfft / 2;
    float fill = 0.5f * (acc[c - 3] + acc[c + 3]);
    for (int k = c - 2; k <= c + 2; k++) acc[k] = fill;
    return peak;
}

static void build_maps(int view, int ch)
{
    for (int x = 0; x < SDR_W; x++) {
        float f0 = sdr_rx_mhz(view, ch, (float)x), f1 = sdr_rx_mhz(view, ch, (float)x + 1.0f);
        float fm = 0.5f * (f0 + f1), fc;
        int nfft;
        if (view == SDR_VIEW_BAND) {
            int best = 0;
            for (int s = 1; s < NSEG; s++)
                if (fabsf(fm - ch_mhz(SEG_CH[s])) < fabsf(fm - ch_mhz(SEG_CH[best]))) best = s;
            s_map_seg[x] = best;
            fc = ch_mhz(SEG_CH[best]);
            nfft = NFFT_BAND;
        } else {
            s_map_seg[x] = 0;
            fc = ch_mhz(ch);
            nfft = NFFT_ZOOM;
        }
        float bw = FS_MHZ / nfft;
        int k0 = (int)floorf((f0 - fc) / bw) + nfft / 2;
        int k1 = (int)floorf((f1 - fc) / bw - 1e-4f) + nfft / 2;
        if (k1 < k0) k1 = k0;
        if (k0 < 0) k0 = 0;
        if (k1 > nfft - 1) k1 = nfft - 1;
        s_map_k0[x] = (uint16_t)k0;
        s_map_k1[x] = (uint16_t)k1;
    }
}

static void push_row(const sdr_row_t *r)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t h = s_head;
    if (h - s_tail >= RING) s_tail = h - RING + 1;   /* l'interface traîne : on perd la plus vieille */
    s_ring[h % RING] = *r;
    s_head = h + 1;
    portEXIT_CRITICAL(&s_mux);
}

bool sdr_rx_pop(sdr_row_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_ring && s_tail != s_head) {
        *out = s_ring[s_tail % RING];
        s_tail++;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static void sdr_task(void *arg)
{
    int map_view = -1, map_ch = -1, fails = 0, rows = 0;
    int64_t t_rate = esp_timer_get_time();
    static sdr_row_t row;
    while (s_run) {
        if (s_pause) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
        int view = s_view, ch = s_ch, gain = s_gain;
        if (view != map_view || (view == SDR_VIEW_ZOOM && ch != map_ch)) {
            build_maps(view, ch);
            map_view = view; map_ch = ch;
        }
        int peak = 0;
        bool ok = true;
        if (view == SDR_VIEW_BAND) {
            for (int s = 0; s < NSEG && ok; s++) {
                tune(SEG_CH[s], gain);
                ok = capture(N_CAP);
                if (ok) {
                    int p = spectrum_of_capture(s_acc + s * NFFT_BAND, NFFT_BAND, s_win_band);
                    if (p > peak) peak = p;
                }
            }
        } else {
            tune(ch, gain);
            ok = capture(N_CAP);
            if (ok) peak = spectrum_of_capture(s_acc, NFFT_ZOOM, s_win_zoom);
        }
        if (!ok) {
            if (++fails > 20) s_err = "capture";
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        fails = 0;
        s_err = NULL;
        /* colonnes : maximum des cases couvertes (une raie fine reste visible) */
        float norm = view == SDR_VIEW_BAND ? s_norm_band : s_norm_zoom;
        for (int x = 0; x < SDR_W; x++) {
            const float *a = view == SDR_VIEW_BAND ? s_acc + s_map_seg[x] * NFFT_BAND : s_acc;
            int k0 = s_map_k0[x], k1 = s_map_k1[x];
            float m = 0;
            for (int k = k0; k <= k1; k++) if (a[k] > m) m = a[k];
            row.db[x] = 10.0f * log10f(m * norm + 1e-12f);
        }
        row.view = view; row.ch = ch; row.gain = gain;
        row.sat = peak >= 500;
        push_row(&row);
        rows++;
        int64_t now = esp_timer_get_time();
        if (now - t_rate > 1000000) {
            s_rate = rows * 1e6f / (now - t_rate);
            rows = 0;
            t_rate = now;
        }
        vTaskDelay(1);                           /* laisse respirer le cœur 0 (Wi-Fi, idle) */
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */
float sdr_rx_mhz(int view, int ch, float x)
{
    if (view == SDR_VIEW_BAND) return BAND_F0 + x * (BAND_SPAN / SDR_W);
    return ch_mhz(ch) - ZOOM_SPAN / 2 + x * (ZOOM_SPAN / SDR_W);
}

float sdr_rx_x(int view, int ch, float mhz)
{
    if (view == SDR_VIEW_BAND) return (mhz - BAND_F0) * (SDR_W / BAND_SPAN);
    return (mhz - ch_mhz(ch) + ZOOM_SPAN / 2) * (SDR_W / ZOOM_SPAN);
}

float sdr_rx_rate(void) { return s_rate; }
const char *sdr_rx_error(void) { return s_err; }

void sdr_rx_config(int view, int ch, int gain)
{
    s_view = view;
    s_ch = ch < 1 ? 1 : ch > 13 ? 13 : ch;
    s_gain = gain < 0 ? 0 : gain >= SDR_GAIN_LEVELS ? SDR_GAIN_LEVELS - 1 : gain;
}

void sdr_rx_pause(bool p) { s_pause = p; }

static void *alloc_f(size_t n, bool internal)
{
    void *p = heap_caps_malloc(n * sizeof(float), internal ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
                                                           : MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n * sizeof(float), MALLOC_CAP_8BIT);
}

static void free_all(void)
{
    heap_caps_free(s_re); heap_caps_free(s_im); heap_caps_free(s_cos); heap_caps_free(s_sin);
    heap_caps_free(s_win_band); heap_caps_free(s_win_zoom); heap_caps_free(s_acc); heap_caps_free(s_ring);
    s_re = s_im = s_cos = s_sin = s_win_band = s_win_zoom = s_acc = NULL;
    s_ring = NULL;
}

esp_err_t sdr_rx_start(void)
{
    if (s_run) return ESP_OK;
    s_err = NULL;
    if (!sdr_mode_active()) { s_err = "mode"; return ESP_ERR_INVALID_STATE; }

    /* tampons de calcul en RAM interne (FFT), le reste où il y a de la place */
    s_re = alloc_f(NFFT_MAX, true);
    s_im = alloc_f(NFFT_MAX, true);
    s_cos = alloc_f(NFFT_MAX / 2, true);
    s_sin = alloc_f(NFFT_MAX / 2, true);
    s_win_band = alloc_f(NFFT_BAND, true);
    s_win_zoom = alloc_f(NFFT_ZOOM, true);
    s_acc = alloc_f(NSEG * NFFT_BAND > NFFT_ZOOM ? NSEG * NFFT_BAND : NFFT_ZOOM, true);
    s_ring = heap_caps_calloc(RING, sizeof(sdr_row_t), MALLOC_CAP_8BIT);
    if (!s_re || !s_im || !s_cos || !s_sin || !s_win_band || !s_win_zoom || !s_acc || !s_ring) {
        free_all();
        s_err = "mem";
        return ESP_ERR_NO_MEM;
    }
    for (int k = 0; k < NFFT_MAX / 2; k++) {
        s_cos[k] = cosf(2.0f * (float)M_PI * k / NFFT_MAX);
        s_sin[k] = sinf(2.0f * (float)M_PI * k / NFFT_MAX);
    }
    float sb = 0, sz = 0;                        /* fenêtres de Hann */
    for (int j = 0; j < NFFT_BAND; j++) sb += s_win_band[j] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * j / NFFT_BAND);
    for (int j = 0; j < NFFT_ZOOM; j++) sz += s_win_zoom[j] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * j / NFFT_ZOOM);
    s_norm_band = 1.0f / ((511.0f * sb) * (511.0f * sb));
    s_norm_zoom = 1.0f / ((511.0f * sz) * (511.0f * sz));
    s_head = s_tail = 0;

    /* la radio Wi-Fi en mode moniteur, rien n'est émis ni enregistré */
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { free_all(); s_err = "evt"; return e; }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    if ((e = esp_wifi_init(&cfg)) != ESP_OK) { free_all(); s_err = "wifi"; return e; }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    if ((e = esp_wifi_start()) != ESP_OK) { esp_wifi_deinit(); free_all(); s_err = "wifi"; return e; }
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    s_tuned = 1;
    tune(1, 0);
    s_gain_max = (REG_READ(RX_GAIN) >> 8) & 127;
    if (s_gain_max < 10) s_gain_max = 100;
    ESP_LOGI(TAG, "radio prête, code de gain max %u", s_gain_max);

    power_hold_active(true);                     /* 240 MHz, pas de veille légère */
    s_run = true;
    s_pause = false;
    if (xTaskCreatePinnedToCore(sdr_task, "sdr", 4096, NULL, 4, &s_task, 0) != pdPASS) {
        s_run = false;
        power_hold_active(false);
        esp_wifi_stop(); esp_wifi_deinit();
        free_all();
        s_err = "task";
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void sdr_rx_stop(void)
{
    if (!s_run) return;
    s_run = false;
    for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
    power_hold_active(false);
    portENTER_CRITICAL(&s_mux);
    sdr_row_t *r = s_ring;
    s_ring = NULL;                               /* sdr_rx_pop ne lit plus rien */
    portEXIT_CRITICAL(&s_mux);
    heap_caps_free(r);
    free_all();
}
