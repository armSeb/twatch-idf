/* demo_synth.c — voir demo_synth.h */
#include "demo_synth.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "sound.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* ------------------------------------------------------------------ partition */
enum { C_AM, C_F, C_C, C_G, C_EM, C_DM };
static const uint8_t CHORD[6][3] = { { 69, 72, 76 }, { 65, 69, 72 }, { 67, 72, 76 }, { 67, 71, 74 }, { 64, 67, 71 }, { 62, 65, 69 } };
static const uint8_t ROOT[6] = { 45, 41, 48, 43, 40, 50 };
static const uint8_t P_A[4] = { C_AM, C_F, C_C, C_G };      /* couplet */
static const uint8_t P_B[4] = { C_F, C_G, C_EM, C_AM };     /* refrain */
static const uint8_t P_D[4] = { C_DM, C_AM, C_F, C_G };     /* pont */

/* mélodies : 0 = on tient, 1 = silence, sinon note MIDI */
static const uint8_t LEAD_A[8][16] = {
    { 76, 0, 0, 74, 0, 0, 72, 0, 74, 0, 76, 0, 0, 0, 69, 0 },
    { 72, 0, 0, 71, 0, 0, 69, 0, 71, 0, 72, 0, 77, 0, 76, 0 },
    { 76, 0, 0, 74, 0, 0, 72, 0, 79, 0, 0, 0, 76, 0, 74, 0 },
    { 74, 0, 0, 0, 71, 0, 74, 0, 79, 0, 0, 0, 76, 0, 74, 0 },
    { 76, 0, 0, 74, 0, 0, 72, 0, 74, 0, 76, 0, 79, 0, 81, 0 },
    { 77, 0, 0, 76, 0, 0, 72, 0, 69, 0, 72, 0, 77, 0, 76, 0 },
    { 76, 0, 79, 0, 76, 0, 74, 0, 72, 0, 74, 0, 76, 0, 79, 0 },
    { 74, 0, 72, 0, 71, 0, 72, 0, 74, 0, 0, 0, 69, 0, 0, 1 },
};
static const uint8_t LEAD_B[8][16] = {
    { 81, 0, 0, 79, 0, 0, 77, 0, 76, 0, 77, 0, 0, 0, 72, 0 },
    { 74, 0, 0, 76, 0, 0, 79, 0, 0, 0, 83, 0, 81, 0, 79, 0 },
    { 79, 0, 0, 0, 76, 0, 0, 0, 71, 0, 74, 0, 76, 0, 0, 0 },
    { 72, 0, 0, 0, 76, 0, 0, 0, 81, 0, 0, 0, 1, 0, 0, 0 },
    { 81, 0, 0, 79, 0, 0, 77, 0, 76, 0, 77, 0, 81, 0, 84, 0 },
    { 83, 0, 0, 0, 79, 0, 0, 0, 74, 0, 76, 0, 79, 0, 0, 0 },
    { 76, 0, 79, 0, 83, 0, 79, 0, 76, 0, 74, 0, 71, 0, 74, 0 },
    { 76, 0, 0, 0, 0, 0, 0, 0, 72, 0, 0, 0, 69, 0, 1, 0 },
};

/* Envol : mélodie lente et clairsemée, sur couplet puis refrain */
static const uint8_t LEAD_G[8][16] = {
    { 76, 0, 0, 0, 0, 0, 0, 0, 72, 0, 0, 0, 74, 0, 0, 0 },
    { 72, 0, 0, 0, 0, 0, 0, 0, 69, 0, 0, 0, 0, 0, 0, 0 },
    { 67, 0, 0, 0, 72, 0, 0, 0, 76, 0, 0, 0, 0, 0, 0, 0 },
    { 74, 0, 0, 0, 0, 0, 0, 0, 71, 0, 0, 0, 0, 0, 1, 0 },
    { 69, 0, 0, 0, 72, 0, 0, 0, 77, 0, 0, 0, 76, 0, 0, 0 },
    { 74, 0, 0, 0, 0, 0, 0, 0, 71, 0, 0, 0, 74, 0, 0, 0 },
    { 71, 0, 0, 0, 0, 0, 0, 0, 67, 0, 0, 0, 0, 0, 0, 0 },
    { 69, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 },
};

