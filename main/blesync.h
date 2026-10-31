/*
 * blesync.h — « Économie d'énergie » Bluetooth : la radio reste éteinte
 * (light sleep réel, ~1 mA) et ne se connecte à l'iPhone que par courtes
 * relèves périodiques, ou tant qu'un écran qui en a besoin est ouvert
 * (lecture en cours, notifications, appairage).
 *
 * Tout s'exécute dans la tâche app (événements EV_SYNC).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { SYNC_EV_START = 1, SYNC_EV_TICK, SYNC_EV_FLUSH };

void blesync_init(void);              /* au démarrage, à la place de ble_start() */
void blesync_apply(void);             /* réglages Bluetooth / économie modifiés */
void blesync_on_event(int32_t code);  /* EV_SYNC */
void blesync_hold(bool on);           /* écran qui a besoin de la liaison (compteur) */
void blesync_note_notif(void);        /* une notif vient d'arriver (activité ANCS) */

bool     blesync_eco(void);           /* mode économie actif (et Bluetooth autorisé) */
bool     blesync_sleeping(void);      /* économie : radio éteinte entre deux relèves */
bool     blesync_in_session(void);    /* économie : relève en cours */
int64_t  blesync_last_sync_s(void);   /* âge de la dernière relève réussie (s), -1 si aucune */

/* Regroupement des alertes pendant une relève : une seule vibration pour le
 * lot de notifications arrivées pendant la coupure. */
bool     blesync_defer_alert(uint32_t hash);
uint32_t blesync_take_pending(void);   /* SYNC_EV_FLUSH : notif à alerter (hash), 0 si aucune */
