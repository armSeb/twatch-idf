/*
 * radio.h — lecteur de webradios : flux HTTP(S) MP3 ou AAC/HE-AAC (Icecast,
 * Shoutcast, listes .m3u/.pls), titres ICY, décodage Helix, haut-parleur.
 *
 * Deux tâches : « rad » (cœur 0) gère le Wi-Fi et le flux HTTP et remplit un
 * tampon compressé de 128 Ko en PSRAM (~8 s à 128 kbit/s) ; « radplay »
 * (cœur 1) décode, passe en mono, corrige la tonalité et alimente l'I2S.
 * Économie : modem sleep Wi-Fi, CPU plafonné à 160 MHz (80 MHz écran
 * éteint) avec relance automatique à 160 MHz si le décodage ne suit plus ;
 * écran éteint, puissance d'émission Wi-Fi réduite si le signal le permet.
 * API à appeler depuis la tâche app.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "webrx_cfg.h"

#define RAD_VOL_START 10              /* % au début de chaque écoute */

enum {
    RAD_OFF = 0,
    RAD_WIFI,          /* recherche / connexion au réseau */
    RAD_CONNECT,       /* connexion au flux */
    RAD_BUFFER,        /* remplissage du tampon */
    RAD_PLAY,
    RAD_FAIL,          /* échec, nouvel essai automatique */
};

typedef struct {
    int      state;
    const char *msg;                  /* N_() : étape ou cause, NULL si rien */
    char     detail[48];
    int      retry_s;
    uint8_t  station, nstations;
    char     name[40];                /* station (nom saisi, sinon icy-name, sinon hôte) */
    char     title[128];              /* titre en cours (ICY StreamTitle), UTF-8 */
    char     codec[8];                /* « MP3 », « AAC », « HE-AAC » */
    int      kbps, rate, chans;
    int      buf_pct;                 /* remplissage du tampon compressé */
    int      fill_pct;                /* RAD_BUFFER : progression avant lecture */
    int      load;                    /* % du temps passé à décoder */
    bool     boost;                   /* CPU relevé pour suivre le décodage */
    uint8_t  volume, tone;
    int      rssi;
} rad_info_t;

esp_err_t radio_start(void);
void      radio_stop(void);           /* bloquant (quelques secondes au pire) */
bool      radio_running(void);
bool      radio_ble_paused(void);
void      radio_retry(void);
void      radio_info(rad_info_t *out);
void      radio_station(int idx);     /* change de station (en marche ou pour la prochaine écoute) */
void      radio_volume(int pct);
void      radio_tone(int level);
void      radio_screen(bool on);      /* indication d'économie (écran éteint) */
