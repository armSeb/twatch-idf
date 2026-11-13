/*
 * webrx_cfg.h — réglages de l'app WebRX en NVS : réseaux Wi-Fi connus et
 * adresse du serveur OpenWebRX (saisis sur la page web de la montre), puis
 * dernier état d'écoute (profil, fréquence, mode, volume, squelch).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WRX_NETS 3

typedef struct {
    char ssid[WRX_NETS][33];
    char pass[WRX_NETS][65];
    char url[192];               /* https://hôte[:port][/chemin/] */
} wrx_cfg_t;

typedef struct {
    char    profile[72];         /* « sdr|profil » choisi, "" = celui du serveur */
    int64_t freq;                /* Hz, 0 = fréquence de départ du profil */
    char    mod[16];             /* "" = mode de départ du profil */
    uint8_t volume;              /* 0..100 */
    int16_t squelch;             /* dB ; -150 = coupé */
    uint8_t filter;              /* filtre « voix » (1 par défaut) */
} wrx_state_t;

void      wrx_cfg_load(wrx_cfg_t *c);
esp_err_t wrx_cfg_save(const wrx_cfg_t *c);
bool      wrx_cfg_ready(void);          /* au moins un réseau et une adresse */

void      wrx_state_load(wrx_state_t *s);
void      wrx_state_save(const wrx_state_t *s);
