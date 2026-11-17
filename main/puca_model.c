/* puca_model.c — voir puca_model.h */
#include "puca_model.h"
#include <string.h>

#define TICK      300                     /* s */
#define MAX_GAP   (7 * 86400)
#define H         3600
#define NIGHT_FROM 22
#define NIGHT_TO   7

#define TEEN_AT   (24 * H)
#define ADULT_AT  (72 * H)
#define GONE_AFTER (48 * H)
#define SICK_HUNGRY (4 * H)
#define SICK_DIRTY  (3 * H)
#define POOP_EVERY  (3 * H)

static int clampi(int v) { return v < 0 ? 0 : v > PUCA_MAX ? PUCA_MAX : v; }
static void add(uint16_t *v, int d) { *v = (uint16_t)clampi((int)*v + d); }

uint32_t puca_rand(puca_t *p)
{
    uint32_t x = p->rng ? p->rng : 0x9E3779B9u;           /* xorshift32 */
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return p->rng = x;
}

void puca_new(puca_t *p)
{
    uint8_t snd = p->ver ? p->sound : 1;
    uint32_t r = p->rng;
    memset(p, 0, sizeof *p);
    p->ver = 1;
    p->stage = p->prev_stage = PS_EGG;
    p->sound = snd;
    p->rng = r ? r : 0x1234567u;
}

static int local_hour(time_t t, int32_t tz_min)
{
    int64_t l = (int64_t)t + (int64_t)tz_min * 60;
    int64_t s = l % 86400;
    if (s < 0) s += 86400;
    return (int)(s / 3600);
}

static bool night_at(time_t t, int32_t tz_min)
{
    int h = local_hour(t, tz_min);
    return h >= NIGHT_FROM || h < NIGHT_TO;
}

bool puca_asleep(const puca_t *p, time_t now, int32_t tz_min)
{
    return p->stage >= PS_BABY && p->stage <= PS_ADULT && night_at(now, tz_min);
}

uint32_t puca_age_h(const puca_t *p, time_t now)
{
    if (p->stage == PS_EGG || p->stage == PS_GONE || (uint32_t)now < p->born) return 0;
    return ((uint32_t)now - p->born) / H;
}

int puca_care_pct(const puca_t *p)
{
    return p->care_n ? (int)(p->care_sum / p->care_n / 10) : 100;
}

static void grow(puca_t *p, uint32_t t)
{
    uint32_t age = t - p->born;
    if (p->stage == PS_BABY && age >= TEEN_AT) {
        p->stage = PS_TEEN;
        p->events |= PE_EVOLVED;
    } else if (p->stage == PS_TEEN && age >= ADULT_AT) {
        uint32_t days = age / 86400 ? age / 86400 : 1;
        int care = puca_care_pct(p);
        p->form = care >= 60 ? (p->steps_total / days >= 5000 ? PF_HARE : PF_HORSE) : PF_GOAT;
        p->stage = PS_ADULT;
        p->events |= PE_EVOLVED;
    }
}

static void tick(puca_t *p, uint32_t t, int32_t tz_min)
{
    bool night = night_at(t, tz_min);
    bool was_night = night_at(t - TICK, tz_min);
    if (was_night && !night) p->light_off = 0;            /* réveil : lumière rallumée */

    if (night) {
        add(&p->hunger, -2);
        add(&p->energy, p->light_off ? 25 : 10);
        if (!p->light_off) add(&p->mood, -2);             /* dort mal à la lumière */
    } else {
        add(&p->hunger, -8);                               /* ~10 h de plein à vide */
        add(&p->energy, -3 - (p->sick ? 3 : 0));          /* ~28 h de plein à vide */
        add(&p->mood, -5 - 2 * p->poops - (p->sick ? 5 : 0) - (p->hunger == 0 ? 5 : 0));
        if (p->digest) {
            p->digest_s += TICK;
            if (p->digest_s >= POOP_EVERY) {
                p->digest_s = 0;
                p->digest--;
                if (p->poops < PUCA_POOPS) { p->poops++; p->events |= PE_POOP; }
            }
        }
        int clean = PUCA_MAX - 300 * p->poops;
        p->care_sum += (uint32_t)((p->hunger + p->mood + (clean < 0 ? 0 : clean) + (p->sick ? 0 : PUCA_MAX)) / 4);
        p->care_n++;
        grow(p, t);
    }
    p->hungry_s = p->hunger == 0 ? p->hungry_s + TICK : 0;
    p->dirty_s = p->poops >= 3 ? p->dirty_s + TICK : 0;
    if (!p->sick && (p->hungry_s >= SICK_HUNGRY || p->dirty_s >= SICK_DIRTY)) {
        p->sick = 1;
        p->events |= PE_SICK;
    }
    p->neglect_s = (p->hunger == 0 && p->mood == 0) ? p->neglect_s + TICK : 0;
    if (p->neglect_s >= GONE_AFTER) {
        p->stage = PS_GONE;
        p->events |= PE_GONE;
    }
}