enum { D_K = 1, D_S = 2, D_H = 4 };
enum { DR_NONE, DR_BUILD, DR_BUILDROLL, DR_FULL, DR_FILL, DR_HALF, DR_FOUR, DR_ROLL, DR_END, DR_SOFT };
static const uint8_t DRUMS[10][16] = {
    { 0 },
    { 0, 0, D_H, 0, 0, 0, D_H, 0, 0, 0, D_H, 0, 0, 0, D_H, D_H },
    { 0, 0, D_H, 0, 0, 0, D_H, 0, 0, 0, D_H, 0, D_S, D_S, D_S, D_S },
    { D_K, 0, D_H, 0, D_S, 0, D_H, D_K, D_K, 0, D_H, 0, D_S, 0, D_H, D_H },
    { D_K, 0, D_H, 0, D_S, 0, D_H, D_K, D_K, 0, D_S, D_S, D_S, D_S, D_S, D_S },
    { D_K, 0, D_H, 0, 0, 0, D_H, 0, 0, 0, D_H, 0, 0, 0, D_H, 0 },
    { D_K, 0, D_H, 0, D_K, 0, D_H, 0, D_K, 0, D_H, 0, D_K, 0, D_H, D_H },
    { D_K, 0, D_S, 0, D_K, 0, D_S, 0, D_K | D_S, 0, D_S, D_S, D_S, D_S, D_S, D_S },
    { D_K, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { D_K, 0, 0, 0, D_H, 0, 0, 0, 0, 0, 0, 0, D_H, 0, 0, 0 },
};

enum { F_ARP = 1, F_BASS = 2, F_PAD = 4, F_ARPHI = 8, F_LEADHI = 16, F_SOFT = 32 };
typedef struct { uint8_t chord, lead, drums, flags; } bar_t;   /* lead : 0, 1..8 = A, 9..16 = B, 17..24 = jeu */
#define GAME_BARS 16
static bar_t ARR[DEMO_BARS + GAME_BARS];      /* la démo, puis la boucle d'Envol */

static void arr_set(int from, int n, const uint8_t *prog, int lead, int drums, int flags)
{
    for (int i = 0; i < n; i++) {
        bar_t *b = &ARR[from + i];
        b->chord = prog[i & 3];
        b->lead = (uint8_t)(lead ? lead + i : 0);
        b->drums = (uint8_t)drums;
        b->flags = (uint8_t)flags;
    }
}

static void arr_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    arr_set(0, 4, P_A, 0, DR_NONE, F_ARP);                                   /* intro */
    arr_set(4, 4, P_A, 0, DR_BUILD, F_ARP | F_BASS);
    ARR[7].drums = DR_BUILDROLL;
    arr_set(8, 8, P_A, 1, DR_FULL, F_ARP | F_BASS | F_ARPHI);                /* couplet */
    ARR[15].drums = DR_FILL;
    arr_set(16, 8, P_B, 9, DR_FULL, F_ARP | F_BASS | F_ARPHI);               /* refrain */
    ARR[23].drums = DR_FILL;
    arr_set(24, 4, P_D, 0, DR_NONE, F_PAD | F_ARP);                          /* pont */
    ARR[26].drums = ARR[27].drums = DR_HALF;
    arr_set(28, 4, P_D, 0, DR_FOUR, F_PAD | F_ARP | F_BASS);
    ARR[31].drums = DR_ROLL;
    arr_set(32, 8, P_A, 1, DR_FULL, F_ARP | F_BASS | F_ARPHI | F_PAD);       /* couplet 2 */
    ARR[39].drums = DR_FILL;
    arr_set(40, 8, P_B, 9, DR_FULL, F_ARP | F_BASS | F_ARPHI);               /* refrain 2 */
    ARR[47].drums = DR_FILL;
    arr_set(48, 8, P_A, 1, DR_FULL, F_ARP | F_BASS | F_ARPHI | F_PAD | F_LEADHI);   /* final */
    ARR[55].drums = DR_FILL;
    arr_set(56, 4, P_A, 1, DR_HALF, F_ARP | F_PAD | F_BASS);                 /* fin */
    arr_set(60, 4, P_A, 0, DR_NONE, F_ARP | F_PAD);
    ARR[60].drums = DR_END;
    ARR[63].chord = C_AM;
    /* Envol : boîte à musique, nappe, basse douce ; la mélodie à la 2e moitié */
    const int G = DEMO_BARS, soft = F_SOFT | F_PAD | F_ARP | F_BASS;
    arr_set(G, 4, P_A, 0, DR_SOFT, soft);
    arr_set(G + 4, 4, P_B, 0, DR_SOFT, soft);
    arr_set(G + 8, 4, P_A, 17, DR_SOFT, soft);
    arr_set(G + 12, 4, P_B, 21, DR_SOFT, soft);
}

