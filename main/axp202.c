#include "axp202.h"
#include "i2c_bus.h"
#include "board.h"
#include "esp_log.h"

static const char *TAG = "axp";
static i2c_master_dev_handle_t s_dev;

#define REG_PWR_STATUS      0x00
#define REG_CHG_STATUS      0x01
#define REG_OUT_CTL         0x12
#define REG_LDO24_VOL       0x28
#define REG_VOFF            0x31   /* bits 2:0 : tension de coupure 2,6 V + n × 0,1 V */
#define REG_OFF_CTL         0x32
#define REG_CHARGE1         0x33
#define REG_PEK_SET         0x36
#define REG_INTEN1          0x40
#define REG_INTSTS1         0x48
#define REG_BAT_V_H8        0x78
#define REG_BAT_ICHG_H8     0x7A
#define REG_BAT_IDCHG_H8    0x7C
#define REG_ADC_EN1         0x82
#define REG_ADC_SPEED       0x84
#define REG_COULOMB_CHG     0xB0
#define REG_COULOMB_DCHG    0xB4
#define REG_COULOMB_CTL     0xB8

/* fréquence ADC : 25 Hz (bits 7:6 de 0x84 = 00) — sert au calcul coulomb */
#define ADC_RATE_HZ 25.0f

esp_err_t axp_init(void)
{
    esp_err_t err = i2c_bus_add(I2C_BUS_SENSORS, BOARD_AXP202_ADDR, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP202 absent ?");
        return err;
    }

    /* Coupure matérielle sur appui long : 4 s (bits 1:0 de 0x36 = 00) */
    i2c_reg_update8(s_dev, REG_PEK_SET, 0x03, 0x00);
    /* Coupure batterie à 3,2 V au lieu de 2,9 V (défaut) : il ne reste que
     * ~1 % d'énergie sous 3,2 V, et une LiPo stockée vide après être
     * descendue à 2,9 V perd vite de la capacité. */
    i2c_reg_update8(s_dev, REG_VOFF, 0x07, 0x06);
    /* LED de charge : contrôlée par registre (bit3=1), éteinte (bits5:4=00) */
    i2c_reg_update8(s_dev, REG_OFF_CTL, 0x38, 0x08);
    /* Courant de charge 300 mA (0.8C pour 380 mAh) : nibble bas = (I-300)/100 */
    i2c_reg_update8(s_dev, REG_CHARGE1, 0x0F, 0x00);
    /* LDO2 = 3,3 V (bits 7:4 de 0x28 : 1,8 V + 100 mV*n → n=15) */
    i2c_reg_update8(s_dev, REG_LDO24_VOL, 0xF0, 0xF0);

    /* Rails : NE RIEN COUPER. Sur la montre de Seb (ancien firmware
     * fonctionnel), LDO3 alimente l'écran et LDO4 le tactile, contrairement
     * au mapping LilyGo « V1 ». Une puce non alimentée mais reliée à l'ESP32
     * se fait alimenter par ses E/S et perturbe le 3V3 → radio BLE sourde
     * (RSSI −82 dBm à 30 cm, supervision timeout). L'AXP conserve ses
     * registres tant que la batterie est branchée : on rallume explicitement. */
    axp_set_outputs(AXP_OUT_DCDC3 | AXP_OUT_LDO2 | AXP_OUT_LDO3 | AXP_OUT_LDO4, true);

    /* ADC : uniquement tension + courant batterie (0x82 bits 7,6) ; 25 Hz */
    i2c_reg_write8(s_dev, REG_ADC_EN1, 0xC0);
    i2c_reg_update8(s_dev, REG_ADC_SPEED, 0xC0, 0x00);

    /* Compteur coulométrique ON */
    i2c_reg_write8(s_dev, REG_COULOMB_CTL, 0x80);

    uint8_t out;
    i2c_reg_read8(s_dev, REG_OUT_CTL, &out);
    ESP_LOGI(TAG, "init OK, sorties=0x%02x Vbat=%.0f mV vbus=%d", out, axp_batt_mv(), axp_vbus_present());
    return ESP_OK;
}

