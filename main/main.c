/*
 * T-Watch 2020 V1 — firmware ESP-IDF + LVGL 9 + NimBLE (ANCS)
 *
 * Tâches :
 *   - « app » (cœur 1) : seule propriétaire de LVGL, de l'UI et du stockage
 *     des notifications. Bloquée sur sa file d'événements quand l'écran est
 *     éteint (réveil au plus toutes les 60 s pour la batterie).
 *   - hôte NimBLE (cœur 0) : GAP/GATT/ANCS, ne fait que poster des événements.
 *   - esp_timer : motif du vibreur uniquement.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "app_events.h"
#include "settings.h"
#include "i2c_bus.h"
#include "axp202.h"
#include "power.h"
#include "wclock.h"
#include "display.h"
#include "ui.h"
#include "ble.h"
#include "ancs.h"
#include "notif_store.h"

#ifndef TW_DIAG_BARE
#define TW_DIAG_BARE 0
#endif
#ifndef TW_DIAG_BLE_ONLY
#define TW_DIAG_BLE_ONLY TW_DIAG_BARE
#endif

static const char *TAG = "app";
static QueueHandle_t s_app_q;

#define BATT_PERIOD_ON_US   (10LL * 1000000)
#define BATT_PERIOD_OFF_US  (60LL * 1000000)
#define IDLE_WAIT_OFF_MS    60000
#define MAX_WAIT_ON_MS      50

bool app_post(const app_event_t *ev)
{
    if (!s_app_q) return false;
    if (xQueueSend(s_app_q, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "file app pleine (ev %d)", ev->type);
        return false;
    }
    return true;
}

bool app_post_from_isr(const app_event_t *ev, BaseType_t *hp)
{
    return s_app_q && xQueueSendFromISR(s_app_q, ev, hp) == pdTRUE;
}

static void screen_wake(void)
{
    if (!display_is_on()) {
        wclock_resync();                 /* l'heure affichée vient de la RTC */
        ui_on_wake();
        display_set_on(true);
    }
    lv_display_trigger_activity(NULL);
}

static void handle_axp(void)
{
    uint64_t m = power_handle_irq();
    if (m & AXP_IRQ_PEK_SHORT) {
        if (!display_is_on()) {
            if (!ui_open_pending()) ui_go_home();   /* dernière notif non vue, sinon cadran */
            screen_wake();
        } else if (!ui_is_home()) {
            ui_go_home();
            lv_display_trigger_activity(NULL);
        } else {
            display_set_on(false);
        }
    }
    if (m & AXP_IRQ_PEK_LONG) {
        screen_wake();
    }
    if (m & (AXP_IRQ_VBUS_CONNECT | AXP_IRQ_VBUS_REMOVED | AXP_IRQ_CHG_START | AXP_IRQ_CHG_DONE)) {
        power_update();
        ui_on_power();
        if (m & (AXP_IRQ_VBUS_CONNECT | AXP_IRQ_VBUS_REMOVED)) {
            screen_wake();
            if (settings_get()->vibrate) power_vibrate(VIB_SHORT);
        }
    }
    if (m & (AXP_IRQ_LOWBAT_L1 | AXP_IRQ_LOWBAT_L2)) {
        ESP_LOGW(TAG, "batterie faible (AXP)");
        power_update();
        ui_on_power();
    }
}

static void handle_notifs(void)
{
    static ancs_notif_t n;     /* 450 o : hors pile */
    while (xQueueReceive(g_notif_queue, &n, 0) == pdTRUE) {
        bool alert = false;
        notif_entry_t *e = ns_add(&n, &alert);
        ESP_LOGI(TAG, "notif [%s] « %s » flags=0x%02x alerte=%d (%d en mémoire)",
                 e->n.app_name, e->n.title, e->n.flags, alert, ns_count());
        if (ui_on_notif(e, alert)) screen_wake();   /* e n'est plus valide après */
    }
}

static void handle_event(const app_event_t *ev)
{
    switch (ev->type) {
    case EV_AXP_IRQ:
        handle_axp();
        break;
    case EV_NOTIF:
        handle_notifs();
        break;
    case EV_NOTIF_REMOVED:
        ui_on_notif_removed((uint32_t)ev->a);
        break;
    case EV_BLE_STATE:
        ui_on_ble_state(ev->a);
        break;
    case EV_PASSKEY:
        ui_show_passkey((uint32_t)ev->a);
        screen_wake();
        break;
    case EV_PAIRING_DONE:
        ui_pairing_done(ev->a);
        break;
    case EV_TIME_SYNC: {
        int32_t off = (ev->a != INT32_MIN) ? ev->a : wclock_tz_offset_min();
        wclock_set_tz_offset_min(off);
        wclock_set_utc((time_t)(ev->t - (int64_t)off * 60));
        ui_on_time_changed();
        ESP_LOGI(TAG, "heure synchronisée (décalage %+ld min)", (long)off);
        break;
    }
    default:
        break;
    }
}

