/*
 * ancs.h — client ANCS (Apple Notification Center Service) + CTS, NimBLE C.
 * Tout tourne dans la tâche hôte NimBLE ; la sortie se fait par une file de
 * notifications complètes (copie par valeur) + événements app.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"

#include "ancs_types.h"

extern QueueHandle_t g_notif_queue;      /* ancs_notif_t, créée par ancs_module_init */

void ancs_module_init(void);             /* une fois, avant ble_start */

/* appelés par ble.c (contexte hôte) */
void ancs_on_connect(uint16_t conn);
void ancs_on_encrypted(uint16_t conn);
void ancs_on_disconnect(void);
void ancs_on_notify_rx(uint16_t conn, uint16_t attr, struct os_mbuf *om);
void ancs_host_init(void);               /* à chaque démarrage de la pile */
void ancs_host_deinit(void);

bool ancs_ready(void);
/* thread-safe : demande une action (0 = positive, 1 = négative) */
void ancs_request_action(uint32_t uid, uint8_t action);
