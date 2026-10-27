/*
 * ble.h — pile NimBLE : GAP périphérique, sécurité (passkey affiché),
 * service batterie, paramètres de connexion économes, cycle de vie.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    BLE_ST_OFF = 0,        /* pile arrêtée (mode avion) */
    BLE_ST_ADVERTISING,
    BLE_ST_CONNECTED,      /* lien établi, pas encore chiffré/ANCS */
    BLE_ST_SECURED,        /* chiffré, découverte en cours */
    BLE_ST_READY,          /* ANCS abonné */
} ble_state_t;

typedef struct {
    ble_state_t state;
    uint16_t itvl_x125;    /* intervalle en unités de 1,25 ms */
    uint16_t latency;
    uint16_t timeout_x10;  /* supervision en unités de 10 ms */
    int      bonds;
    uint8_t  addr[6];
    bool     adv_active;   /* publicité réellement active côté pile */
} ble_info_t;

/* Journal de diagnostic BLE en RAM (consultable dans « À propos », sans PC) */
#define BLE_DIAG_LINES 16
void ble_diag(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  ble_diag_dump(char *out, size_t len);   /* plus récent en premier */

esp_err_t ble_start(void);              /* init + démarrage de la pile */
void      ble_stop(void);               /* arrêt complet (contrôleur off) */
bool      ble_running(void);
void      ble_get_info(ble_info_t *out);
void      ble_set_battery(uint8_t pct); /* thread-safe */
void      ble_forget_all(void);         /* thread-safe : efface les bonds, déconnecte */
void      ble_pairing_mode(void);       /* thread-safe : pub rapide pendant 60 s */

/* usage interne (ancs.c) */
void      ble_internal_set_state(ble_state_t st);
void      ble_internal_ancs_ready(uint16_t conn);
