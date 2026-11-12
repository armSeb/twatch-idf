/* net.c — voir net.h. Appelé depuis la tâche app uniquement. */
#include "net.h"
#include "esp_event.h"
#include "esp_wifi_default.h"

static bool s_base;
static esp_netif_t *s_ap, *s_sta;

esp_err_t net_base_init(void)
{
    if (s_base) return ESP_OK;
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    s_base = true;
    return ESP_OK;
}

esp_netif_t *net_ap_netif(void)
{
    if (!s_ap && net_base_init() == ESP_OK) s_ap = esp_netif_create_default_wifi_ap();
    return s_ap;
}

esp_netif_t *net_sta_netif(void)
{
    if (!s_sta && net_base_init() == ESP_OK) s_sta = esp_netif_create_default_wifi_sta();
    return s_sta;
}
