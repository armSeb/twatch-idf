#include "power.h"
#include <string.h>
#include "axp202.h"
#include "i2c_bus.h"
#include "board.h"
#include "app_events.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_lock_cpu, s_lock_nosleep;
#endif
static int s_cnt_active, s_cnt_nosleep;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static float s_ocv_filt = -1;
static int s_percent = -1;
static bool s_vbus, s_charging;
static int64_t s_period_start_us;

/* ------------------------------------------------------------------ */
/* Courbe OCV LiPo (repos, 25 °C) → %. Interpolation linéaire.         */
/* ------------------------------------------------------------------ */
static const struct { uint16_t mv; uint8_t pct; } OCV[] = {
    {3300, 0}, {3610, 5}, {3690, 10}, {3710, 15}, {3730, 20}, {3750, 25},
    {3770, 30}, {3790, 35}, {3800, 40}, {3820, 45}, {3840, 50}, {3850, 55},
    {3870, 60}, {3910, 65}, {3950, 70}, {3980, 75}, {4020, 80}, {4080, 85},
    {4110, 90}, {4150, 95}, {4190, 100},
};
#define R_INT_OHM 0.20f   /* cellule + protection + connecteur, ordre de grandeur */

static int ocv_to_pct(float mv)
{
    const int n = sizeof(OCV) / sizeof(OCV[0]);
    if (mv <= OCV[0].mv) return 0;
    if (mv >= OCV[n - 1].mv) return 100;
    for (int i = 1; i < n; i++) {
        if (mv < OCV[i].mv) {
            float f = (mv - OCV[i - 1].mv) / (float)(OCV[i].mv - OCV[i - 1].mv);
            return (int)(OCV[i - 1].pct + f * (OCV[i].pct - OCV[i - 1].pct) + 0.5f);
        }
    }
    return 100;
}

/* ------------------------------------------------------------------ */
/* Verrous PM                                                          */
/* ------------------------------------------------------------------ */
void power_hold_active(bool hold)
{
    portENTER_CRITICAL(&s_mux);
    bool acquire = hold && (s_cnt_active++ == 0);
    bool release = !hold && s_cnt_active > 0 && (--s_cnt_active == 0);
    portEXIT_CRITICAL(&s_mux);
#if CONFIG_PM_ENABLE
    if (acquire) { esp_pm_lock_acquire(s_lock_cpu); power_hold_nosleep(true); }
    if (release) { esp_pm_lock_release(s_lock_cpu); power_hold_nosleep(false); }
#else
    (void)acquire; (void)release;
#endif
}

void power_hold_nosleep(bool hold)
{
    portENTER_CRITICAL(&s_mux);
    bool acquire = hold && (s_cnt_nosleep++ == 0);
    bool release = !hold && s_cnt_nosleep > 0 && (--s_cnt_nosleep == 0);
    portEXIT_CRITICAL(&s_mux);
#if CONFIG_PM_ENABLE
    if (acquire) esp_pm_lock_acquire(s_lock_nosleep);
    if (release) esp_pm_lock_release(s_lock_nosleep);
#else
    (void)acquire; (void)release;
#endif
}

