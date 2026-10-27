/*
 * face_pub.h — cadran carré « pub irlandais ».
 *
 * Plein écran : cadre en bois d'acajou veiné, cadran vert bouteille aux
 * angles arrondis de montre carrée, filets et index laiton, petit trèfle.
 * Mêmes complications que le cadran classique :
 *   12 h : réserve de marche (batterie)      3 h : guichet jour + date
 *    6 h : petite seconde                     9 h : état Bluetooth + notifications
 *
 * Le décor (bois, graduations, index, trèfle) est dessiné par deux callbacks
 * de dessin, sans objets LVGL par trait : ~20 objets au total.
 */
#pragma once
#include <stdbool.h>
#include <time.h>
#include "lvgl.h"

lv_obj_t *face_pub_create(lv_obj_t *parent);               /* conteneur 240×240 */
void      face_pub_set_time(const struct tm *tm, bool force);
void      face_pub_set_status(int batt_pct, bool charging, bool ble_ok, bool ble_on, int notifs);
