/*
 * ui.h — interface LVGL (appelée uniquement depuis la tâche app).
 *
 * Écrans : cadran · liste des notifications · détail · appel entrant ·
 * menu/réglages · énergie (télémétrie) · appairage · à propos.
 * Gestes : cadran ↑ notifications, ← menu ; → retour partout.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "notif_store.h"

void     ui_init(void);
void     ui_go_home(void);                 /* cadran, sans animation si écran éteint */
bool     ui_is_home(void);
uint32_t ui_timeout_ms(void);              /* délai d'inactivité de l'écran courant */

void ui_on_notif(notif_entry_t *e, bool alert); /* alert → affiche le détail */
void ui_on_notif_removed(uint32_t uid);
void ui_on_ble_state(int st);
void ui_on_power(void);                    /* batterie / charge changés */
void ui_on_time_changed(void);
void ui_show_passkey(uint32_t pk);
void ui_pairing_done(int status);
void ui_on_wake(void);                     /* juste avant l'allumage de l'écran */
