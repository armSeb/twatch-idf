/* audio_fx.c — voir audio_fx.h */
#include "audio_fx.h"
#include <math.h>

/* Tonalité pour le petit haut-parleur, par paliers : passe-haut (le HP ne
 * rend pas le grave, autant garder la marge), bosse vers 600–800 Hz (corps
 * de la voix), passe-bas de plus en plus bas et raide (souffle FM, aigus
 * agressifs). Biquads RBJ, forme directe II transposée, en flottant (FPU). */

static void bq_set(bq_t *q, float b0, float b1, float b2, float a0, float a1, float a2)
{
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0; q->a1 = a1 / a0; q->a2 = a2 / a0;
    q->z1 = q->z2 = 0;
}

static void bq_hp(bq_t *q, float f, float Q, float rate)
{
    float w = 2 * (float)M_PI * f / rate, c = cosf(w), al = sinf(w) / (2 * Q);
    bq_set(q, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

static void bq_lp(bq_t *q, float f, float Q, float rate)
{
    float w = 2 * (float)M_PI * f / rate, c = cosf(w), al = sinf(w) / (2 * Q);
    bq_set(q, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

static void bq_peak(bq_t *q, float f, float Q, float db, float rate)
{
    float A = powf(10, db / 40), w = 2 * (float)M_PI * f / rate, c = cosf(w), al = sinf(w) / (2 * Q);
    bq_set(q, 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

/* palier → filtres. Passe-bas d'ordre 4 (deux étages de Butterworth,
 * Q 0,54 et 1,31) à partir du palier 3 */
void tone_fx_init(tone_fx_t *fx, int tone, int rate)
{
    fx->n = 0;
    bq_t *s_bq = fx->bq;
#define s_nbq fx->n
    switch (tone) {
    case 1:                                       /* claire */
        bq_hp(&s_bq[s_nbq++], 150, 0.7071f, rate);
        bq_peak(&s_bq[s_nbq++], 800, 0.8f, 3, rate);
        bq_lp(&s_bq[s_nbq++], 3200, 0.7071f, rate);
        break;
    case 2:                                       /* voix (0.22.0) */
        bq_hp(&s_bq[s_nbq++], 180, 0.7071f, rate);
        bq_peak(&s_bq[s_nbq++], 700, 0.8f, 5, rate);
        bq_lp(&s_bq[s_nbq++], 2600, 0.7071f, rate);
        break;
    case 3:                                       /* douce */
        bq_hp(&s_bq[s_nbq++], 180, 0.7071f, rate);
        bq_peak(&s_bq[s_nbq++], 650, 0.8f, 5, rate);
        bq_lp(&s_bq[s_nbq++], 2200, 0.5412f, rate);
        bq_lp(&s_bq[s_nbq++], 2200, 1.3066f, rate);
        break;
    case 4:                                       /* sourde */
        bq_hp(&s_bq[s_nbq++], 200, 0.7071f, rate);
        bq_peak(&s_bq[s_nbq++], 600, 0.8f, 6, rate);
        bq_lp(&s_bq[s_nbq++], 1700, 0.5412f, rate);
        bq_lp(&s_bq[s_nbq++], 1700, 1.3066f, rate);
        break;
    default:                                      /* neutre */
        break;
    }
#undef s_nbq
}

void tone_fx_run(tone_fx_t *fx, int16_t *x, int n)
{
    if (!fx->n) return;
    for (int i = 0; i < n; i++) {
        float v = x[i];
        for (int k = 0; k < fx->n; k++) {
            bq_t *q = &fx->bq[k];
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