esp_err_t axp_set_outputs(uint8_t mask, bool on)
{
    /* garde-fou : on ne coupe jamais DCDC3 */
    if (!on) mask &= ~AXP_OUT_DCDC3;
    return i2c_reg_update8(s_dev, REG_OUT_CTL, mask, on ? mask : 0);
}

esp_err_t axp_irq_enable(uint64_t mask)
{
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 5; i++) {
        err |= i2c_reg_write8(s_dev, REG_INTEN1 + i, (uint8_t)(mask >> (8 * i)));
    }
    /* purge des statuts en attente */
    for (int i = 0; i < 5; i++) {
        err |= i2c_reg_write8(s_dev, REG_INTSTS1 + i, 0xFF);
    }
    return err;
}

uint64_t axp_irq_read_and_clear(void)
{
    uint8_t st[5] = {0};
    if (i2c_reg_read(s_dev, REG_INTSTS1, st, 5) != ESP_OK) {
        return 0;
    }
    uint64_t m = 0;
    for (int i = 0; i < 5; i++) {
        m |= (uint64_t)st[i] << (8 * i);
        if (st[i]) i2c_reg_write8(s_dev, REG_INTSTS1 + i, st[i]);   /* W1C */
    }
    return m;
}

bool axp_vbus_present(void)
{
    uint8_t v = 0;
    i2c_reg_read8(s_dev, REG_PWR_STATUS, &v);
    return (v & (1 << 5)) != 0;
}

bool axp_is_charging(void)
{
    uint8_t v = 0;
    i2c_reg_read8(s_dev, REG_CHG_STATUS, &v);
    return (v & (1 << 6)) != 0;
}

static uint16_t read_12(uint8_t reg)      /* H8 + L4 */
{
    uint8_t b[2] = {0};
    i2c_reg_read(s_dev, reg, b, 2);
    return ((uint16_t)b[0] << 4) | (b[1] & 0x0F);
}

static uint16_t read_13(uint8_t reg)      /* H8 + L5 */
{
    uint8_t b[2] = {0};
    i2c_reg_read(s_dev, reg, b, 2);
    return ((uint16_t)b[0] << 5) | (b[1] & 0x1F);
}

float axp_batt_mv(void)          { return read_12(REG_BAT_V_H8) * 1.1f; }

uint8_t axp_reg(uint8_t reg)
{
    uint8_t v = 0;
    i2c_reg_read8(s_dev, reg, &v);
    return v;
}
float axp_batt_charge_ma(void)   { return read_12(REG_BAT_ICHG_H8) * 0.5f; }
float axp_batt_discharge_ma(void){ return read_13(REG_BAT_IDCHG_H8) * 0.5f; }

esp_err_t axp_coulomb_reset(void)
{
    /* bit7 enable, bit5 clear (auto-effacé) */
    return i2c_reg_write8(s_dev, REG_COULOMB_CTL, 0xA0);
}

static uint32_t read_be32(uint8_t reg)
{
    uint8_t b[4] = {0};
    i2c_reg_read(s_dev, reg, b, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

float axp_coulomb_mah_per_count(void)
{
    return 65536.0f * 0.5f / 3600.0f / ADC_RATE_HZ;   /* ≈ 0,364 mAh à 25 Hz */
}

esp_err_t axp_coulomb_raw(uint32_t *chg, uint32_t *dchg)
{
    uint8_t b[8];
    /* 0xB0..0xB7 : CHG (4 octets) puis DCHG (4 octets), lus d'un bloc */
    esp_err_t e = i2c_reg_read(s_dev, REG_COULOMB_CHG, b, 8);
    if (e != ESP_OK) return e;
    *chg = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    *dchg = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    return ESP_OK;
}

float axp_coulomb_used_mah(void)
{
    /* C(mAh) = 65536 * 0.5 mA * (DCHG - CHG) / 3600 / fréquence ADC */
    int64_t chg = read_be32(REG_COULOMB_CHG);
    int64_t dchg = read_be32(REG_COULOMB_DCHG);
    return (float)(dchg - chg) * 65536.0f * 0.5f / 3600.0f / ADC_RATE_HZ;
}