void puca_update(puca_t *p, time_t now, int32_t tz_min, uint32_t steps, uint32_t day)
{
    uint32_t n = (uint32_t)now;
    if (p->stage == PS_EGG || p->stage == PS_GONE) { p->last = n; return; }
    if (n < p->last) { p->last = n; return; }              /* horloge reculée */
    if (n - p->last > MAX_GAP) p->last = n - MAX_GAP;
    while (p->last + TICK <= n && p->stage != PS_GONE) {
        p->last += TICK;
        tick(p, p->last, tz_min);
    }
    if (p->stage == PS_GONE) return;

    /* marche : humeur, et un trèfle par tranche de 1000 pas du jour */
    if (day != p->steps_day) { p->steps_day = day; p->steps_seen = 0; }
    if (steps > p->steps_seen) {
        uint32_t d = steps - p->steps_seen;
        uint32_t c = steps / 1000 - p->steps_seen / 1000;
        if (c) { p->clovers = (uint16_t)(p->clovers + c > 999 ? 999 : p->clovers + c); p->events |= PE_CLOVER; }
        add(&p->mood, (int)(d / 20 > 300 ? 300 : d / 20));
        p->steps_total += d;
        p->steps_seen = steps;
    }
}

bool puca_egg_tap(puca_t *p, time_t now)
{
    if (p->stage != PS_EGG) return false;
    if (++p->egg_taps < PUCA_HATCH_TAPS) return false;
    p->stage = p->prev_stage = PS_BABY;
    p->born = p->last = (uint32_t)now;
    p->hunger = 700; p->mood = 800; p->energy = PUCA_MAX;
    p->rng ^= (uint32_t)now;
    return true;
}

puca_res_t puca_feed(puca_t *p, time_t now, int32_t tz_min)
{
    if (puca_asleep(p, now, tz_min)) return PR_ASLEEP;
    if (p->hunger >= 900) return PR_FULL;
    add(&p->hunger, 350);
    add(&p->mood, 30);
    if (p->digest < 3) p->digest++;
    return PR_OK;
}

puca_res_t puca_treat(puca_t *p, time_t now, int32_t tz_min)
{
    if (puca_asleep(p, now, tz_min)) return PR_ASLEEP;
    if (!p->clovers) return PR_NO_CLOVER;
    if ((uint32_t)now - p->treat_t > 2 * H) p->treats = 0;
    if (p->treats >= 3) return PR_FULL;                   /* pas trop de gâteau */
    p->clovers--;
    p->treats++;
    p->treat_t = (uint32_t)now;
    add(&p->mood, 250);
    add(&p->hunger, 80);
    return PR_OK;
}

puca_res_t puca_play_ok(const puca_t *p, time_t now, int32_t tz_min)
{
    if (puca_asleep(p, now, tz_min)) return PR_ASLEEP;
    if (p->sick) return PR_SICK;
    if (p->energy < 100) return PR_TIRED;
    return PR_OK;
}

void puca_play_done(puca_t *p, int wins, int rounds)
{
    p->games++;
    add(&p->energy, -60);                                 /* une partie : 6 % */
    if (wins * 2 > rounds) {
        p->wins++;
        if (p->clovers < 999) p->clovers++;
        add(&p->mood, 300);
    } else {
        add(&p->mood, 100);
    }
}

puca_res_t puca_clean(puca_t *p)
{
    if (!p->poops) return PR_NOTHING;
    p->poops = 0;
    p->dirty_s = 0;
    add(&p->mood, 50);
    return PR_OK;
}

puca_res_t puca_heal(puca_t *p)
{
    if (!p->sick) return PR_NOTHING;
    p->sick = 0;
    p->hungry_s = p->dirty_s = 0;
    add(&p->mood, -30);                                   /* c'est amer */
    return PR_OK;
}

void puca_light(puca_t *p) { p->light_off = !p->light_off; }
