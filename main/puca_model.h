/*
 * puca_model.h — vie du Púca (sans LVGL, testable sur PC).
 *
 * Rien ne tourne en tâche de fond : l'état est recalculé d'un bloc à chaque
 * ouverture de l'app, par pas de 5 minutes depuis la dernière visite (au plus
 * 7 jours). Les jauges vont de 0 à 1000.
 *
 *   œuf ──(6 touchers)──► bébé ──24 h──► jeune ──72 h──► adulte
 *                                                          lièvre  : bien soigné et grand marcheur
 *                                                          cheval  : bien soigné
 *                                                          bouc    : soins négligés
 *
 * Il dort de 22 h à 7 h (heure locale). Laissé à l'abandon (affamé et
 * malheureux) 48 h d'affilée, il retourne au tertre des fées : nouvel œuf.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

enum { PS_EGG = 0, PS_BABY, PS_TEEN, PS_ADULT, PS_GONE };
enum { PF_NONE = 0, PF_HARE, PF_HORSE, PF_GOAT };

/* événements à mettre en scène (puca_t.events, effacés par la vue) */
#define PE_EVOLVED  0x01        /* changement de stade pendant l'absence */
#define PE_GONE     0x02
#define PE_POOP     0x04        /* nouvelle crotte */
#define PE_SICK     0x08        /* vient de tomber malade */
#define PE_CLOVER   0x10        /* trèfles gagnés en marchant */

#define PUCA_MAX    1000
#define PUCA_POOPS  4
#define PUCA_HATCH_TAPS 6

typedef struct {
    uint8_t  ver;
    uint8_t  stage, form, prev_stage;
    uint8_t  poops, sick, light_off, egg_taps;
    uint8_t  digest, treats, sound, events;
    uint16_t hunger, mood, energy;      /* satiété, humeur, énergie */
    uint16_t clovers;                   /* trèfles (pas, jeux) : friandises */
    uint16_t games, wins;
    uint32_t born, last;                /* UTC : éclosion, dernière mise à jour */
    uint32_t hungry_s, dirty_s, neglect_s, digest_s, treat_t;
    uint32_t care_sum, care_n;
    uint32_t steps_day, steps_seen, steps_total;
    uint32_t rng;
} puca_t;

typedef enum {
    PR_OK = 0,
    PR_ASLEEP,      /* il dort */
    PR_FULL,        /* pas faim */
    PR_NO_CLOVER,   /* pas de trèfle */
    PR_TIRED,       /* trop fatigué pour jouer */
    PR_SICK,        /* malade : pas de jeu */
    PR_NOTHING,     /* rien à faire */
} puca_res_t;

void       puca_new(puca_t *p);                          /* œuf neuf */
/* avance l'état jusqu'à now ; tz = décalage local (min) ; steps = pas du jour
 * (0 si compteur coupé) ; day = numéro du jour local */
void       puca_update(puca_t *p, time_t now, int32_t tz_min, uint32_t steps, uint32_t day);
bool       puca_asleep(const puca_t *p, time_t now, int32_t tz_min);
uint32_t   puca_age_h(const puca_t *p, time_t now);
bool       puca_egg_tap(puca_t *p, time_t now);           /* true = éclosion */
puca_res_t puca_feed(puca_t *p, time_t now, int32_t tz_min);
puca_res_t puca_treat(puca_t *p, time_t now, int32_t tz_min);
puca_res_t puca_play_ok(const puca_t *p, time_t now, int32_t tz_min);
void       puca_play_done(puca_t *p, int wins, int rounds);
puca_res_t puca_clean(puca_t *p);
puca_res_t puca_heal(puca_t *p);
void       puca_light(puca_t *p);
uint32_t   puca_rand(puca_t *p);
int        puca_care_pct(const puca_t *p);               /* moyenne des soins, % */
