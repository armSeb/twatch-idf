/*
 * webrx.h — client OpenWebRX : Wi-Fi station, WebSocket (wss:// ou ws://),
 * décodage de l'audio IMA-ADPCM du serveur et lecture sur le haut-parleur.
 *
 * Deux tâches : « wrx » (cœur 0) gère le Wi-Fi et la connexion, reçoit les
 * trames et remplit un tampon audio de ~2,7 s en PSRAM ; « wrxplay » (cœur 1)
 * le vide vers l'I2S avec ~0,4 s d'avance pour absorber les à-coups du réseau
 * et le modem sleep. L'interface lit l'état par webrx_info() (copie sous
 * verrou) et envoie ses ordres par les autres fonctions, non bloquantes sauf
 * webrx_start / webrx_stop. Toutes depuis la tâche app.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WRX_VOL_START 10           /* % : volume au début de chaque écoute */
#define WRX_RATE   12000            /* Hz : audio demandé au serveur (tous modes) */
#define WRX_FFT_W  240              /* colonnes d'une ligne de waterfall */
#define WRX_BM_MAX 64
#define WRX_PROF_MAX 24
#define WRX_MODE_MAX 24

enum {
    WRX_OFF = 0,     /* arrêté */
    WRX_WIFI,        /* recherche / connexion au réseau */
    WRX_SERVER,      /* connexion au serveur, poignée de main */
    WRX_LIVE,        /* réception */
    WRX_FAIL,        /* échec (msg), nouvel essai automatique ou bouton */
};

typedef struct {
    int      state;
    const char *msg;                /* N_() : étape ou cause d'échec, NULL si rien */
    char     detail[48];            /* réseau, hôte, code HTTP… */
    int      retry_s;               /* WRX_FAIL : nouvel essai dans … s (0 = non) */
    int64_t  center;                /* Hz, 0 = inconnu */
    int32_t  samp_rate;
    int64_t  freq;                  /* Hz, fréquence écoutée */
    char     mod[16];
    float    level_db;              /* S-mètre (dB), -200 = inconnu */
    int16_t  squelch;               /* dB, -150 = coupé */
    uint8_t  volume;                /* 0..100 */
    uint8_t  filter;                /* 1 = filtre « voix » (aigus et souffle atténués) */
    char     profile[72];           /* « sdr|profil » actif */
    char     station[48];           /* nom RDS (WFM) ou du signet écouté */
    uint32_t bm_ver, prof_ver, mode_ver;   /* changent quand les listes changent */
    int      buf_ms;                /* audio d'avance */
    bool     buffering;
    int      rssi;                  /* dBm du Wi-Fi, 0 = inconnu */
} wrx_info_t;

typedef struct { char name[40]; int64_t freq; char mod[16]; } wrx_bm_t;
typedef struct { char id[72]; char name[48]; } wrx_prof_t;
/* mode d'écoute proposé par le serveur (liste « modes », modes ajoutés par
 * l'administrateur compris) ; bande passante en Hz autour de la fréquence */
typedef struct { char mod[16]; char name[24]; int lo, hi; bool bp; } wrx_mode_t;

typedef struct {
    float   db[WRX_FFT_W];
    int64_t lo;                     /* Hz, fréquence de la colonne 0 */
    int32_t span;                   /* Hz couverts par les 240 colonnes */
} wrx_row_t;

esp_err_t webrx_start(void);        /* réglages lus en NVS ; Bluetooth suspendu si la RAM manque */
void      webrx_stop(void);         /* bloquant (quelques secondes au pire) */
bool      webrx_running(void);
bool      webrx_ble_paused(void);   /* le Bluetooth a été suspendu pour la RAM */
void      webrx_retry(void);        /* WRX_FAIL : réessayer tout de suite */

void      webrx_info(wrx_info_t *out);
int       webrx_bookmarks(wrx_bm_t *out, int max);    /* triés par fréquence */
int       webrx_profiles(wrx_prof_t *out, int max);
int       webrx_modes(wrx_mode_t *out, int max);          /* modes analogiques écoutables */

void      webrx_tune(int64_t hz);   /* borné à la bande du profil */
void      webrx_set_mod(const char *mod);
void      webrx_select(const wrx_bm_t *bm);          /* fréquence + mode d'un signet */
void      webrx_profile(const char *id);
void      webrx_volume(int pct);
void      webrx_squelch(int db);
void      webrx_filter(bool on);    /* filtre « voix » */    /* -150 = coupé */

/* waterfall : produit seulement si visible ; zoom 1, 4 ou 16 (centré sur la
 * fréquence écoutée) */
void      webrx_fft_enable(bool on, int zoom);
bool      webrx_fft_pop(wrx_row_t *out);
