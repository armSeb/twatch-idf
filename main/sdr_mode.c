/* sdr_mode.c — voir sdr_mode.h */
#include "sdr_mode.h"
#include <stdint.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "heap_memory_layout.h"
#include "multi_heap.h"
#include "freertos/FreeRTOS.h"
#include "ble.h"
#include "settings.h"
#include "i18n.h"

static const char *TAG = "sdr_mode";

/* bancs 7 et 8 de la SRAM1 (la table mémoire d'ESP-IDF les signale comme
 * utilisables pour le « MAC dump ») : hors du tas général */
SOC_RESERVE_MEMORY_REGION(SDR_DUMP_START, SDR_DUMP_END, twatch_rf_dump);

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static multi_heap_handle_t s_heap;         /* NULL pendant le SDR */
static size_t s_empty;                     /* libre quand rien n'est alloué */
static bool s_active, s_ble_was;
static const char *s_err;

/* app à rouvrir au prochain démarrage (après les réglages WebRX…) */
#define APP_MAGIC 0xA9900000u
static RTC_NOINIT_ATTR uint32_t s_app_req;
static int s_app = -1;

static void bank_register(void)
{
    multi_heap_handle_t h = multi_heap_register((void *)SDR_DUMP_START, SDR_DUMP_END - SDR_DUMP_START);
    portENTER_CRITICAL(&s_mux);
    s_heap = h;
    s_empty = h ? multi_heap_free_size(h) : 0;
    portEXIT_CRITICAL(&s_mux);
    if (!h) ESP_LOGE(TAG, "tas des bancs non créé");
}

void sdr_mode_boot(void)
{
    if ((s_app_req & 0xFFFFFF00u) == APP_MAGIC && esp_reset_reason() == ESP_RST_SW) s_app = (int)(s_app_req & 0xFF);
    s_app_req = 0;
    bank_register();
}

bool sdr_mode_active(void) { return s_active; }
const char *sdr_mode_error(void) { return s_err; }

esp_err_t sdr_mode_enter(void)
{
    if (s_active) return ESP_OK;
    s_err = NULL;
    wifi_mode_t m;
    if (esp_wifi_get_mode(&m) != ESP_ERR_WIFI_NOT_INIT) {
        s_err = N_("Wi-Fi occupé par une autre app");
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_mux);
    bool busy = s_heap && multi_heap_free_size(s_heap) != s_empty;
    if (!busy) s_heap = NULL;              /* plus d'allocation dans les bancs */
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        s_err = N_("Mémoire radio occupée (Radio en cours ?)");
        return ESP_ERR_INVALID_STATE;
    }
    s_ble_was = ble_running();
    if (s_ble_was) ble_stop();
    if (ble_running()) {
        bank_register();
        s_err = N_("Bluetooth impossible à arrêter");
        return ESP_ERR_INVALID_STATE;
    }
    s_active = true;
    ESP_LOGI(TAG, "radio prêtée au SDR (Bluetooth %s)", s_ble_was ? "arrêté" : "déjà coupé");
    return ESP_OK;
}

void sdr_mode_leave(void)
{
    if (!s_active) return;
    s_active = false;
    bank_register();                       /* tas neuf : le MAC a tout écrasé */
    if (s_ble_was && settings_get()->ble_enabled) ble_start();
    s_ble_was = false;
    ESP_LOGI(TAG, "radio rendue");
}

void *rfbank_malloc(size_t n)
{
    void *p = NULL;
    portENTER_CRITICAL(&s_mux);
    if (s_heap) p = multi_heap_malloc(s_heap, n);
    portEXIT_CRITICAL(&s_mux);
    return p;
}

bool rfbank_free(void *p)
{
    if ((uintptr_t)p < SDR_DUMP_START || (uintptr_t)p >= SDR_DUMP_END) return false;
    portENTER_CRITICAL(&s_mux);
    if (s_heap) multi_heap_free(s_heap, p);   /* sinon : tas déjà abandonné */
    portEXIT_CRITICAL(&s_mux);
    return true;
}

size_t rfbank_free_size(void)
{
    portENTER_CRITICAL(&s_mux);
    size_t n = s_heap ? multi_heap_free_size(s_heap) : 0;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void sdr_mode_set_boot_app(int idx) { s_app_req = idx >= 0 ? (APP_MAGIC | (uint32_t)(idx & 0xFF)) : 0; }
int  sdr_mode_boot_app(void) { return s_app; }