static demo_song_t s_song;

/* mesure jouée (indice dans ARR), -1 = silence */
static int song_bar(int bar)
{
    if (s_song == SONG_DEMO) return bar < DEMO_BARS ? bar : -1;
    return DEMO_BARS + bar % GAME_BARS;
}

static uint8_t drums_at(int bar, int step)
{
    int b = song_bar(bar);
    return b < 0 ? 0 : DRUMS[ARR[b].drums][step];
}

/* ------------------------------------------------------------------ voix */
typedef struct {
    uint32_t ph, inc;
    int32_t env;            /* Q15 */
    int32_t decay;          /* multiplicateur Q15 par échantillon */
} voice_t;

static voice_t v_lead, v_bass, v_arp;
static uint32_t v_pad_ph[3], v_pad_inc[3];
static int32_t v_pad_env, v_pad_target;
static uint32_t s_t;                  /* échantillons depuis le début */
static uint32_t k_t = 1 << 30, s_tt = 1 << 30, h_t = 1 << 30;
static float k_ph;
static uint32_t lfsr = 0xACE1u;
static int32_t hat_prev;
static int s_master = 32767;         /* Q15, fondu de la fin */
static bool s_soft;                  /* mesure douce : triangles, batterie feutrée */

#define ECHO_N (DEMO_RATE * 3 * DEMO_STEP_MS / 1000)   /* écho à 3 pas */
static int16_t *s_echo;
static int s_echo_i;

static volatile int s_sfx_req;
static int s_sfx, s_sfx_t;
static uint32_t s_sfx_ph;

static uint32_t note_inc(int midi)
{
    float f = 440.0f * powf(2.0f, (midi - 69) / 12.0f);
    return (uint32_t)(f * 4294967296.0f / DEMO_RATE);
}

static int32_t decay_q15(float ms) { return (int32_t)(32768.0f * expf(-1000.0f / (ms * DEMO_RATE))); }

