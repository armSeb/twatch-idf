/*
 * ir_tx.h — émission infrarouge (LED IR, GPIO 13) par le RMT, tous les
 * protocoles d'ir_proto.h. Le canal (et le verrou de fréquence APB que le
 * pilote RMT prend) n'existe qu'entre ir_tx_open() et ir_tx_close().
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "ir_proto.h"

esp_err_t ir_tx_open(void);
/* bloquant ; held = touche gardée enfoncée (RC5/RC6 : même bascule) */
esp_err_t ir_tx_send(const ir_code_t *c, bool held);
void      ir_tx_close(void);
