/*
 * sound.h — bips sur le haut-parleur (ampli I2S MAX98357, V1 : BCLK 26,
 * LRCLK 25, DIN 33). Le périphérique I2S n'existe que le temps du son : rien
 * ne consomme ni ne bloque la fréquence CPU au repos.
 */
#pragma once
#include <stdbool.h>

/* n bips de freq_hz, on_ms / off_ms ; ignoré si un son est déjà en cours.
 * Non bloquant, appelable depuis la tâche app. */
void sound_beeps(int n, int freq_hz, int on_ms, int off_ms);
bool sound_busy(void);
