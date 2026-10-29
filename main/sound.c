/*
 * sound.c — voir sound.h. Une tâche éphémère crée le canal I2S, joue, puis le
 * détruit et remet les broches à 0 (état de repos de power.c).
 */
#include <math.h>
#include <string.h>
#include "sound.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "sound"
#define RATE 16000
#define AMPL 9000                /* ~ -11 dBFS : net sans saturer le petit HP */
#define PIN_BCK 26
#define PIN_WS  25
#define PIN_DO  33

typedef struct { int n, freq, on_ms, off_ms; } beeps_t;
static volatile int s_last_err;          /* 0 = joué, sinon code esp_err_t */
static volatile bool s_busy;
static beeps_t s_req;

static void write_tone(i2s_chan_handle_t ch, int freq, int ms)
{
    int16_t buf[2 * 128];                        /* stéréo : même échantillon à gauche et à droite */
    int total = RATE * ms / 1000, done = 0;
    float ph = 0, step = freq ? 2.0f * (float)M_PI * freq / RATE : 0;
    int ramp = RATE / 200;                       /* 5 ms de fondu : pas de clic */
    while (done < total) {
        int k = total - done < 128 ? total - done : 128;
        for (int i = 0; i < k; i++) {
            int t = done + i;
            float env = 1.0f;
            if (t < ramp) env = (float)t / ramp;
            else if (total - t < ramp) env = (float)(total - t) / ramp;
            int16_t v = freq ? (int16_t)(AMPL * env * sinf(ph)) : 0;
            buf[2 * i] = buf[2 * i + 1] = v;
            ph += step;
            if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
        }
        size_t w;
        i2s_channel_write(ch, buf, 2 * k * sizeof(int16_t), &w, pdMS_TO_TICKS(500));
        done += k;
    }
}

static void sound_task(void *arg)
{
    beeps_t r = s_req;
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        /* stéréo explicite : en mono, l'ESP32 ne remplit qu'un canal et
         * l'ampli peut écouter l'autre */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = PIN_BCK, .ws = PIN_WS,
            .dout = PIN_DO, .din = I2S_GPIO_UNUSED,
        },
    };
    esp_err_t e = i2s_new_channel(&cc, &tx, NULL);
    if (e == ESP_OK) e = i2s_channel_init_std_mode(tx, &sc);
    if (e == ESP_OK) e = i2s_channel_enable(tx);
    s_last_err = e;
    if (e == ESP_OK) {
        for (int i = 0; i < r.n; i++) {
            write_tone(tx, r.freq, r.on_ms);
            write_tone(tx, 0, i + 1 < r.n ? r.off_ms : 40);   /* silence final : vide le DMA */
        }
        i2s_channel_disable(tx);
    } else {
        ESP_LOGW(TAG, "I2S indisponible : %s", esp_err_to_name(e));
    }
    if (tx) i2s_del_channel(tx);
    /* broches au repos (sorties à 0, comme au démarrage) */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_BCK) | (1ULL << PIN_WS) | (1ULL << PIN_DO),
        .mode = GPIO_MODE_OUTPUT,             /* pas de tirage : aucune fuite */
    };
    gpio_config(&out);
    gpio_set_level(PIN_BCK, 0);
    gpio_set_level(PIN_WS, 0);
    gpio_set_level(PIN_DO, 0);
    s_busy = false;
    vTaskDelete(NULL);
}

bool sound_busy(void) { return s_busy; }
int  sound_last_error(void) { return s_last_err; }

void sound_beeps(int n, int freq_hz, int on_ms, int off_ms)
{
    if (s_busy || n <= 0) return;
    s_busy = true;
    s_req = (beeps_t){ n, freq_hz, on_ms, off_ms };
    if (xTaskCreate(sound_task, "sound", 3072, NULL, 3, NULL) != pdPASS) s_busy = false;
}
