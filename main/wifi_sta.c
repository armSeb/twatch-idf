/* wifi_sta.c — voir wifi_sta.h */
#include "wifi_sta.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "i18n.h"
#include "net.h"
#include "ble.h"

static const char *TAG = "wsta";

#define BIT_IP   BIT0
#define BIT_DISC BIT1

static EventGroupHandle_t s_evg;
static bool s_inited, s_ble_paused;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;
static volatile int s_reason;
static const char *s_who = "NET";

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        xEventGroupClearBits(s_evg, BIT_IP);
        xEventGroupSetBits(s_evg, BIT_DISC);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_evg, BIT_IP);
    }
}

esp_err_t wsta_init(const char *who)
{
    if (s_inited) return ESP_OK;
    s_who = who;
    if (!s_evg) s_evg = xEventGroupCreate();
    if (!s_evg) return ESP_ERR_NO_MEM;
    xEventGroupClearBits(s_evg, BIT_IP | BIT_DISC);
    if (net_base_init() != ESP_OK || !net_sta_netif()) return ESP_FAIL;
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    wc.nvs_enable = 0;
    esp_err_t e = esp_wifi_init(&wc);
    if (e != ESP_OK && ble_running()) {
        /* pas assez de RAM interne à côté du Bluetooth : suspendu le temps
         * de l'écoute */
        ESP_LOGW(TAG, "esp_wifi_init: %s, Bluetooth suspendu", esp_err_to_name(e));
        ble_diag("%s : Bluetooth suspendu (RAM)", who);
        ble_stop();
        if (!ble_running()) {
            s_ble_paused = true;
            esp_wifi_deinit();
            e = esp_wifi_init(&wc);
        }
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_init: %s (interne libre %u, bloc %u)", esp_err_to_name(e),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (s_ble_paused) { s_ble_paused = false; ble_start(); }
        return e;
    }
    s_inited = true;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL, &s_h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, &s_h_ip);
    return ESP_OK;
}

void wsta_deinit(void)
{
    if (s_inited) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_h_wifi);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
        esp_wifi_deinit();
        s_inited = false;
    }
    if (s_ble_paused) {
        s_ble_paused = false;
        ble_start();
    }
}

bool wsta_ble_paused(void) { return s_ble_paused; }

bool wsta_start(wifi_ps_type_t ps)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(ps);
    return esp_wifi_start() == ESP_OK;
}

void wsta_stop(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (s_evg) xEventGroupClearBits(s_evg, BIT_IP);
}

bool wsta_up(void) { return s_evg && (xEventGroupGetBits(s_evg) & BIT_IP); }
bool wsta_lost(void) { return s_evg && (xEventGroupGetBits(s_evg) & BIT_DISC); }

int wsta_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

/* attend un des bits (ou l'arrêt demandé) au plus ms */
static EventBits_t wait_bits(EventBits_t bits, volatile bool *quit, int ms)
{
    for (int t = 0; t < ms; t += 100) {
        EventBits_t b = xEventGroupWaitBits(s_evg, bits, pdFALSE, pdFALSE, pdMS_TO_TICKS(100));
        if ((b & bits) || *quit) return b;
    }
    return xEventGroupGetBits(s_evg);
}

bool wsta_connect(const wrx_cfg_t *cfg, volatile bool *quit, wsta_status_cb cb)
{
    cb(false, N_("Recherche du Wi-Fi…"), "");
    bool seen[WRX_NETS] = { 0 };
    int nseen = 0;
    wifi_scan_config_t sc = { .show_hidden = true };
    sc.scan_time.active.min = 40;
    sc.scan_time.active.max = 100;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 24;
        wifi_ap_record_t *rec = heap_caps_malloc(n * sizeof *rec, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (rec && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
            for (int i = 0; i < n; i++)
                for (int k = 0; k < WRX_NETS; k++)
                    if (cfg->ssid[k][0] && !strcmp((const char *)rec[i].ssid, cfg->ssid[k]) && !seen[k]) {
                        seen[k] = true;
                        nseen++;
                    }
        } else {
            esp_wifi_clear_ap_list();
        }
        free(rec);
    }
    if (*quit) return false;
    /* réseaux vus, dans l'ordre de préférence ; aucun vu : tous (réseau masqué) */
    int order[WRX_NETS], no = 0;
    for (int k = 0; k < WRX_NETS; k++) if (cfg->ssid[k][0] && (seen[k] || !nseen)) order[no++] = k;
    bool bad_pass = false;
    int last_reason = 0;
    for (int j = 0; j < no && !*quit; j++) {
        int k = order[j];
        cb(false, N_("Connexion au Wi-Fi"), cfg->ssid[k]);
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, cfg->ssid[k], sizeof wc.sta.ssid);
        strlcpy((char *)wc.sta.password, cfg->pass[k], sizeof wc.sta.password);
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wc.sta.listen_interval = 3;              /* modem sleep : réveil toutes les 3 balises */
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        xEventGroupClearBits(s_evg, BIT_IP | BIT_DISC);
        s_reason = 0;
        esp_wifi_connect();
        EventBits_t b = wait_bits(BIT_IP | BIT_DISC, quit, 15000);
        if (b & BIT_IP) {
            ESP_LOGI(TAG, "Wi-Fi « %s » connecté", cfg->ssid[k]);
            return true;
        }
        int r = s_reason;
        last_reason = r;
        if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE) bad_pass = true;
        ESP_LOGW(TAG, "Wi-Fi « %s » : échec (raison %d)", cfg->ssid[k], r);
        ble_diag("%s wifi %.16s r%d int %u Ko", s_who, cfg->ssid[k], r,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        esp_wifi_disconnect();
        wait_bits(BIT_DISC, quit, 500);
    }
    if (*quit) return false;
    if (bad_pass) cb(true, N_("Mot de passe Wi-Fi refusé"), "");
    else if (!nseen) cb(true, N_("Aucun réseau connu à portée"), "");
    else {
        char d[32];
        if (last_reason) snprintf(d, sizeof d, _("raison %d"), last_reason);
        else snprintf(d, sizeof d, "%s", _("pas de réponse du réseau"));
        cb(true, N_("Connexion Wi-Fi impossible"), d);
    }
    return false;
}
