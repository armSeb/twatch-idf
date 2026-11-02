/*
 * face_pub.h — cadran carré « pub irlandais ».
 *
 * Plein écran : cadran vert bouteille aux angles arrondis de montre carrée,
 * filets et index laiton, harpe celtique dans un coin.
 * Complications :
 *   12 h : jauge FUEL (batterie, E → F, zone rouge en réserve)
 *    3 h : guichet jour + date          6 h : petite seconde
 *    9 h : enseigne de pub = état de la liaison iPhone (OPEN / WAIT / CLOSED /
 *          OFF) avec voyant de couleur, pastille du nombre de notifications
 *
 * Le décor (graduations, index, harpe, zone rouge) est dessiné par un
 * callback de dessin, sans objet LVGL par trait.
 */
#pragma once
#include <stdbool.h>
#include <time.h>
#include "lvgl.h"

enum { PUB_LINK_OFF = 0, PUB_LINK_ADV, PUB_LINK_WAIT, PUB_LINK_READY };

lv_obj_t *face_pub_create(lv_obj_t *parent);               /* conteneur 240×240 */
void      face_pub_set_time(const struct tm *tm, bool force);
void      face_pub_set_status(int batt_pct, bool charging, int link, int notifs);
