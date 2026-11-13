/*
 * display.h — ST7789 (esp_lcd, SPI DMA) + FT6336 + glue LVGL 9.
 *
 * Pas d'esp_lvgl_port volontairement : son timer de tick périodique et sa
 * tâche dédiée réveilleraient le CPU en permanence. Ici le tick LVGL est lu
 * à la demande (esp_timer_get_time) et lv_timer_handler n'est appelé que
 * par la tâche app, uniquement écran allumé.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t display_init(void);          /* LCD + tactile + lv_init + display/indev */
void      display_set_on(bool on);      /* séquence complète veille/réveil */
bool      display_is_on(void);
void      display_set_brightness(uint8_t level);
void      display_perf_tick(void);
/* réveil par double tap (écran éteint) */
bool      display_tap_begin(void);      /* sur EV_TOUCH_INT : démarre l'analyse */
int       display_tap_poll(void);       /* 1 = double tap, 0 = continuer, -1 = abandon */
uint32_t  display_tap_sample_ms(void);
void      display_tap_enable(bool en);  /* réglage modifié */      /* CPU max seulement pendant l'interaction */
