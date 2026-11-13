/*
 * audio_fx.h — correction de tonalité pour le petit haut-parleur (WebRX,
 * Radio) : 5 paliers, du neutre au « sourd ». Biquads RBJ en flottant.
 */
#pragma once
#include <stdint.h>

#define TONE_LEVELS 5

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } bq_t;
typedef struct { bq_t bq[5]; int n; } tone_fx_t;

void tone_fx_init(tone_fx_t *fx, int level, int rate);   /* remet l'état à zéro */
void tone_fx_run(tone_fx_t *fx, int16_t *x, int n);       /* sur place, saturé */
