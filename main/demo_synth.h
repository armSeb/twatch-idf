/*
 * demo_synth.h — synthétiseur 4 voies de l'intro « Tír na nÓg » : lead
 * (carré 25 %), basse, arpège, batterie (grosse caisse, caisse claire,
 * charleston au bruit). Partition façon tracker, 125 BPM, 16 pas par mesure.
 * Tourne dans sa propre tâche (cœur 0) et sert d'horloge à l'image.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DEMO_RATE     16000
#define DEMO_STEP_MS  120                 /* une double croche à 125 BPM */
#define DEMO_BAR_MS   (16 * DEMO_STEP_MS) /* 1,92 s */
#define DEMO_LOOP_BAR 8                   /* après la mesure 15, on reprend à la 8 */

void     demo_synth_start(bool sound);    /* sound = false : horloge seule, sans son */
void     demo_synth_stop(void);
void     demo_synth_sound(bool on);
uint32_t demo_synth_ms(void);             /* position dans le morceau (ms, ne boucle pas) */
float    demo_synth_kick(void);           /* 1 au coup de grosse caisse, décroît vers 0 */
float    demo_synth_snare(void);
/* rendu hors temps réel (tests) : n échantillons à partir du début */
void     demo_synth_render(int16_t *out, int n);
