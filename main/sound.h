/*
 * sound.h — bips sur le haut-parleur (ampli I2S MAX98357, V1 : BCLK 26,
 * LRCLK 25, DIN 33). Le périphérique I2S n'existe que le temps du son : rien
 * ne consomme ni ne bloque la fréquence CPU au repos.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* n bips de freq_hz, on_ms / off_ms ; ignoré si un son est déjà en cours.
 * Non bloquant, appelable depuis la tâche app. */
void sound_beeps(int n, int freq_hz, int on_ms, int off_ms);
bool sound_busy(void);
int  sound_last_error(void);       /* 0 = dernier son joué, sinon esp_err_t */

/* Flux audio continu (une seule tâche écrivain). open : ampli + I2S à `rate`
 * Hz ; write : échantillons mono 16 bits, gain en Q12 (4096 = ×1), bloquant
 * au rythme du son ; mono NULL = silence. Les bips sont ignorés pendant un flux. */
esp_err_t sound_stream_open(int rate);
void      sound_stream_write(const int16_t *mono, int n, int gain_q12);
void      sound_stream_close(void);
