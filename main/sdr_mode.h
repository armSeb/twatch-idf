/*
 * sdr_mode.h — prêt de la radio à l'app SDR, sans redémarrage.
 *
 * La capture I/Q de l'ESP32 passe par le mode « MAC dump » de la radio, qui
 * écrit dans deux bancs de SRAM fixes (0x3FFE8000–0x3FFF8000, 64 Ko). Ces
 * bancs ne sont jamais donnés au tas général : ils forment un petit tas privé
 * (« rfbank ») que le lecteur de webradios utilise pour l'état de ses
 * décodeurs (RAM interne rapide). L'app SDR les prend quand elle s'ouvre :
 * le tas privé doit alors être vide (Radio arrêtée), le Wi-Fi libre, et le
 * Bluetooth est arrêté le temps de l'app (il partage la radio) puis relancé.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define SDR_DUMP_START 0x3ffe8000u
#define SDR_DUMP_END   0x3fff8000u

void      sdr_mode_boot(void);       /* tout début d'app_main */
bool      sdr_mode_active(void);     /* la radio est à l'app SDR */
esp_err_t sdr_mode_enter(void);      /* arrête le Bluetooth, prend les bancs */
void      sdr_mode_leave(void);      /* rend les bancs, relance le Bluetooth */
const char *sdr_mode_error(void);    /* N_() : pourquoi sdr_mode_enter a échoué */

/* tas privé des bancs (NULL si plein ou prêté au SDR) */
void  *rfbank_malloc(size_t n);
bool   rfbank_free(void *p);         /* false : p n'est pas dans les bancs */
size_t rfbank_free_size(void);

/* App à rouvrir après le prochain redémarrage logiciel (index dans g_apps) :
 * set avant esp_restart, lu une fois au démarrage (-1 = aucune). */
void sdr_mode_set_boot_app(int idx);
int  sdr_mode_boot_app(void);
