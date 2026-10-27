/*
 * face_classic.h — cadran analogique « montre ancienne ».
 *
 * Cadran ivoire, index appliqués, aiguilles acier bleui. Complications :
 *   12 h : réserve de marche (batterie)      3 h : guichet jour + date
 *    6 h : petite seconde                     9 h : état Bluetooth + notifications
 *
 * Coût : aiguilles heures/minutes redessinées une fois par minute, petite
 * seconde (zone 60×60) une fois par seconde, uniquement écran allumé.
 */
#pragma once
#include <stdbool.h>
#include <time.h>
#include "lvgl.h"

lv_obj_t *face_classic_create(lv_obj_t *parent);           /* conteneur 240×240 */
void      face_classic_set_time(const struct tm *tm, bool force);
void      face_classic_set_status(int batt_pct, bool charging, bool ble_ok, bool ble_on, int notifs);
