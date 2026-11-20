/* demo_synth.c — voir demo_synth.h */
#include "demo_synth.h"
#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "sound.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* ------------------------------------------------------------------ partition */
/* accords (une mesure chacun) : la mineur, fa, do, sol */
static const uint8_t CHORD[4][3] = { { 69, 72, 76 }, { 65, 69, 72 }, { 67, 72, 76 }, { 67, 71, 74 } };
static const uint8_t ROOT[4] = { 45, 41, 48, 43 };

/* mélodie : 0 = on tient, 1 = silence, sinon note MIDI */
static const uint8_t LEAD[8][16] = {
    { 76, 0, 0, 74, 0, 0, 72, 0, 74, 0, 76, 0, 0, 0, 69, 0 },
    { 72, 0, 0, 71, 0, 0, 69, 0, 71, 0, 72, 0, 77, 0, 76, 0 },
    { 76, 0, 0, 74, 0, 0, 72, 0, 79, 0, 0, 0, 76, 0, 74, 0 },
    { 74, 0, 0, 0, 71, 0, 74, 0, 79, 0, 0, 0, 76, 0, 74, 0 },
    { 76, 0, 0, 74, 0, 0, 72, 0, 74, 0, 76, 0, 79, 0, 81, 0 },
    { 77, 0, 0, 76, 0, 0, 72, 0, 69, 0, 72, 0, 77, 0, 76, 0 },
    { 76, 0, 79, 0, 76, 0, 74, 0, 72, 0, 74, 0, 76, 0, 79, 0 },
    { 74, 0, 72, 0, 71, 0, 72, 0, 74, 0, 0, 0, 69, 0, 0, 1 },
};

enum { D_K = 1, D_S = 2, D_H = 4 };
static const uint8_t DR_BUILD[16] = { 0, 0, D_H, 0, 0, 0, D_H, 0, 0, 0, D_H, 0, 0, 0, D_H, D_H };
static const uint8_t DR_FULL[16]  = { D_K, 0, D_H, 0, D_S, 0, D_H, D_K, D_K, 0, D_H, 0, D_S, 0, D_H, D_H };
static const uint8_t DR_FILL[16]  = { D_K, 0, D_H, 0, D_S, 0, D_H, D_K, D_K, 0, D_S, D_S, D_S, D_S, D_S, D_S };

static int song_bar(int bar) { return bar < 16 ? bar : DEMO_LOOP_BAR + (bar - 16) % 8; }

static uint8_t drums_at(int bar, int step)
{
    int b = song_bar(bar);
    if (b < 4) return 0;
    if (b < 8) return b == 7 && step >= 12 ? D_S : DR_BUILD[step];
    return (b == 15 ? DR_FILL : DR_FULL)[step];
}

/* ------------------------------------------------------------------ voix */
typedef struct {
    uint32_t ph, inc;
    int32_t env;            /* Q15 */
    int32_t decay;          /* multiplicateur Q15 par échantillon */
} voice_t;

static voice_t v_lead, v_bass, v_arp;
static uint32_t s_t;                  /* échantillons depuis le début */
static uint32_t k_t = 1 << 30, s_tt = 1 << 30, h_t = 1 << 30;   /* depuis le dernier coup */
static float k_ph;
static uint32_t lfsr = 0xACE1u;
static int32_t hat_prev;

static uint32_t note_inc(int midi)
{
    float f = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
    return (uint32_t)(f * 4294967296.0f / DEMO_RATE);
}

static int32_t decay_q15(float ms) { return (int32_t)(32768.0f * expf(-1000.0f / (ms * DEMO_RATE))); }

static void on_step(int bar, int step)
{
    int b = song_bar(bar), ch = b & 3;
    /* arpège : nouvelle note à chaque pas */
    v_arp.inc = note_inc(CHORD[ch][step % 3] + (b >= 8 && (step & 4) ? 12 : 0));
    v_arp.env = 32767;
    v_arp.decay = decay_q15(90);
    /* basse : croches en octaves, à partir de la mesure 4 */
    if (b >= 4 && !(step & 1)) {
        v_bass.inc = note_inc(ROOT[ch] + ((step & 2) ? 12 : 0));
        v_bass.env = 32767;
        v_bass.decay = decay_q15(140);
    }
    /* mélodie à partir de la mesure 8 */
    if (b >= 8) {
        uint8_t n = LEAD[b - 8][step];
        if (n == 1) v_lead.env = 0;
        else if (n) { v_lead.inc = note_inc(n); v_lead.env = 32767; v_lead.decay = decay_q15(420); }
    }
    uint8_t d = drums_at(bar, step);
    if (d & D_K) { k_t = 0; k_ph = 0; }
    if (d & D_S) s_tt = 0;
    if (d & D_H) h_t = 0;
}

static inline uint32_t noise(void)
{
    lfsr ^= lfsr << 13; lfsr ^= lfsr >> 17; lfsr ^= lfsr << 5;
    return lfsr;
}

