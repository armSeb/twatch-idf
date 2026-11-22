/*
 * ir_nec.h — émission infrarouge au protocole NEC (LED IR, GPIO 13, RMT).
 *
 * Trame NEC : porteuse 38 kHz, en-tête 9 ms + 4,5 ms, 32 bits poids faible
 * d'abord (adresse basse, adresse haute, commande, commande inversée), bit
 * 562 µs + 562 µs (0) ou 1687 µs (1), puis une impulsion de fin.
 * L'adresse est donnée telle qu'elle part sur 16 bits : en NEC « standard »
 * l'octet haut est l'inverse de l'octet bas (ir_nec_std_addr).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

static inline uint16_t ir_nec_std_addr(uint8_t a) { return (uint16_t)(a | ((uint8_t)~a << 8)); }

esp_err_t ir_nec_open(void);                            /* canal RMT réservé jusqu'à close */
esp_err_t ir_nec_send(uint16_t addr, uint8_t cmd);      /* bloquant, ~68 ms */
void      ir_nec_close(void);
