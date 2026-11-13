/*
 * wifi_sta.h — Wi-Fi station partagé par les apps qui écoutent le réseau
 * (WebRX, Radio) : réseaux connus de la page de réglages (webrx_cfg), essai
 * dans l'ordre de préférence parmi ceux qui sont visibles, Bluetooth
 * suspendu seulement si le pilote Wi-Fi manque de RAM interne.
 *
 * init / deinit depuis la tâche app ; start / connect / stop depuis la
 * tâche réseau de l'app.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"
#include "webrx_cfg.h"

/* étape (fail = false) ou échec (fail = true) : msg N_(), detail traduit */
typedef void (*wsta_status_cb)(bool fail, const char *msg, const char *detail);

esp_err_t wsta_init(const char *who);       /* who : préfixe du journal (« WRX », « RAD ») */
void      wsta_deinit(void);                /* rend aussi le Bluetooth s'il avait été suspendu */
bool      wsta_ble_paused(void);
bool      wsta_start(wifi_ps_type_t ps);
bool      wsta_connect(const wrx_cfg_t *cfg, volatile bool *quit, wsta_status_cb cb);
bool      wsta_up(void);                    /* adresse IP obtenue */
bool      wsta_lost(void);                  /* déconnecté depuis la dernière connexion */
int       wsta_rssi(void);                  /* dBm, 0 = inconnu */
void      wsta_stop(void);
