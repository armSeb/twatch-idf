#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t brightness;       /* 10..255 */
    uint8_t screen_timeout_s; /* 5..60 */
    bool    vibrate;
    bool    ble_enabled;      /* false = mode avion (light sleep réel) */
    int16_t tz_offset_min;    /* fuseau + DST */
    uint8_t watchface;        /* 0 = classique, 1 = numérique, 2 = pub (carré) */
    bool    tap_wake;         /* double tap sur l'écran éteint = réveil */
    bool    notif_wake;       /* false = notif : vibre seulement, bouton pour voir */
    bool    dnd;              /* ne pas déranger : ni vibration ni allumage (sauf appels) */
} settings_t;

void        settings_load(void);
void        settings_save(void);
settings_t *settings_get(void);
