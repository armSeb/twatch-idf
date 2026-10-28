/*
 * app_events.h — bus d'événements vers la tâche applicative.
 *
 * Règle d'architecture : LVGL et l'état applicatif n'appartiennent qu'à la
 * tâche « app ». Les autres contextes (tâche hôte NimBLE, ISR AXP, timers)
 * ne font que poster des événements. Aucun mutex LVGL, aucun accès croisé.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"

typedef enum {
    EV_AXP_IRQ = 1,     /* ligne IRQ de l'AXP202 basse                      */
    EV_NOTIF,           /* une notification ANCS est dispo dans la file      */
    EV_NOTIF_REMOVED,   /* a = UID iOS retiré                                */
    EV_BLE_STATE,       /* a = ble_state_t                                   */
    EV_PASSKEY,         /* a = code à 6 chiffres à afficher                  */
    EV_PAIRING_DONE,    /* a = 0 succès, sinon code d'erreur                 */
    EV_TIME_SYNC,
    EV_TOUCH_INT,
    EV_MEDIA,           /* lecture en cours (AMS) modifiée : media_get()     */
    EV_OTA,             /* a = code OTA_EV_* (progression, fin, erreur)      */
    EV_COUNT,       /* écran éteint : ligne INT du FT6336 basse (toucher) */       /* t = epoch UTC, a = offset local en minutes ou INT32_MIN */
} app_ev_type_t;

typedef struct {
    app_ev_type_t type;
    int32_t a;
    int64_t t;
} app_event_t;

/* diagnostic conso : événements traités écran éteint, par type (main.c) */
extern uint32_t g_wake_cnt[EV_COUNT];

bool app_post(const app_event_t *ev);
bool app_post_from_isr(const app_event_t *ev, BaseType_t *hp_woken);

static inline bool app_post_simple(app_ev_type_t type, int32_t a)
{
    app_event_t ev = { .type = type, .a = a, .t = 0 };
    return app_post(&ev);
}
