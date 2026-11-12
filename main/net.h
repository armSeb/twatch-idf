/*
 * net.h — socle réseau partagé (pile IP, boucle d'événements, interfaces
 * Wi-Fi par défaut), créé une seule fois à la première utilisation.
 */
#pragma once
#include "esp_err.h"
#include "esp_netif.h"

esp_err_t    net_base_init(void);      /* esp_netif + boucle d'événements par défaut */
esp_netif_t *net_ap_netif(void);       /* interface point d'accès (créée au besoin) */
esp_netif_t *net_sta_netif(void);      /* interface station (créée au besoin) */