static void on_step(int bar, int step)
{
    int b = song_bar(bar);
    if (b < 0) { v_pad_target = 0; return; }
    const bar_t *a = &ARR[b];
    const uint8_t *ch = CHORD[a->chord];
    s_soft = a->flags & F_SOFT;
    if (s_soft) {
        if ((a->flags & F_ARP) && !(step & 1)) {             /* boîte à musique, croches */
            v_arp.inc = note_inc(ch[(step >> 1) % 3] + 12);
            v_arp.env = 16000;
            v_arp.decay = decay_q15(260);
        }
        if ((a->flags & F_BASS) && (step == 0 || step == 8)) {
            v_bass.inc = note_inc(ROOT[a->chord]);
            v_bass.env = 26000;
            v_bass.decay = decay_q15(700);
        }
    } else if (a->flags & F_ARP) {
        v_arp.inc = note_inc(ch[step % 3] + ((a->flags & F_ARPHI) && (step & 4) ? 12 : 0));
        v_arp.env = (a->flags & F_PAD) && !(a->flags & F_BASS) ? 20000 : 32767;
        v_arp.decay = decay_q15(90);
    }
    if (!s_soft && (a->flags & F_BASS) && !(step & 1)) {
        v_bass.inc = note_inc(ROOT[a->chord] + ((step & 2) ? 12 : 0));
        v_bass.env = 32767;
        v_bass.decay = decay_q15(140);
    }
    if (step == 0) {
        v_pad_target = (a->flags & F_PAD) ? 32767 : 0;
        for (int i = 0; i < 3; i++) v_pad_inc[i] = note_inc(ch[i] - 12) + (uint32_t)(i * 9000);   /* léger désaccord */
    }
    if (a->lead) {
        const uint8_t *row = a->lead <= 8 ? LEAD_A[a->lead - 1] : a->lead <= 16 ? LEAD_B[a->lead - 9] : LEAD_G[a->lead - 17];
        uint8_t n = row[step];
        if (n == 1) v_lead.env = 0;
        else if (n) {
            v_lead.inc = note_inc(n + ((a->flags & F_LEADHI) ? 12 : 0));
            v_lead.env = s_soft ? 24000 : 32767;
            v_lead.decay = decay_q15(s_soft ? 900 : 420);
        }
    }
    uint8_t d = DRUMS[a->drums][step];
    if (d & D_K) { k_t = 0; k_ph = 0; }
    if (d & D_S) s_tt = 0;
    if (d & D_H) h_t = 0;
    /* fondu des 4 dernières mesures de la démo */
    if (s_song == SONG_DEMO && b >= 60) s_master = 32767 - (int)(((b - 60) * 16 + step) * 32767 / 64) + 2000;
    else s_master = s_song == SONG_GAME ? 21000 : 32767;      /* le jeu : en fond */
}

static inline uint32_t noise(void)
{
    lfsr ^= lfsr << 13; lfsr ^= lfsr >> 17; lfsr ^= lfsr << 5;
    return lfsr;
}

static int32_t sfx_sample(void)
{
    if (!s_sfx) return 0;
    int t = s_sfx_t++;
    float ts = t / (float)DEMO_RATE;
    int32_t v = 0;
    switch (s_sfx) {
    case SFX_RING: {                                       /* arpège montant scintillant */
        static const uint8_t N[] = { 84, 88, 91, 96 };
        int k = t / (DEMO_RATE / 22);
        if (k >= 4) { s_sfx = 0; break; }
        s_sfx_ph += note_inc(N[k]);
        v = ((s_sfx_ph >> 30) == 0 ? 3800 : -1300) * (1 - (t % (DEMO_RATE / 22)) / (float)(DEMO_RATE / 22) * 0.5f);
        break;
    }
    case SFX_GO: {
        static const uint8_t N[] = { 72, 79, 84 };
        int k = t / (DEMO_RATE / 8);
        if (k >= 3) { s_sfx = 0; break; }
        s_sfx_ph += note_inc(N[k]);
        v = (s_sfx_ph >> 31) ? 3000 : -3000;
        break;
    }
    case SFX_CRASH:                                        /* bruit grave qui s'éteint */
        if (ts > 0.9f) { s_sfx = 0; break; }
        {
            static int32_t lp;
            int32_t n = (int32_t)(noise() & 0xFFFF) - 32768;
            lp += (n - lp) >> 3;
            v = (int32_t)(lp * 0.5f * expf(-ts * 4));
        }
        break;
    case SFX_BOOST:                                        /* sifflement qui monte */
        if (ts > 0.45f) { s_sfx = 0; break; }
        s_sfx_ph += (uint32_t)((300 + 1600 * ts / 0.45f) * 4294967296.0f / DEMO_RATE);
        v = (int32_t)(((s_sfx_ph >> 31) ? 1800 : -1800) * (1 - ts / 0.45f));
        v += (int32_t)(((int32_t)(noise() & 0xFFF) - 2048) * (1 - ts / 0.45f));
        break;
    case SFX_TICK:
        if (t > DEMO_RATE / 25) { s_sfx = 0; break; }
        s_sfx_ph += note_inc(93);
        v = (s_sfx_ph >> 31) ? 2200 : -2200;
        break;
    case SFX_OVER: {                                       /* trois notes qui descendent */
        static const uint8_t N[] = { 72, 67, 63, 60 };
        int k = t / (DEMO_RATE / 5);
        if (k >= 4) { s_sfx = 0; break; }
        s_sfx_ph += note_inc(N[k]);
        v = (int32_t)(((s_sfx_ph >> 30) == 0 ? 3200 : -1100) * (k == 3 ? 1 - (t % (DEMO_RATE / 5)) / (float)(DEMO_RATE / 5) : 1));
        break;
    }
    default: s_sfx = 0;
    }
    return v;
}