static int16_t sample(void)
{
    uint32_t step_len = DEMO_RATE * DEMO_STEP_MS / 1000;
    if (s_t % step_len == 0) {
        uint32_t st = s_t / step_len;
        on_step((int)(st / 16), (int)(st % 16));
    }
    s_t++;
    int32_t acc = 0;
    /* lead : carré 25 % */
    if (v_lead.env > 40) {
        acc += ((v_lead.ph >> 30) == 0 ? 4200 : -1400) * v_lead.env >> 15;
        v_lead.ph += v_lead.inc;
        v_lead.env = v_lead.env * v_lead.decay >> 15;
        if (v_lead.env < 13000 && v_lead.decay < 32760) v_lead.decay = decay_q15(1400);   /* tenue */
    }
    /* arpège : carré 50 %, discret */
    if (v_arp.env > 40) {
        acc += ((int32_t)(v_arp.ph >> 31) ? 1500 : -1500) * v_arp.env >> 15;
        v_arp.ph += v_arp.inc;
        v_arp.env = v_arp.env * v_arp.decay >> 15;
    }
    /* basse : carré 50 % */
    if (v_bass.env > 40) {
        acc += ((int32_t)(v_bass.ph >> 31) ? 3600 : -3600) * v_bass.env >> 15;
        v_bass.ph += v_bass.inc;
        v_bass.env = v_bass.env * v_bass.decay >> 15;
    }
    /* grosse caisse : sinus qui plonge */
    if (k_t < DEMO_RATE / 4) {
        float t = k_t / (float)DEMO_RATE;
        float f = 48.0f + 140.0f * expf(-t * 35.0f);
        k_ph += f / DEMO_RATE;
        if (k_ph > 1) k_ph -= 1;
        acc += (int32_t)(9000.0f * expf(-t * 9.0f) * sinf(k_ph * 6.2831853f));
        k_t++;
    }
    /* caisse claire : bruit + ton */
    if (s_tt < DEMO_RATE / 5) {
        float t = s_tt / (float)DEMO_RATE;
        int32_t n = (int32_t)(noise() & 0xFFFF) - 32768;
        acc += (int32_t)(n * 0.11f * expf(-t * 22.0f));
        s_tt++;
    }
    /* charleston : bruit dérivé, très court */
    if (h_t < DEMO_RATE / 20) {
        float t = h_t / (float)DEMO_RATE;
        int32_t n = (int32_t)(noise() & 0xFFFF) - 32768;
        int32_t hp = n - hat_prev;
        hat_prev = n;
        acc += (int32_t)(hp * 0.03f * expf(-t * 90.0f));
        h_t++;
    }
    if (acc > 32767) acc = 32767;
    if (acc < -32768) acc = -32768;
    return (int16_t)acc;
}

static void reset(void)
{
    memset(&v_lead, 0, sizeof v_lead);
    memset(&v_bass, 0, sizeof v_bass);
    memset(&v_arp, 0, sizeof v_arp);
    s_t = 0;
    k_t = s_tt = h_t = 1 << 30;
}

void demo_synth_render(int16_t *out, int n)
{
    reset();
    for (int i = 0; i < n; i++) out[i] = sample();
}

/* ------------------------------------------------------------------ horloge, effets */
#ifdef ESP_PLATFORM
static int64_t s_t0;
static volatile bool s_run, s_sound, s_task;

uint32_t demo_synth_ms(void) { return s_t0 ? (uint32_t)((esp_timer_get_time() - s_t0) / 1000) : 0; }
#else
uint32_t g_demo_ms;                       /* simulateur : horloge pilotée */
uint32_t demo_synth_ms(void) { return g_demo_ms; }
#endif

static float hit_env(int mask, float tau_ms)
{
    uint32_t ms = demo_synth_ms();
    int st = (int)(ms / DEMO_STEP_MS);
    for (int back = 0; back < 6 && st - back >= 0; back++) {
        int s = st - back;
        if (drums_at(s / 16, s % 16) & mask) {
            float dt = (float)(ms - (uint32_t)s * DEMO_STEP_MS);
            return expf(-dt / tau_ms);
        }
    }
    return 0;
}

float demo_synth_kick(void) { return hit_env(D_K, 110); }
float demo_synth_snare(void) { return hit_env(D_S, 90); }

#ifdef ESP_PLATFORM
#define CHUNK 256

static void synth_task(void *arg)
{
    int16_t buf[CHUNK];
    bool open = false;
    while (s_run) {
        if (!s_sound) {
            if (open) { sound_stream_write(NULL, CHUNK * 4, 0); sound_stream_close(); open = false; }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (!open) {
            if (sound_stream_open(DEMO_RATE) != ESP_OK) { s_sound = false; continue; }
            open = true;
            /* on se cale sur l'horloge de l'image */
            uint32_t target = (uint32_t)((uint64_t)demo_synth_ms() * DEMO_RATE / 1000);
            if (target < s_t || target - s_t > DEMO_RATE / 10) {
                reset();
                while (s_t < target) (void)sample();
            }
        }
        for (int i = 0; i < CHUNK; i++) buf[i] = sample();
        sound_stream_write(buf, CHUNK, 4096 * 6 / 10);
    }
    if (open) { sound_stream_write(NULL, CHUNK * 4, 0); sound_stream_close(); }
    s_task = false;
    vTaskDelete(NULL);
}

void demo_synth_start(bool sound)
{
    demo_synth_stop();
    reset();
    s_t0 = esp_timer_get_time();
    s_sound = sound;
    s_run = true;
    s_task = true;
    if (xTaskCreatePinnedToCore(synth_task, "demosnd", 4096, NULL, 6, NULL, 0) != pdPASS) s_task = false;
}

void demo_synth_stop(void)
{
    s_run = false;
    for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

void demo_synth_sound(bool on) { s_sound = on; }
#else
void demo_synth_start(bool sound) { (void)sound; reset(); g_demo_ms = 0; }
void demo_synth_stop(void) {}
void demo_synth_sound(bool on) { (void)on; }
#endif
