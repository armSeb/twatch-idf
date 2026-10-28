/*
 * ota.h — mise à jour du firmware par Wi-Fi, sans câble.
 *
 * La montre coupe le Bluetooth, ouvre son propre point d'accès WPA2
 * (« T-Watch-MAJ », mot de passe fixe affiché à l'écran) et sert une page web
 * sur http://192.168.4.1 pour envoyer le fichier twatch2020v1-app.bin.
 *
 * Sécurité :
 *  - l'image est écrite dans l'autre emplacement (ota_0 / ota_1) ; l'actuel
 *    n'est jamais touché tant que la nouvelle n'est pas complète et vérifiée
 *    (en-tête ESP32, nom de projet « twatch », empreinte SHA-256) ;
 *  - au premier démarrage, la nouvelle image est « en attente » : si elle
 *    plante avant d'avoir tourné 30 s, le bootloader revient à l'ancienne ;
 *  - refus sous 30 % de batterie hors USB, abandon après 5 min sans envoi.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* codes EV_OTA (champ a) */
#define OTA_EV_PROGRESS_MAX 100   /* 0..100 : pourcentage reçu */
#define OTA_EV_CLIENT       200   /* un appareil s'est connecté au point d'accès */
#define OTA_EV_DONE         300   /* image vérifiée, démarrage programmé */
#define OTA_EV_ERR_IMAGE    (-1)  /* pas un firmware T-Watch / image corrompue */
#define OTA_EV_ERR_WRITE    (-2)  /* erreur d'écriture flash */
#define OTA_EV_ERR_SIZE     (-3)  /* trop gros pour l'emplacement */
#define OTA_EV_ERR_NET      (-4)  /* transfert interrompu */
#define OTA_EV_TIMEOUT      (-5)  /* 5 min sans envoi : abandon */

void        ota_boot_check(void);          /* au démarrage : état de l'image courante */
void        ota_tick(void);                /* tâche app, périodique : validation après 30 s */
bool        ota_rolled_back(void);         /* l'image précédente avait échoué */

/* depuis la tâche app (UI) */
const char *ota_start(void);               /* NULL si démarré, sinon raison du refus */
void        ota_cancel(void);              /* abandon : redémarrage propre */
bool        ota_active(void);
const char *ota_ssid(void);
const char *ota_password(void);
const char *ota_new_version(void);         /* version de l'image reçue (après OTA_EV_DONE) */
void        ota_reboot_now(void);
