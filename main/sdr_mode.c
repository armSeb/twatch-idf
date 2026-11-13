/* sdr_mode.c — voir sdr_mode.h */
#include "sdr_mode.h"
#include <stdint.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "heap_memory_layout.h"
#include "esp_ota_ops.h"

static const char *TAG = "sdr_mode";

/* bancs 7 et 8 de la SRAM1 (la table mémoire d'ESP-IDF les signale comme
 * utilisables pour le « MAC dump ») */
SOC_RESERVE_MEMORY_REGION(SDR_DUMP_START, SDR_DUMP_END, twatch_rf_dump);

#define SDR_MAGIC 0x5D2B007Au
static RTC_NOINIT_ATTR uint32_t s_req;
static bool s_active;

/* app à rouvrir au prochain démarrage (après les réglages WebRX…) */
#define APP_MAGIC 0xA9900000u
static RTC_NOINIT_ATTR uint32_t s_app_req;
static int s_app = -1;

void sdr_mode_boot(void)
{
    s_active = s_req == SDR_MAGIC && esp_reset_reason() == ESP_RST_SW;
    s_req = 0;                                   /* un plantage ramène au mode normal */
    if ((s_app_req & 0xFFFFFF00u) == APP_MAGIC && esp_reset_reason() == ESP_RST_SW) s_app = (int)(s_app_req & 0xFF);
    s_app_req = 0;
    if (!s_active) {
        esp_err_t e = heap_caps_add_region(SDR_DUMP_START, SDR_DUMP_END);
        if (e != ESP_OK) ESP_LOGE(TAG, "bancs SDR non rendus au tas : %s", esp_err_to_name(e));
    } else {
        ESP_LOGW(TAG, "mode SDR : Bluetooth coupé, bancs 0x%08x-0x%08x réservés à la radio",
                 (unsigned)SDR_DUMP_START, (unsigned)SDR_DUMP_END);
    }
}

bool sdr_mode_active(void) { return s_active; }

void sdr_mode_reboot(bool sdr)
{
    /* une mise à jour encore « à l'essai » serait annulée par ce redémarrage
     * volontaire : l'utilisateur s'en sert, on la valide */
    esp_ota_mark_app_valid_cancel_rollback();
    s_req = sdr ? SDR_MAGIC : 0;
    esp_restart();
}

void sdr_mode_set_boot_app(int idx) { s_app_req = idx >= 0 ? (APP_MAGIC | (uint32_t)(idx & 0xFF)) : 0; }
int  sdr_mode_boot_app(void) { return s_active ? -1 : s_app; }