/* ------------------------------------------------------------------ */
/* IRQ AXP202 : niveau bas sur GPIO35. L'ISR masque la ligne et poste  */
/* un événement ; la tâche app lit/acquitte via I2C puis démasque.     */
/* Niveau (et non front) : aucune IRQ perdue pendant un light sleep.   */
/* ------------------------------------------------------------------ */
static void IRAM_ATTR axp_isr(void *arg)
{
    gpio_intr_disable(BOARD_AXP_INT);
    BaseType_t hp = pdFALSE;
    app_event_t ev = { .type = EV_AXP_IRQ };
    app_post_from_isr(&ev, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* ------------------------------------------------------------------ */
/* Ventilation écran allumé / veille (compteur coulométrique)           */
/* ------------------------------------------------------------------ */
static bool    s_seg_on;
static float   s_seg_mah, s_on_mah, s_off_mah;
static int64_t s_seg_us, s_on_us, s_off_us;

static void meter_reset(void)
{
    s_seg_mah = 0; s_on_mah = s_off_mah = 0;
    s_on_us = s_off_us = 0;
    s_seg_us = esp_timer_get_time();
}

void power_screen_segment(bool on)
{
    int64_t now = esp_timer_get_time();
    float mah = axp_coulomb_used_mah();
    if (s_seg_on) { s_on_mah += mah - s_seg_mah; s_on_us += now - s_seg_us; }
    else          { s_off_mah += mah - s_seg_mah; s_off_us += now - s_seg_us; }
    s_seg_mah = mah; s_seg_us = now; s_seg_on = on;
}

uint64_t power_handle_irq(void)
{
    uint64_t m = axp_irq_read_and_clear();
    if (m & (AXP_IRQ_VBUS_CONNECT | AXP_IRQ_VBUS_REMOVED | AXP_IRQ_CHG_START | AXP_IRQ_CHG_DONE)) {
        bool was_vbus = s_vbus;
        s_vbus = axp_vbus_present();
        s_charging = axp_is_charging();
        if (was_vbus && !s_vbus) {
            /* début d'une période sur batterie : on remet la mesure à zéro */
            axp_coulomb_reset();
            s_period_start_us = esp_timer_get_time();
            meter_reset();
            s_ocv_filt = -1;   /* tension de charge non représentative */
        }
        if (m & AXP_IRQ_CHG_DONE) s_percent = 100;
    }
    gpio_intr_enable(BOARD_AXP_INT);
    return m;
}

/* ------------------------------------------------------------------ */
/* Vibreur : petit lecteur de motif sur esp_timer                       */
/* ------------------------------------------------------------------ */
const uint16_t VIB_SHORT[] = { 60, 0 };
const uint16_t VIB_NOTIF[] = { 120, 120, 120, 0 };
const uint16_t VIB_CALL[]  = { 500, 300, 500, 300, 500, 0 };

static esp_timer_handle_t s_vib_timer;
static const uint16_t *s_vib_pat;
static int s_vib_idx;
static bool s_vib_busy;

static void vib_step(void *arg)
{
    uint16_t d = s_vib_pat ? s_vib_pat[s_vib_idx] : 0;
    if (d == 0) {
        gpio_set_level(BOARD_MOTOR, 0);
        s_vib_pat = NULL;
        if (s_vib_busy) { s_vib_busy = false; power_hold_nosleep(false); }
        return;
    }
    gpio_set_level(BOARD_MOTOR, (s_vib_idx & 1) ? 0 : 1);
    s_vib_idx++;
    esp_timer_start_once(s_vib_timer, (uint64_t)d * 1000);
}

void power_vibrate(const uint16_t *pattern)
{
    /* appelé depuis la tâche app uniquement */
    esp_timer_stop(s_vib_timer);
    if (!s_vib_busy) { s_vib_busy = true; power_hold_nosleep(true); }
    s_vib_pat = pattern;
    s_vib_idx = 0;
    vib_step(NULL);
}

/* ------------------------------------------------------------------ */
/* Rétroéclairage : LEDC sur REF_TICK (1 MHz, indépendant du DFS)       */
/* ------------------------------------------------------------------ */
static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_USE_REF_TICK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void power_backlight(uint8_t level)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ------------------------------------------------------------------ */
/* BMA423 : on le met en suspend (~2,5 µA au lieu de ~500 µA)           */
/* ------------------------------------------------------------------ */
static void bma423_suspend(void)
{
    i2c_master_dev_handle_t bma;
    if (i2c_bus_add(I2C_BUS_SENSORS, BOARD_BMA423_ADDR, &bma) != ESP_OK) return;
    uint8_t id = 0;
    i2c_reg_read8(bma, 0x00, &id);
    i2c_reg_write8(bma, 0x7D, 0x00);         /* PWR_CTRL : accéléro OFF */
    vTaskDelay(pdMS_TO_TICKS(2));
    i2c_reg_write8(bma, 0x7C, 0x01);         /* PWR_CONF : adv_power_save */
    ESP_LOGI(TAG, "BMA423 (id 0x%02x) en suspend", id);
}

/* ------------------------------------------------------------------ */
/* Broches inutilisées : état défini, aucun tirage                      */
/* ------------------------------------------------------------------ */
static void unused_gpio_setup(void)
{
    /* Ampli I2S MAX98357 (BCLK 26, LRCLK 25, DIN 33) et LED IR (13) :
     * sorties à 0. Entrées flottantes = courant de fuite ; LED IR jamais
     * allumée par accident. L'ampli partage LDO3 avec l'écran sur cette
     * montre, on ne peut donc pas couper son alimentation. */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << GPIO_NUM_13) | (1ULL << GPIO_NUM_25) |
                        (1ULL << GPIO_NUM_26) | (1ULL << GPIO_NUM_33),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(GPIO_NUM_13, 0);
    gpio_set_level(GPIO_NUM_25, 0);
    gpio_set_level(GPIO_NUM_26, 0);
    gpio_set_level(GPIO_NUM_33, 0);

    /* Broches non câblées / strapping : entrées sans pull-up ni pull-down */
    const gpio_num_t floating[] = { GPIO_NUM_0, GPIO_NUM_2, GPIO_NUM_14, GPIO_NUM_15,
                                    GPIO_NUM_34, GPIO_NUM_36 };
    for (size_t i = 0; i < sizeof(floating) / sizeof(floating[0]); i++) {
        gpio_set_direction(floating[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(floating[i], GPIO_FLOATING);
    }
}

/* ------------------------------------------------------------------ */
esp_err_t power_init(void)
{
    unused_gpio_setup();
    /* moteur */
    gpio_config_t mo = { .pin_bit_mask = 1ULL << BOARD_MOTOR, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&mo);
    gpio_set_level(BOARD_MOTOR, 0);
    esp_timer_create_args_t ta = { .callback = vib_step, .name = "vib" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_vib_timer));

    backlight_init();
    power_backlight(0);

    ESP_ERROR_CHECK(axp_init());
    bma423_suspend();

    s_vbus = axp_vbus_present();
    s_charging = axp_is_charging();
    axp_coulomb_reset();
    s_period_start_us = esp_timer_get_time();
    meter_reset();

    axp_irq_enable(AXP_IRQ_VBUS_CONNECT | AXP_IRQ_VBUS_REMOVED | AXP_IRQ_CHG_START |
                   AXP_IRQ_CHG_DONE | AXP_IRQ_PEK_SHORT | AXP_IRQ_PEK_LONG |
                   AXP_IRQ_LOWBAT_L1 | AXP_IRQ_LOWBAT_L2);

    /* GPIO35 : IRQ niveau bas + source de réveil du light sleep */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_AXP_INT,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOARD_AXP_INT, axp_isr, NULL);
    gpio_wakeup_enable(BOARD_AXP_INT, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    /* PM : DFS + light sleep auto. Avec le BLE actif sur ESP32 « classique »
     * (horloge BT = quartz principal), le contrôleur garde NO_LIGHT_SLEEP :
     * on obtient DFS + modem sleep. BLE coupé → vrai light sleep. */
#if CONFIG_PM_ENABLE
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "active", &s_lock_cpu));
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "nosleep", &s_lock_nosleep));
    esp_pm_config_t pm = {
        .max_freq_mhz = POWER_CPU_MAX_MHZ,
        .min_freq_mhz = POWER_CPU_MIN_MHZ,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure: %s", esp_err_to_name(err));
    }

