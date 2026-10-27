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
#include "esp_err.h"

esp_err_t display_init(void);          /* LCD + tactile + lv_init + display/indev */
void      display_set_on(bool on);      /* séquence complète veille/réveil */
bool      display_is_on(void);
void      display_set_brightness(uint8_t level);
void      display_perf_tick(void);      /* CPU max seulement pendant l'interaction */
