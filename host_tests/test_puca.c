/* Tests PC de la vie du Púca (puca_model.c) */
#include <assert.h>
#include <stdio.h>
#include "puca_model.h"

#define H 3600
#define D 86400
static const int32_t TZ = 60;                       /* Paris, heure d'hiver */
static time_t at(int day, int hour) { return 1767225600 + day * D + hour * H - TZ * 60; }   /* 01/01/2026 local */

static void care(puca_t *p, time_t t)               /* une visite attentionnée */
{
    puca_update(p, t, TZ, 0, 0);
    while (puca_feed(p, t, TZ) == PR_OK) {}
    puca_clean(p);
    puca_heal(p);
    if (puca_play_ok(p, t, TZ) == PR_OK) puca_play_done(p, 4, 5);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    puca_t p = { 0 };
    puca_new(&p);
    assert(p.stage == PS_EGG);
    /* l'œuf n'évolue pas tout seul */
    puca_update(&p, at(5, 12), TZ, 5000, 5);
    assert(p.stage == PS_EGG && p.clovers == 0);
    for (int i = 0; i < PUCA_HATCH_TAPS - 1; i++) assert(!puca_egg_tap(&p, at(0, 9)));
    assert(puca_egg_tap(&p, at(0, 9)) && p.stage == PS_BABY);
    for (int h = 12; h <= 21; h += 3) care(&p, at(0, h));

    /* la nuit il dort : pas de repas, la lumière éteinte le repose mieux */
    puca_update(&p, at(0, 23), TZ, 0, 0);
    assert(puca_asleep(&p, at(0, 23), TZ));
    assert(puca_feed(&p, at(0, 23), TZ) == PR_ASLEEP);
    puca_light(&p);
    uint16_t e = p.energy;
    puca_update(&p, at(1, 6), TZ, 0, 1);
    assert(p.energy > e && p.light_off);
    puca_update(&p, at(1, 8), TZ, 0, 1);
    assert(!p.light_off);                                /* rallumée au réveil */

    /* repas : satiété, digestion → crotte trois heures plus tard */
    puca_clean(&p);
    p.digest = 0; p.digest_s = 0; p.events = 0;
    p.hunger = 500;
    assert(puca_feed(&p, at(1, 8), TZ) == PR_OK && p.digest == 1);
    puca_update(&p, at(1, 11) + 600, TZ, 0, 1);
    assert(p.poops == 1 && (p.events & PE_POOP));
    assert(puca_clean(&p) == PR_OK && p.poops == 0);
    assert(puca_feed(&p, at(1, 11), TZ) == PR_OK);

    /* marche : un trèfle par 1000 pas du jour */
    uint16_t c0 = p.clovers;
    puca_update(&p, at(1, 12), TZ, 2500, 1);
    assert(p.clovers == c0 + 2);
    puca_update(&p, at(1, 12) + 300, TZ, 3100, 1);
    assert(p.clovers == c0 + 3);
    puca_update(&p, at(2, 12), TZ, 400, 2);            /* nouveau jour : compteur reparti */
    assert(p.clovers == c0 + 3 && p.steps_seen == 400);
    assert(puca_treat(&p, at(2, 12), TZ) == PR_OK && p.clovers == c0 + 2);

    /* soigné régulièrement : jeune à 24 h, adulte à 72 h, forme selon les soins */
    for (int d = 2; d <= 4; d++) for (int h = 8; h <= 20; h += 3) care(&p, at(d, h));
    assert(p.stage == PS_ADULT && p.form == PF_HORSE);
    printf("adulte soigné : %s, soins %d %%\n", p.form == PF_HORSE ? "cheval" : "?", puca_care_pct(&p));

    /* un grand marcheur bien soigné devient lièvre */
    puca_t q = { 0 };
    puca_new(&q);
    for (int i = 0; i < PUCA_HATCH_TAPS; i++) puca_egg_tap(&q, at(0, 9));
    for (int d = 0; d <= 3; d++) for (int h = 9; h <= 21; h += 3) { care(&q, at(d, h)); puca_update(&q, at(d, h), TZ, (uint32_t)(h - 8) * 700, (uint32_t)d); }
    assert(q.stage == PS_ADULT && q.form == PF_HARE);

    /* négligé : malade, puis bouc, puis parti au bout de 48 h d'abandon */
    puca_t n = { 0 };
    puca_new(&n);
    for (int i = 0; i < PUCA_HATCH_TAPS; i++) puca_egg_tap(&n, at(0, 9));
    puca_update(&n, at(1, 15), TZ, 0, 1);
    assert(n.sick && (n.events & PE_SICK));
    assert(puca_play_ok(&n, at(1, 15), TZ) == PR_SICK);
    assert(puca_heal(&n) == PR_OK && !n.sick);
    for (int d = 1; d <= 3; d++) { puca_update(&n, at(d, 12), TZ, 0, (uint32_t)d); puca_feed(&n, at(d, 12), TZ); }
    puca_update(&n, at(3, 20), TZ, 0, 3);
    assert(n.stage == PS_ADULT && n.form == PF_GOAT);
    puca_update(&n, at(9, 12), TZ, 0, 9);
    assert(n.stage == PS_GONE && (n.events & PE_GONE));
    puca_new(&n);
    assert(n.stage == PS_EGG);

    /* horloge reculée, très longue absence : pas de boucle infinie */
    puca_t r = q;
    puca_update(&r, at(1, 0), TZ, 0, 1);
    puca_update(&r, at(400, 12), TZ, 0, 400);
    assert(r.stage == PS_GONE);
    printf("Púca : OK (taille de l'état %u o)\n", (unsigned)sizeof(puca_t));
    return 0;
}