#else
    ESP_LOGW(TAG, "PM désactivé (build de diagnostic)");
#endif

    power_update();
    ESP_LOGI(TAG, "PM %d-%d MHz, batterie %d %%", POWER_CPU_MIN_MHZ, POWER_CPU_MAX_MHZ, s_percent);
    return ESP_OK;
}

void power_update(void)
{
    s_vbus = axp_vbus_present();
    s_charging = axp_is_charging();
    float v = axp_batt_mv();
    if (v < 2500 || v > 4500) return;               /* lecture aberrante */
    float ocv;
    if (s_charging) {
        ocv = v - axp_batt_charge_ma() * R_INT_OHM;
    } else {
        ocv = v + axp_batt_discharge_ma() * R_INT_OHM;
    }
    s_ocv_filt = (s_ocv_filt < 0) ? ocv : (0.75f * s_ocv_filt + 0.25f * ocv);
    int p = ocv_to_pct(s_ocv_filt);
    if (s_percent < 0 || s_vbus) {
        s_percent = p;
    } else if (p < s_percent) {
        s_percent = p;          /* sur batterie, jamais de remontée */
    }
}

int  power_percent(void)  { return s_percent < 0 ? 0 : s_percent; }
bool power_vbus(void)     { return s_vbus; }
bool power_charging(void) { return s_charging; }

void power_get_telemetry(power_telemetry_t *t)
{
    memset(t, 0, sizeof(*t));
    t->vbat_mv = axp_batt_mv();
    t->i_dischg_ma = axp_batt_discharge_ma();
    t->i_chg_ma = axp_batt_charge_ma();
    t->vbus = axp_vbus_present();
    t->charging = axp_is_charging();
    t->percent = power_percent();
    t->used_mah = axp_coulomb_used_mah();
    t->since_s = (uint32_t)((esp_timer_get_time() - s_period_start_us) / 1000000);
    if (t->since_s >= 120 && !t->vbus) {
        t->avg_ma = t->used_mah / (t->since_s / 3600.0f);
    }
    /* segment en cours inclus */
    int64_t now = esp_timer_get_time();
    float on_mah = s_on_mah, off_mah = s_off_mah;
    int64_t on_us = s_on_us, off_us = s_off_us;
    if (s_seg_on) { on_mah += t->used_mah - s_seg_mah; on_us += now - s_seg_us; }
    else          { off_mah += t->used_mah - s_seg_mah; off_us += now - s_seg_us; }
    if (!t->vbus && off_us >= 120LL * 1000000) t->off_avg_ma = off_mah / (off_us / 3.6e9f);
    if (!t->vbus && on_us >= 10LL * 1000000)   t->on_avg_ma  = on_mah / (on_us / 3.6e9f);
    if (on_us + off_us > 0) t->screen_pct = (float)(100.0 * on_us / (double)(on_us + off_us));
}
