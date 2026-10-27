#include "wclock.h"
#include <sys/time.h>
#include "i2c_bus.h"
#include "board.h"
#include "settings.h"
#include "esp_log.h"

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;
static bool s_valid;

static inline uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static inline uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

/* Relit le PCF8563 (qui fait foi) et recale l'horloge système si besoin.
 * En light sleep, l'horloge système ESP32 tourne sur l'oscillateur RC
 * interne (~150 kHz), qui dérive de plusieurs secondes par heure. */
static esp_err_t read_rtc_to_system(bool verbose)
{
    uint8_t r[7];
    esp_err_t err = i2c_reg_read(s_dev, 0x02, r, 7);
    if (err != ESP_OK) return err;
    if (r[0] & 0x80) {
        if (verbose) ESP_LOGW(TAG, "VL=1 : heure RTC non fiable, attente synchro CTS");
        s_valid = false;
        return ESP_OK;
    }
    struct tm tm = {
        .tm_sec  = bcd2bin(r[0] & 0x7F),
        .tm_min  = bcd2bin(r[1] & 0x7F),
        .tm_hour = bcd2bin(r[2] & 0x3F),
        .tm_mday = bcd2bin(r[3] & 0x3F),
        .tm_mon  = bcd2bin(r[5] & 0x1F) - 1,
        .tm_year = bcd2bin(r[6]) + 100,    /* 20xx */
    };
    if (tm.tm_year < 124 || tm.tm_mon < 0 || tm.tm_mon > 11) return ESP_ERR_INVALID_STATE;
    time_t t = mktime(&tm);                /* TZ système = UTC → mktime == timegm */
    time_t now = time(NULL);
    long drift = (long)(now - t);
    if (drift != 0) {
        struct timeval tv = { .tv_sec = t };
        settimeofday(&tv, NULL);
    }
    s_valid = true;
    if (verbose) {
        ESP_LOGI(TAG, "RTC → %04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else if (drift > 1 || drift < -1) {
        ESP_LOGI(TAG, "horloge système recalée de %ld s sur la RTC", -drift);
    }
    return ESP_OK;
}

esp_err_t wclock_init(void)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    esp_err_t err = i2c_bus_add(I2C_BUS_SENSORS, BOARD_PCF8563_ADDR, &s_dev);
    if (err != ESP_OK) return err;

    i2c_reg_write8(s_dev, 0x00, 0x00);   /* CTRL1 : horloge en marche */
    i2c_reg_write8(s_dev, 0x01, 0x00);   /* CTRL2 : pas d'IRQ alarme/timer */
    i2c_reg_write8(s_dev, 0x0D, 0x00);   /* CLKOUT désactivé (actif à 32 kHz par défaut) */
    i2c_reg_write8(s_dev, 0x0E, 0x00);   /* timer off */
    return read_rtc_to_system(true);
}

void wclock_resync(void)
{
    if (s_dev && s_valid) read_rtc_to_system(false);
}

bool wclock_time_valid(void) { return s_valid; }

void wclock_set_utc(time_t utc)
{
    struct timeval tv = { .tv_sec = utc };
    settimeofday(&tv, NULL);
    struct tm tm;
    gmtime_r(&utc, &tm);
    uint8_t r[7] = {
        bin2bcd(tm.tm_sec), bin2bcd(tm.tm_min), bin2bcd(tm.tm_hour),
        bin2bcd(tm.tm_mday), (uint8_t)tm.tm_wday, bin2bcd(tm.tm_mon + 1),
        bin2bcd(tm.tm_year % 100),
    };
    if (i2c_reg_write(s_dev, 0x02, r, 7) != ESP_OK) {
        ESP_LOGW(TAG, "écriture RTC échouée");
    }
    s_valid = true;
}

int32_t wclock_tz_offset_min(void) { return settings_get()->tz_offset_min; }

void wclock_set_tz_offset_min(int32_t off)
{
    if (off < -14 * 60 || off > 14 * 60) return;
    if (settings_get()->tz_offset_min != off) {
        settings_get()->tz_offset_min = (int16_t)off;
        settings_save();
    }
}

void wclock_local_tm(struct tm *out)
{
    time_t t = time(NULL) + (time_t)wclock_tz_offset_min() * 60;
    gmtime_r(&t, out);
}
