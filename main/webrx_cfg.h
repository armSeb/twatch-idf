/*
 * webrx_cfg.h — réglages de l'app WebRX en NVS : réseaux Wi-Fi connus et
 * adresse du serveur OpenWebRX (saisis sur la page web de la montre), puis
 * dernier état d'écoute (profil, fréquence, mode, volume, squelch).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define WRX_NETS 3
#define WRX_SRV  4                       /* serveurs OpenWebRX mémorisés */
#define WRX_TONES 5                      /* tonalité : 0 neutre … 4 sourde */
#define WRX_TONE_DEF 3
/* « sdr|profil » : OpenWebRX+ utilise des UUID (36 + 1 + 36 caractères) */
#define WRX_ID_LEN 96

typedef struct {
    char ssid[WRX_NETS][33];
    char pass[WRX_NETS][65];
    struct {
        char name[32];                   /* "" = l'hôte de l'adresse */
        char url[192];                   /* https://hôte[:port][/chemin/] */
    } srv[WRX_SRV];
} wrx_cfg_t;

/* état d'écoute : serveur choisi et tonalité (communs), puis profil,
 * fréquence, mode et squelch propres à ce serveur */
typedef struct {
    uint8_t server;              /* index dans srv[] */
    uint8_t tone;                /* 0..WRX_TONES-1 */
    char    profile[WRX_ID_LEN];         /* « sdr|profil » choisi, "" = celui du serveur */
    int64_t freq;                /* Hz, 0 = fréquence de départ du profil */
    char    mod[16];             /* "" = mode de départ du profil */
    int16_t squelch;             /* dB ; -150 = coupé */
} wrx_state_t;

void      wrx_cfg_load(wrx_cfg_t *c);
esp_err_t wrx_cfg_save(const wrx_cfg_t *c);
bool      wrx_cfg_ready(void);          /* au moins un réseau et un serveur */
int       wrx_cfg_servers(const wrx_cfg_t *c);   /* serveurs renseignés (tassés en tête) */
void      wrx_srv_label(const wrx_cfg_t *c, int i, char *out, size_t cap);  /* nom, sinon hôte */

void      wrx_state_load(wrx_state_t *s);           /* serveur courant */
void      wrx_state_load_srv(wrx_state_t *s, int server);
void      wrx_state_save(const wrx_state_t *s);

/* ---------------- webradios (app Radio), espace NVS « radio » ---------------- */
#define RAD_MAX 12

typedef struct {
    char name[32];
    char url[192];                       /* flux MP3/AAC, ou liste .m3u / .pls */
} rad_station_t;

typedef struct {
    uint8_t station;                     /* dernière écoutée */
    uint8_t tone;                        /* 0..WRX_TONES-1 */
} rad_state_t;

int       rad_cfg_load(rad_station_t st[RAD_MAX]);        /* nombre de stations (tassées en tête) */
esp_err_t rad_cfg_save(const rad_station_t st[RAD_MAX]);
void      rad_state_load(rad_state_t *s);
void      rad_state_save(const rad_state_t *s);
void      rad_label(const rad_station_t *st, char *out, size_t cap);   /* nom, sinon hôte */
