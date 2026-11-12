/*
 * sdr_rx.h — réception I/Q par la radio Wi-Fi de l'ESP32 et calcul des lignes
 * de spectre (waterfall). Mode SDR uniquement (voir sdr_mode.h).
 *
 * Une tâche dédiée (cœur 0) accorde la radio, capture des blocs d'I/Q à
 * 40 MS/s, fait les FFT et produit des lignes de SDR_W valeurs en dBFS.
 * L'interface (tâche app) les récupère avec sdr_rx_pop().
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SDR_W 240

enum { SDR_VIEW_BAND = 0, SDR_VIEW_ZOOM = 1 };
#define SDR_GAIN_LEVELS 5            /* 0 = AGC matériel, 1…4 = gain manuel croissant */

typedef struct {
    float    db[SDR_W];              /* dBFS par colonne */
    uint8_t  view, ch, gain;         /* réglages au moment de la capture */
    uint8_t  sat;                    /* convertisseur saturé pendant la ligne */
} sdr_row_t;

esp_err_t sdr_rx_start(void);        /* Wi-Fi (mode moniteur) + tâche */
void      sdr_rx_stop(void);
void      sdr_rx_pause(bool pause);  /* radio laissée accordée, plus de captures */
void      sdr_rx_config(int view, int ch, int gain);
bool      sdr_rx_pop(sdr_row_t *out);/* ligne suivante, false si aucune */
float     sdr_rx_mhz(int view, int ch, float x);   /* colonne → fréquence */
float     sdr_rx_x(int view, int ch, float mhz);   /* fréquence → colonne */
float     sdr_rx_rate(void);         /* lignes par seconde (mesuré) */
const char *sdr_rx_error(void);      /* NULL si tout va bien */
