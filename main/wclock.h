/*
 * rtc.h — PCF8563 (stocke l'UTC) + heure système + fuseau.
 *
 * L'horloge système ESP32 tourne en UTC. L'affichage applique un décalage
 * en minutes (fuseau + DST) fourni par le CTS de l'iPhone (Local Time
 * Information) et persisté en NVS.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

esp_err_t wclock_init(void);                  /* lit la RTC → settimeofday() */
bool      wclock_time_valid(void);
void      wclock_resync(void);              /* RTC → système (au réveil, toutes les 60 s) */
void      wclock_set_utc(time_t utc);         /* système + RTC */
int32_t   wclock_tz_offset_min(void);
void      wclock_set_tz_offset_min(int32_t off);   /* + NVS si changé */
void      wclock_local_tm(struct tm *out);    /* heure locale courante */
