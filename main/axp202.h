/*
 * axp202.h — pilote minimal du PMIC AXP202 (I2C brut, sans bibliothèque).
 * Registres et bits vérifiés contre lewisxhe/AXP202X_Library.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Bits du registre 0x12 (contrôle des sorties) */
#define AXP_OUT_EXTEN   (1u << 0)
#define AXP_OUT_DCDC3   (1u << 1)   /* 3V3 ESP32 : ne JAMAIS couper */
#define AXP_OUT_LDO2    (1u << 2)   /* V1 : alimente ST7789 + rétroéclairage */
#define AXP_OUT_LDO4    (1u << 3)
#define AXP_OUT_DCDC2   (1u << 4)
#define AXP_OUT_LDO3    (1u << 6)   /* V1 : ampli audio MAX98357 */

/* Sources d'IRQ décodées (index de bit sur 40 bits : reg 0x48..0x4C) */
#define AXP_IRQ_VBUS_REMOVED    (1ull << 2)
#define AXP_IRQ_VBUS_CONNECT    (1ull << 3)
#define AXP_IRQ_CHG_DONE        (1ull << 10)
#define AXP_IRQ_CHG_START       (1ull << 11)
#define AXP_IRQ_PEK_LONG        (1ull << 16)
#define AXP_IRQ_PEK_SHORT       (1ull << 17)
#define AXP_IRQ_LOWBAT_L2       (1ull << 24)
#define AXP_IRQ_LOWBAT_L1       (1ull << 25)

esp_err_t axp_init(void);
esp_err_t axp_set_outputs(uint8_t mask, bool on);

esp_err_t axp_irq_enable(uint64_t mask);       /* active exactement ce masque */
uint64_t  axp_irq_read_and_clear(void);

bool      axp_vbus_present(void);
bool      axp_is_charging(void);
float     axp_batt_mv(void);
float     axp_batt_discharge_ma(void);
float     axp_batt_charge_ma(void);

/* Compteur coulométrique (mAh net consommés depuis le dernier reset, >0 = décharge) */
esp_err_t axp_coulomb_reset(void);
float     axp_coulomb_used_mah(void);
