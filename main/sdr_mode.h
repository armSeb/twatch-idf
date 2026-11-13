/*
 * sdr_mode.h — démarrage « mode SDR ».
 *
 * La capture I/Q de l'ESP32 passe par le mode « MAC dump » de la radio, qui
 * écrit dans deux bancs de SRAM fixes (0x3FFE8000–0x3FFF8000, 64 Ko). Ces
 * bancs sont donc réservés au lien ; en mode normal, ils sont rendus au tas dès
 * le démarrage (aucune RAM perdue). Le mode SDR s'obtient par un redémarrage :
 * Bluetooth jamais démarré (sa mémoire est même libérée), bancs gardés pour la
 * radio, app SDR ouverte d'office. En sortir redémarre en mode normal ; un
 * plantage aussi (le drapeau est consommé au démarrage).
 */
#pragma once
#include <stdbool.h>

#define SDR_DUMP_START 0x3ffe8000u
#define SDR_DUMP_END   0x3fff8000u

void sdr_mode_boot(void);        /* tout début d'app_main */
bool sdr_mode_active(void);
void sdr_mode_reboot(bool sdr);  /* redémarre en mode SDR (true) ou normal */

/* App à rouvrir après le prochain redémarrage logiciel (index dans g_apps) :
 * set avant esp_restart, lu une fois au démarrage (-1 = aucune). */
void sdr_mode_set_boot_app(int idx);
int  sdr_mode_boot_app(void);
