/* motion.c — voir motion.h */
#include "motion.h"
#include <string.h>
#include <time.h>
#include "bma423.h"
#include "board.h"
#include "i2c_bus.h"
#include "settings.h"
#include "wclock.h"
#include "app_events.h"
#include "ble.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motion";

#define SAVE_PERIOD_US (15LL * 60 * 1000000)
#define READ_CACHE_US  (2LL * 1000000)

static i2c_master_dev_handle_t s_i2c;
static struct bma4_dev s_dev;
static bool s_loaded;                  /* microcode chargé depuis le démarrage */
static bool s_steps, s_wrist;          /* fonctions actuellement actives */
static bool s_isr;

/* pas */
static uint32_t s_day;                 /* AAAAMMJJ local du compteur, 0 = inconnu */
static uint32_t s_base;                /* pas du jour hors compteur du capteur */
static uint32_t s_hist[MOTION_HIST_DAYS];
static uint32_t s_saved = UINT32_MAX;  /* total du jour à la dernière sauvegarde */
static int64_t  s_save_us;
static uint32_t s_cache; static int64_t s_cache_us = -1;
static uint32_t s_last;               /* dernière lecture réussie du compteur */

/* ------------------------------------------------------------------ */
/* bus pour le pilote Bosch                                            */
/* ------------------------------------------------------------------ */
static int8_t bus_read(uint8_t reg, uint8_t *d, uint32_t len, void *p)
{
    return i2c_master_transmit_receive(s_i2c, &reg, 1, d, len, 100) == ESP_OK ? 0 : -1;
}

static int8_t bus_write(uint8_t reg, const uint8_t *d, uint32_t len, void *p)
{
    i2c_master_transmit_multi_buffer_info_t b[2] = {
        { .write_buffer = &reg, .buffer_size = 1 },
        { .write_buffer = (uint8_t *)d, .buffer_size = len },
    };
    return i2c_master_multi_buffer_transmit(s_i2c, b, 2, 100) == ESP_OK ? 0 : -1;
}

static void bus_delay(uint32_t us, void *p)
{
    if (us >= 1000) vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
    else esp_rom_delay_us(us);
}