static void app_task(void *arg)
{
    ui_on_wake();
    display_set_on(true);
    int64_t last_batt = esp_timer_get_time();

    for (;;) {
        uint32_t wait_ms;
        if (display_is_on()) {
            display_perf_tick();
            uint32_t next = lv_timer_handler();
            if (lv_display_get_inactive_time(NULL) > ui_timeout_ms()) {
                display_set_on(false);
                ui_go_home();            /* sans animation : écran éteint */
                continue;
            }
            wait_ms = (next == LV_NO_TIMER_READY || next > MAX_WAIT_ON_MS) ? MAX_WAIT_ON_MS : next;
            if (wait_ms == 0) wait_ms = 1;
        } else {
            wait_ms = IDLE_WAIT_OFF_MS;
        }

        app_event_t ev;
        if (xQueueReceive(s_app_q, &ev, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            do {
                handle_event(&ev);
            } while (xQueueReceive(s_app_q, &ev, 0) == pdTRUE);
        }

        int64_t now = esp_timer_get_time();
        if (now - last_batt >= (display_is_on() ? BATT_PERIOD_ON_US : BATT_PERIOD_OFF_US)) {
            last_batt = now;
            wclock_resync();
            power_update();
            ble_set_battery((uint8_t)power_percent());
            ui_on_power();
        }
    }
}

#if TW_DIAG_BLE_ONLY
/* Build de diagnostic : BLE + ANCS seuls, sans écran / LVGL / tactile. */
static void app_task_diag(void *arg)
{
    for (;;) {
        app_event_t ev;
        if (xQueueReceive(s_app_q, &ev, pdMS_TO_TICKS(60000)) != pdTRUE) continue;
        switch (ev.type) {
        case EV_AXP_IRQ: if (!TW_DIAG_BARE) power_handle_irq(); break;
        case EV_NOTIF: {
            static ancs_notif_t n;
            while (xQueueReceive(g_notif_queue, &n, 0) == pdTRUE) {
                bool alert;
                notif_entry_t *e = ns_add(&n, &alert);
                ESP_LOGI(TAG, "notif [%s] « %s » alerte=%d", e->n.app_name, e->n.title, alert);
            }
            break;
        }
        case EV_BLE_STATE: ESP_LOGI(TAG, "état BLE %ld", (long)ev.a); break;
        case EV_TIME_SYNC: {
            if (TW_DIAG_BARE) { ESP_LOGI(TAG, "CTS reçu (non appliqué)"); break; }
            int32_t off = (ev.a != INT32_MIN) ? ev.a : wclock_tz_offset_min();
            wclock_set_tz_offset_min(off);
            wclock_set_utc((time_t)(ev.t - (int64_t)off * 60));
            break;
        }
        default: break;
        }
    }
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings_load();
#if CONFIG_BT_NIMBLE_LOG_LEVEL == 0
    esp_log_level_set("NimBLE", ESP_LOG_DEBUG);   /* build de diagnostic SMP */
#endif

    s_app_q = xQueueCreate(24, sizeof(app_event_t));
    assert(s_app_q);

#if TW_DIAG_BARE
    ESP_LOGW(TAG, "DIAGNOSTIC NU : aucun I2C/AXP/GPIO, BLE seul");
#else
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(power_init());
    if (wclock_init() != ESP_OK) ESP_LOGE(TAG, "RTC indisponible");
#endif
    ancs_module_init();
#if !TW_DIAG_BLE_ONLY
    ESP_ERROR_CHECK(display_init());
    ui_init();
#else
    ESP_LOGW(TAG, "DIAGNOSTIC : BLE seul, écran non initialisé");
#endif

    if (settings_get()->ble_enabled) {
        ble_start();
    } else {
        ESP_LOGW(TAG, "mode avion : BLE non démarré");
    }
#if !TW_DIAG_BARE
    ble_set_battery((uint8_t)power_percent());
#endif

    ESP_LOGI(TAG, "tas interne libre %u o (plus grand bloc %u o)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

#if TW_DIAG_BLE_ONLY
    xTaskCreatePinnedToCore(app_task_diag, "app", 6144, NULL, 5, NULL, 1);
#else
    xTaskCreatePinnedToCore(app_task, "app", 8192, NULL, 5, NULL, 1);
#endif
}
