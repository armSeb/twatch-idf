/*
 * demo_synth.h — synthétiseur façon tracker de la Démo et du jeu de vol :
 * mélodie (carré 25 % avec écho), arpège, basse, nappe (3 triangles),
 * batterie (grosse caisse, caisse claire, charleston au bruit) et bruitages.
 * 125 BPM, 16 pas par mesure. Tourne dans sa propre tâche (cœur 0) et sert
 * d'horloge à l'image.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DEMO_RATE     16000
#define DEMO_STEP_MS  120                 /* une double croche à 125 BPM */
#define DEMO_BAR_MS   (16 * DEMO_STEP_MS) /* 1,92 s */
#define DEMO_BARS     64                  /* longueur du morceau de la démo */
#define DEMO_END_MS   (DEMO_BARS * DEMO_BAR_MS)

typedef enum { SONG_DEMO, SONG_GAME } demo_song_t;   /* démo : finit ; jeu : boucle */
typedef enum { SFX_RING = 1, SFX_CRASH, SFX_BOOST, SFX_TICK, SFX_OVER, SFX_GO } demo_sfx_t;

void     demo_synth_start(demo_song_t song, bool sound);
void     demo_synth_stop(void);
void     demo_synth_sound(bool on);       /* coupe / remet le son, le morceau continue */
void     demo_synth_sfx(demo_sfx_t s);
uint32_t demo_synth_ms(void);             /* position dans le morceau (ms) */
float    demo_synth_kick(void);           /* 1 au coup de grosse caisse, décroît vers 0 */
float    demo_synth_snare(void);
/* rendu hors temps réel (tests) : n échantillons à partir du début */
void     demo_synth_render(demo_song_t song, int16_t *out, int n);