static inline int32_t tri(uint32_t ph)                  /* triangle, ±16384 */
{
    int32_t x = (int32_t)(ph >> 16) - 32768;
    return (x < 0 ? -x : x) - 16384;
}

static int16_t sample(void)
{
    uint32_t step_len = DEMO_RATE * DEMO_STEP_MS / 1000;
    if (s_t % step_len == 0) {
        uint32_t st = s_t / step_len;
        on_step((int)(st / 16), (int)(st % 16));
    }
    s_t++;
    if (s_sfx_req) { s_sfx = s_sfx_req; s_sfx_req = 0; s_sfx_t = 0; s_sfx_ph = 0; }
    int32_t acc = 0, lead = 0;
    if (v_lead.env > 40) {                                 /* mélodie : carré 25 % */
        lead = s_soft ? (tri(v_lead.ph) * 3 / 16) * v_lead.env >> 15
                      : ((v_lead.ph >> 30) == 0 ? 3800 : -1270) * v_lead.env >> 15;
        v_lead.ph += v_lead.inc;
        v_lead.env = v_lead.env * v_lead.decay >> 15;
        if (v_lead.env < 13000 && v_lead.decay < 32760) v_lead.decay = decay_q15(1400);   /* tenue */
    }
    if (s_echo) {                                          /* écho de la mélodie */
        int16_t e = s_echo[s_echo_i];
        s_echo[s_echo_i] = (int16_t)(lead + (e * 3 >> 3));
        s_echo_i = (s_echo_i + 1) % ECHO_N;
        acc += lead + (e * 3 >> 3);
    } else acc += lead;
    if (v_arp.env > 40) {                                  /* arpège : carré 50 % */
        acc += s_soft ? (tri(v_arp.ph) / 12) * v_arp.env >> 15
                      : ((int32_t)(v_arp.ph >> 31) ? 1400 : -1400) * v_arp.env >> 15;
        v_arp.ph += v_arp.inc;
        v_arp.env = v_arp.env * v_arp.decay >> 15;
    }
    if (v_bass.env > 40) {                                 /* basse : carré 50 % */
        acc += s_soft ? (tri(v_bass.ph) / 4) * v_bass.env >> 15
                      : ((int32_t)(v_bass.ph >> 31) ? 3400 : -3400) * v_bass.env >> 15;
        v_bass.ph += v_bass.inc;
        v_bass.env = v_bass.env * v_bass.decay >> 15;
    }
    v_pad_env += (v_pad_target - v_pad_env) >> 11;         /* nappe : montée lente */
    if (v_pad_env > 64) {
        int32_t p = 0;
        for (int i = 0; i < 3; i++) {
            int32_t x = (int32_t)(v_pad_ph[i] >> 16) - 32768;        /* triangle */
            p += (x < 0 ? -x : x) - 16384;
            v_pad_ph[i] += v_pad_inc[i];
        }
        acc += (p / 24) * v_pad_env >> 15;
    }
    if (k_t < DEMO_RATE / 4) {                             /* grosse caisse */
        float t = k_t / (float)DEMO_RATE;
        float f = 48.0f + 140.0f * expf(-t * 35.0f);
        k_ph += f / DEMO_RATE;
        if (k_ph > 1) k_ph -= 1;
        acc += (int32_t)((s_soft ? 3800.0f : 9000.0f) * expf(-t * 9.0f) * sinf(k_ph * 6.2831853f));
        k_t++;
    }
    if (s_tt < DEMO_RATE / 5) {                            /* caisse claire */
        float t = s_tt / (float)DEMO_RATE;
        int32_t n = (int32_t)(noise() & 0xFFFF) - 32768;
        acc += (int32_t)(n * 0.11f * expf(-t * 22.0f));
        s_tt++;
    }
    if (h_t < DEMO_RATE / 20) {                            /* charleston */
        float t = h_t / (float)DEMO_RATE;
        int32_t n = (int32_t)(noise() & 0xFFFF) - 32768;
        int32_t hp = n - hat_prev;
        hat_prev = n;
        acc += (int32_t)(hp * (s_soft ? 0.012f : 0.03f) * expf(-t * 90.0f));
        h_t++;
    }
    acc = (acc * (s_master < 0 ? 0 : s_master)) >> 15;
    acc += sfx_sample();
    if (acc > 32767) acc = 32767;
    if (acc < -32768) acc = -32768;
    return (int16_t)acc;
}

