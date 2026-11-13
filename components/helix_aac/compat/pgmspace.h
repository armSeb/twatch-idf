/* Compatibilité : les sources Helix viennent d'ESP8266Audio (Arduino). Sur
 * ESP32 les tables const sont lues directement en flash (cache). */
#pragma once
#include <stdint.h>
#ifndef PROGMEM
#define PROGMEM
#endif
#define pgm_read_byte(p)  (*(const uint8_t *)(p))
#define pgm_read_word(p)  (*(const uint16_t *)(p))
#define pgm_read_dword(p) (*(const uint32_t *)(p))