/* ------------------------------------------------------------------ */
/* persistance                                                          */
/* ------------------------------------------------------------------ */
static uint32_t today_key(void)
{
    if (!wclock_time_valid()) return 0;
    struct tm t;
    wclock_local_tm(&t);
    return (uint32_t)((t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday);
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open("motion", NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u32(h, "day", &s_day);
    nvs_get_u32(h, "today", &s_base);
    size_t n = sizeof s_hist;
    nvs_get_blob(h, "hist", s_hist, &n);
    nvs_close(h);
    s_saved = s_base;
}

static uint32_t sensor_steps(void)
{
    if (!s_steps) return 0;
    uint32_t v = 0;
    if (bma423_step_counter_output(&v, &s_dev) != BMA4_OK) return s_last;   /* I2C raté : valeur précédente */
    s_last = v;
    return v;
}

static void save(void)
{
    uint32_t total = s_base + sensor_steps();
    nvs_handle_t h;
    if (nvs_open("motion", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "day", s_day);
    nvs_set_u32(h, "today", total);
    nvs_set_blob(h, "hist", s_hist, sizeof s_hist);
    nvs_commit(h);
    nvs_close(h);
    s_saved = total;
    s_save_us = esp_timer_get_time();
}

static void shutdown_save(void)
{
    if (s_steps && s_base + sensor_steps() != s_saved) save();
}

/* jour écoulé : on pousse le total dans l'historique (autant de jours que
 * nécessaire si la montre est restée éteinte) et on repart de zéro */
static void rollover(uint32_t today)
{
    if (s_day == today) return;
    if (s_day == 0) { s_day = today; return; }   /* première date connue */
    uint32_t total = s_base + sensor_steps();
    struct tm a = { .tm_year = (int)(s_day / 10000) - 1900, .tm_mon = (int)(s_day / 100 % 100) - 1,
                    .tm_mday = (int)(s_day % 100), .tm_hour = 12 };
    struct tm b = { .tm_year = (int)(today / 10000) - 1900, .tm_mon = (int)(today / 100 % 100) - 1,
                    .tm_mday = (int)(today % 100), .tm_hour = 12 };
    long days = (long)((mktime(&b) - mktime(&a)) / 86400);
    if (days < 1) days = 1;                      /* heure reculée : on ne garde pas l'ancien jour */
    for (int k = 0; k < days && k < MOTION_HIST_DAYS; k++) {
        memmove(&s_hist[1], &s_hist[0], (MOTION_HIST_DAYS - 1) * sizeof s_hist[0]);
        s_hist[0] = k == 0 ? total : 0;
    }
    if (days >= MOTION_HIST_DAYS + 1) memset(s_hist, 0, sizeof s_hist);
    if (s_steps) bma423_reset_step_counter(&s_dev);
    s_base = 0;
    s_last = 0;
    s_day = today;
    s_cache_us = -1;
    save();
    ESP_LOGI(TAG, "nouveau jour %lu (hier %lu pas)", (unsigned long)today, (unsigned long)s_hist[0]);
}

/* ------------------------------------------------------------------ */
/* capteur                                                              */
/* ------------------------------------------------------------------ */
static void IRAM_ATTR bma_isr(void *arg)
{
    gpio_intr_disable(BOARD_BMA_INT1);
    BaseType_t hp = pdFALSE;
    app_event_t ev = { .type = EV_MOTION };
    app_post_from_isr(&ev, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void suspend(void)
{
    /* INT1 → GPIO39, qui a un tirage de 10 kΩ vers la masse sur la carte
     * (R10) : la ligne ne flotte pas. On garde quand même INT1 en sortie
     * push-pull active haute, sans interruption routée : niveau bas imposé,
     * aucun courant dans R10. Registre conservé en suspend. */
    i2c_reg_write8(s_i2c, 0x58, 0x00);         /* INT_MAP_DATA : rien sur INT1/INT2 */
    i2c_reg_write8(s_i2c, 0x56, 0x00);         /* INT1_MAP : aucune fonction */
    i2c_reg_write8(s_i2c, 0x53, 0x0A);         /* INT1_IO_CTRL : sortie, push-pull, active haute */
    i2c_reg_write8(s_i2c, 0x7D, 0x00);         /* PWR_CTRL : accéléro OFF */
    vTaskDelay(pdMS_TO_TICKS(2));
    i2c_reg_write8(s_i2c, 0x7C, 0x01);         /* PWR_CONF : adv_power_save */
}

static bool load_microcode(void)
{
    memset(&s_dev, 0, sizeof s_dev);
    s_dev.intf = BMA4_I2C_INTF;
    s_dev.variant = BMA42X_VARIANT;
    s_dev.bus_read = bus_read;
    s_dev.bus_write = bus_write;
    s_dev.delay_us = bus_delay;
    s_dev.read_write_len = 64;                 /* morceaux du microcode (≤ 70, pair) */
    s_dev.intf_ptr = &s_i2c;

    int64_t t0 = esp_timer_get_time();
    int8_t r = BMA4_OK;
    const char *step = "";
    for (int tries = 0; tries < 3; tries++) {
        /* la puce se réinitialise pendant la commande : elle peut ne pas
         * acquitter la fin de l'écriture, le résultat est donc ignoré */
        bma4_soft_reset(&s_dev);
        vTaskDelay(pdMS_TO_TICKS(10));
        step = "id";
        r = bma423_init(&s_dev);
        if (r == BMA4_OK) { step = "microcode"; r = bma423_write_config_file(&s_dev); }
        if (r == BMA4_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (r != BMA4_OK) {
        uint8_t st = 0xFF;
        i2c_reg_read8(s_i2c, 0x2A, &st);                  /* INTERNAL_STATUS */
        ESP_LOGE(TAG, "microcode BMA423 non chargé (%s %d, id 0x%02x, état 0x%02x)", step, r, s_dev.chip_id, st);
        ble_diag("BMA %s e%d id%02x st%02x", step, r, s_dev.chip_id, st);
        return false;
    }
    /* T-Watch 2020 V1 : orientation du capteur sur la carte */
    struct bma423_axes_remap m = { .x_axis = 0, .x_axis_sign = 1, .y_axis = 1, .y_axis_sign = 0,
                                   .z_axis = 2, .z_axis_sign = 1 };
    bma423_set_remap_axes(&m, &s_dev);
    struct bma4_accel_config c = { .odr = BMA4_OUTPUT_DATA_RATE_50HZ, .range = BMA4_ACCEL_RANGE_2G,
                                   .bandwidth = BMA4_ACCEL_NORMAL_AVG4, .perf_mode = BMA4_CIC_AVG_MODE };
    bma4_set_accel_config(&c, &s_dev);
    struct bma4_int_pin_config p = { .edge_ctrl = BMA4_LEVEL_TRIGGER, .lvl = BMA4_ACTIVE_HIGH,
                                     .od = BMA4_PUSH_PULL, .output_en = BMA4_OUTPUT_ENABLE,
                                     .input_en = BMA4_INPUT_DISABLE };
    bma4_set_int_pin_config(&p, BMA4_INTR1_MAP, &s_dev);
    bma4_set_interrupt_mode(BMA4_LATCH_MODE, &s_dev);   /* tenu jusqu'à lecture du statut */
    ESP_LOGI(TAG, "microcode BMA423 chargé en %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

static void isr_setup(bool on)
{
    if (on && !s_isr) {
        gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_BMA_INT1, .mode = GPIO_MODE_INPUT,
                             .intr_type = GPIO_INTR_HIGH_LEVEL };
        gpio_config(&io);
        gpio_isr_handler_add(BOARD_BMA_INT1, bma_isr, NULL);   /* service installé par power_init */
        gpio_wakeup_enable(BOARD_BMA_INT1, GPIO_INTR_HIGH_LEVEL);
        s_isr = true;
    } else if (!on && s_isr) {
        gpio_wakeup_disable(BOARD_BMA_INT1);
        gpio_isr_handler_remove(BOARD_BMA_INT1);
        gpio_intr_disable(BOARD_BMA_INT1);
        s_isr = false;
    }
}

void motion_apply(void)
{
    if (!s_i2c) return;
    settings_t *st = settings_get();
    bool steps = st->step_count, wrist = st->wrist_wake;

    if (s_steps && !steps) {                   /* on fige le total du jour */
        s_base += sensor_steps();
        bma423_reset_step_counter(&s_dev);
        s_steps = false;
        s_last = 0;
        save();
    }
    if (!steps && !wrist) {
        if (s_loaded) {
            bma423_feature_enable(BMA423_STEP_CNTR | BMA423_WRIST_WEAR, BMA4_DISABLE, &s_dev);
            bma423_map_interrupt(BMA4_INTR1_MAP, BMA423_WRIST_WEAR_INT, BMA4_DISABLE, &s_dev);
        }
        isr_setup(false);
        suspend();
        s_wrist = false;
        ESP_LOGI(TAG, "BMA423 en suspend");
        return;
    }
    if (!s_loaded) {
        s_loaded = load_microcode();
        if (!s_loaded) { suspend(); return; }
    }
    bma4_set_accel_enable(BMA4_ENABLE, &s_dev);
    if (steps && !s_steps) { bma423_reset_step_counter(&s_dev); s_last = 0; }   /* total = s_base + compteur */
    bma423_feature_enable(BMA423_STEP_CNTR, steps ? BMA4_ENABLE : BMA4_DISABLE, &s_dev);
    bma423_feature_enable(BMA423_WRIST_WEAR, wrist ? BMA4_ENABLE : BMA4_DISABLE, &s_dev);
    bma423_map_interrupt(BMA4_INTR1_MAP, BMA423_WRIST_WEAR_INT, wrist ? BMA4_ENABLE : BMA4_DISABLE, &s_dev);
    uint16_t st16;
    bma423_read_int_status(&st16, &s_dev);     /* efface un geste ancien */
    bma4_set_advance_power_save(BMA4_ENABLE, &s_dev);
    s_steps = steps;
    s_wrist = wrist;
    s_cache_us = -1;
    isr_setup(wrist);
    if (wrist) gpio_intr_enable(BOARD_BMA_INT1);
    ESP_LOGI(TAG, "pas %s, poignet %s", steps ? "oui" : "non", wrist ? "oui" : "non");
}

esp_err_t motion_init(void)
{
    esp_err_t e = i2c_bus_add(I2C_BUS_SENSORS, BOARD_BMA423_ADDR, &s_i2c);
    if (e != ESP_OK) { s_i2c = NULL; return e; }
    load();
    esp_register_shutdown_handler(shutdown_save);
    motion_apply();
    return ESP_OK;
}

bool motion_on_irq(void)
{
    uint16_t st = 0;
    if (s_loaded) bma423_read_int_status(&st, &s_dev);  /* lit et libère la ligne */
    if (s_isr) gpio_intr_enable(BOARD_BMA_INT1);
    return s_wrist && (st & BMA423_WRIST_WEAR_INT);
}

/* ------------------------------------------------------------------ */
/* Confirmation du geste : le moteur Bosch n'a pas de réglage de       */
/* sensibilité. On vérifie que l'écran regarde vers le haut (gravité   */
/* surtout sur l'axe Z, signe indifférent) et que la montre est tenue  */
/* immobile un instant ; en sensibilité basse, une montre posée à plat */
/* (bras sur la table) est aussi refusée. 2 g, 12 bits : 1024 = 1 g.   */
/* ------------------------------------------------------------------ */
typedef struct { int samples, gap_ms, zmin, zmax, still; } conf_t;
static const conf_t CONF[2] = {
    { 3, 220, 785, 1000, 150 },   /* basse : < 38° de l'horizontale, pas à plat (> 12°), immobile */
    { 2, 250, 640, 2048, 260 },   /* moyenne : < 50°, presque immobile */
};

static int iabs(int v) { return v < 0 ? -v : v; }

bool motion_wrist_confirm(int sens)
{
    if (sens >= 2 || !s_loaded) return true;
    const conf_t *c = &CONF[sens < 0 ? 0 : sens];
    struct bma4_accel a, prev = { 0 };
    for (int i = 0; i < c->samples; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(c->gap_ms));
        if (bma4_read_accel_xyz(&a, &s_dev) != BMA4_OK) return true;   /* doute : on allume */
        int z = iabs(a.z);
        int ok = z >= c->zmin && z <= c->zmax;
        if (ok && i && (iabs(a.x - prev.x) > c->still || iabs(a.y - prev.y) > c->still ||
                        iabs(a.z - prev.z) > c->still)) ok = 0;
        if (!ok) {
            ESP_LOGI(TAG, "geste refusé (x %d y %d z %d, échantillon %d)", a.x, a.y, a.z, i);
            return false;
        }
        prev = a;
    }
    return true;
}

void motion_tick(void)
{
    if (!s_i2c) return;
    uint32_t today = today_key();
    if (s_steps && today) rollover(today);
    if (s_steps && esp_timer_get_time() - s_save_us >= SAVE_PERIOD_US) {
        if (s_base + sensor_steps() != s_saved) save();
        else s_save_us = esp_timer_get_time();
    }
}

bool motion_ok(void) { return s_loaded; }
bool motion_steps_on(void) { return s_steps; }

uint32_t motion_steps_today(void)
{
    int64_t now = esp_timer_get_time();
    if (s_cache_us >= 0 && now - s_cache_us < READ_CACHE_US) return s_cache;
    uint32_t today = today_key();
    if (s_steps && today) rollover(today);
    s_cache = s_base + sensor_steps();
    s_cache_us = now;
    return s_cache;
}

void motion_history(uint32_t h[MOTION_HIST_DAYS])
{
    uint32_t today = today_key();
    if (s_steps && today) rollover(today);
    memcpy(h, s_hist, sizeof s_hist);
}