static void reset(void)
{
    arr_init();
    memset(&v_lead, 0, sizeof v_lead);
    memset(&v_bass, 0, sizeof v_bass);
    memset(&v_arp, 0, sizeof v_arp);
    memset(v_pad_ph, 0, sizeof v_pad_ph);
    v_pad_env = v_pad_target = 0;
    s_t = 0;
    k_t = s_tt = h_t = 1 << 30;
    s_master = 32767;
    s_sfx = 0;
    if (s_echo) memset(s_echo, 0, ECHO_N * sizeof(int16_t));
    s_echo_i = 0;
}

void demo_synth_render(demo_song_t song, int16_t *out, int n)
{
    s_song = song;
    if (!s_echo) s_echo = calloc(ECHO_N, sizeof(int16_t));
    reset();
    for (int i = 0; i < n; i++) out[i] = sample();
}

void demo_synth_sfx(demo_sfx_t s) { s_sfx_req = s; }

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
    arr_init();
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

/* Le flux I2S reste ouvert du début à la fin : couper le son n'envoie que du
 * silence, le morceau continue et reste calé sur l'image. */
static void synth_task(void *arg)
{
    int16_t buf[CHUNK];
    bool open = sound_stream_open(DEMO_RATE) == ESP_OK;
    while (s_run) {
        /* rattrape l'horloge si la tâche a pris du retard (écran, Wi-Fi…) */
        uint32_t target = (uint32_t)((uint64_t)demo_synth_ms() * DEMO_RATE / 1000);
        while (s_t + DEMO_RATE / 5 < target && s_run) (void)sample();
        for (int i = 0; i < CHUNK; i++) buf[i] = sample();
        if (open) sound_stream_write(buf, CHUNK, s_sound ? 4096 * 6 / 10 : 0);
        else {
            vTaskDelay(pdMS_TO_TICKS(CHUNK * 1000 / DEMO_RATE));
            if (s_sound) open = sound_stream_open(DEMO_RATE) == ESP_OK;
        }
    }
    if (open) { sound_stream_write(NULL, CHUNK * 4, 0); sound_stream_close(); }
    s_task = false;
    vTaskDelete(NULL);
}

void demo_synth_start(demo_song_t song, bool sound)
{
    demo_synth_stop();
    s_song = song;
    if (!s_echo) s_echo = heap_caps_malloc(ECHO_N * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    if (!s_task && s_echo) { heap_caps_free(s_echo); s_echo = NULL; }
}

void demo_synth_sound(bool on) { s_sound = on; }
#else
void demo_synth_start(demo_song_t song, bool sound) { (void)sound; s_song = song; reset(); g_demo_ms = 0; }
void demo_synth_stop(void) {}
void demo_synth_sound(bool on) { (void)on; }
#endif
