/*
 * puca_sfx.c — petits airs « 8 bits » du Púca (onde carrée, enveloppe courte).
 *
 * Une tâche brève par air : ouvre le flux I2S, joue la suite de notes, ferme.
 * Un air demandé pendant qu'un autre joue est ignoré (pas de file d'attente).
 */
#include "puca_sfx.h"
#include <stdbool.h>
#include "sound.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RATE  16000
#define CHUNK 160
#define AMP   7000                         /* doux : le haut-parleur est près de l'oreille */

typedef struct { uint16_t hz; uint16_t ms; } note_t;     /* hz 0 = silence ; ms 0 = fin */

/* notes */
#define C5 523
#define D5 587
#define E5 659
#define F5 698
#define G5 784
#define A5 880
#define B5 988
#define C6 1047
#define D6 1175
#define E6 1319
#define G6 1568
#define C4 262
#define G4 392
#define A4 440
#define E4 330

static const note_t S_TAP[]    = { { G5, 35 }, { 0, 0 } };
static const note_t S_HATCH[]  = { { C5, 90 }, { E5, 90 }, { G5, 90 }, { C6, 150 }, { 0, 40 }, { G5, 70 }, { C6, 220 }, { 0, 0 } };
static const note_t S_EAT[]    = { { E5, 45 }, { 0, 70 }, { E5, 45 }, { 0, 70 }, { G5, 60 }, { 0, 0 } };
static const note_t S_TREAT[]  = { { C6, 60 }, { E6, 60 }, { G6, 120 }, { 0, 0 } };
static const note_t S_HAPPY[]  = { { C5, 60 }, { E5, 60 }, { G5, 60 }, { C6, 110 }, { 0, 0 } };
static const note_t S_REFUSE[] = { { E4, 110 }, { 0, 40 }, { C4, 170 }, { 0, 0 } };
static const note_t S_CLEAN[]  = { { C5, 40 }, { D5, 40 }, { E5, 40 }, { F5, 40 }, { G5, 40 }, { A5, 40 }, { B5, 40 }, { C6, 80 }, { 0, 0 } };
static const note_t S_HEAL[]   = { { G5, 70 }, { 0, 30 }, { C6, 70 }, { 0, 30 }, { E6, 140 }, { 0, 0 } };
static const note_t S_EVOLVE[] = { { G4, 120 }, { C5, 120 }, { E5, 120 }, { G5, 120 }, { 0, 60 }, { E5, 90 }, { G5, 90 }, { C6, 300 }, { 0, 0 } };
static const note_t S_WIN[]    = { { E5, 70 }, { G5, 70 }, { E6, 70 }, { C6, 70 }, { D6, 70 }, { G6, 180 }, { 0, 0 } };
static const note_t S_LOSE[]   = { { G4, 120 }, { E4, 120 }, { C4, 240 }, { 0, 0 } };
static const note_t S_RIGHT[]  = { { C6, 50 }, { G6, 90 }, { 0, 0 } };
static const note_t S_WRONG[]  = { { A4, 140 }, { 0, 0 } };
static const note_t S_CLOVER[] = { { E6, 50 }, { G6, 50 }, { C6, 50 }, { G6, 120 }, { 0, 0 } };
static const note_t S_LIGHT[]  = { { G5, 40 }, { C5, 60 }, { 0, 0 } };

static const note_t *const TUNES[] = {
    [SFX_TAP] = S_TAP, [SFX_HATCH] = S_HATCH, [SFX_EAT] = S_EAT, [SFX_TREAT] = S_TREAT,
    [SFX_HAPPY] = S_HAPPY, [SFX_REFUSE] = S_REFUSE, [SFX_CLEAN] = S_CLEAN, [SFX_HEAL] = S_HEAL,
    [SFX_EVOLVE] = S_EVOLVE, [SFX_WIN] = S_WIN, [SFX_LOSE] = S_LOSE, [SFX_RIGHT] = S_RIGHT,
    [SFX_WRONG] = S_WRONG, [SFX_CLOVER] = S_CLOVER, [SFX_LIGHT] = S_LIGHT,
};

static volatile bool s_busy;

static void sfx_task(void *arg)
{
    const note_t *n = arg;
    int16_t buf[CHUNK];
    if (sound_stream_open(RATE) == ESP_OK) {
        uint32_t ph = 0;
        for (; n->ms; n++) {
            int len = RATE * n->ms / 1000;
            uint32_t step = n->hz ? (uint32_t)(((uint64_t)n->hz << 32) / RATE) : 0;
            for (int i = 0; i < len; i += CHUNK) {
                int k = len - i < CHUNK ? len - i : CHUNK;
                for (int j = 0; j < k; j++) {
                    int pos = i + j, env = AMP;
                    if (pos < 48) env = AMP * pos / 48;                       /* attaque 3 ms */
                    else if (len - pos < 160) env = AMP * (len - pos) / 160;  /* relâche 10 ms */
                    buf[j] = step ? (int16_t)((ph & 0x80000000u) ? env : -env) : 0;
                    ph += step;
                }
                sound_stream_write(buf, k, 4096);
            }
        }
        sound_stream_write(NULL, CHUNK * 4, 0);                               /* vide le DMA */
        sound_stream_close();
    }
    s_busy = false;
    vTaskDelete(NULL);
}

void puca_sfx(puca_sfx_t s)
{
    if (s_busy || s < 0 || s >= SFX_COUNT || !TUNES[s]) return;
    s_busy = true;
    if (xTaskCreatePinnedToCore(sfx_task, "pucasfx", 3072, (void *)TUNES[s], 4, NULL, 1) != pdPASS) s_busy = false;
}
