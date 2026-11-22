/*
 * apps.c — registre des apps (ordre du lanceur).
 * Ajouter une app : écrire app_xxx.c avec un `const app_t app_xxx`, puis
 * l'ajouter ici. Les entrées système (Lecture, Énergie, Réglages) sont
 * ajoutées par le lanceur lui-même (ui.c).
 */
#include "app.h"

extern const app_t app_chrono;
extern const app_t app_timer;
extern const app_t app_lamp;
extern const app_t app_steps;
extern const app_t app_puca;
extern const app_t app_demo;
extern const app_t app_flight;
extern const app_t app_remote;
extern const app_t app_mouse;
extern const app_t app_rfgen;
extern const app_t app_wifi;
extern const app_t app_sdr;
extern const app_t app_webrx;
extern const app_t app_tone;
extern const app_t app_radio;
#ifndef TW_NO_TVOFF
extern const app_t app_tvoff;          /* codes TV-B-Gone, CC BY-SA 4.0 */
#endif

const app_t *const g_apps[] = {
    &app_chrono,
    &app_timer,
    &app_steps,
    &app_puca,
    &app_demo,
    &app_flight,
    &app_lamp,
    &app_remote,
    &app_mouse,
#ifndef TW_NO_TVOFF
    &app_tvoff,
#endif
    &app_wifi,
    &app_sdr,
    &app_radio,
    &app_webrx,
    &app_tone,
    &app_rfgen,
};
const int g_app_count = sizeof(g_apps) / sizeof(g_apps[0]);
