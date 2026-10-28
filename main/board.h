/*
 * board.h — LilyGo T-Watch 2020 V1 : brochage et constantes matérielles
 *
 * Sources : TTGO_TWatch_Library (board/twatch2020_v1.h, TTGO.h).
 */
#pragma once

#include "driver/gpio.h"

/* ---------- Écran ST7789 240x240 (SPI) ---------- */
#define BOARD_LCD_HOST          SPI2_HOST
#define BOARD_LCD_MOSI          GPIO_NUM_19
#define BOARD_LCD_SCLK          GPIO_NUM_18
#define BOARD_LCD_CS            GPIO_NUM_5
#define BOARD_LCD_DC            GPIO_NUM_27
#define BOARD_LCD_RST           GPIO_NUM_NC
#define BOARD_LCD_BL            GPIO_NUM_12
#define BOARD_LCD_W             240
#define BOARD_LCD_H             240
/* MOSI=19 passe par la matrice GPIO (pas l'IOMUX VSPI) : 40 MHz en écriture
 * seule fonctionne (valeur TFT_eSPI d'origine). Descendre à 26 MHz si artefacts. */
#define BOARD_LCD_PCLK_HZ       (40 * 1000 * 1000)

/* Orientation. La lib LilyGo utilise setRotation(2) sur la 2020 (180°) :
 * miroir X+Y et décalage de 80 lignes dans la GRAM 240x320 du ST7789.
 * Certaines 2020 ont une dalle « PANEL_V1 » montée à l'endroit : mettre 1. */
#define BOARD_PANEL_ALT         0
#if BOARD_PANEL_ALT
#define BOARD_LCD_MIRROR_X      false
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         0
#define BOARD_TOUCH_INVERT      1   /* x = W-x, y = H-y */
#else
#define BOARD_LCD_MIRROR_X      true
#define BOARD_LCD_MIRROR_Y      true
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         80
#define BOARD_TOUCH_INVERT      0
#endif

/* ---------- Bus I2C 0 : AXP202, PCF8563, BMA423 ---------- */
#define BOARD_I2C0_SDA          GPIO_NUM_21
#define BOARD_I2C0_SCL          GPIO_NUM_22
#define BOARD_I2C0_HZ           400000

/* ---------- Bus I2C 1 : tactile FT6336 ---------- */
#define BOARD_I2C1_SDA          GPIO_NUM_23
#define BOARD_I2C1_SCL          GPIO_NUM_32
#define BOARD_I2C1_HZ           400000

#define BOARD_AXP202_ADDR       0x35
#define BOARD_PCF8563_ADDR      0x51
#define BOARD_BMA423_ADDR       0x19
#define BOARD_FT6336_ADDR       0x38

/* ---------- Interruptions / divers ---------- */
#define BOARD_AXP_INT           GPIO_NUM_35   /* actif bas, entrée seule */
#define BOARD_TOUCH_INT         GPIO_NUM_38
#define BOARD_RTC_INT           GPIO_NUM_37
#define BOARD_BMA_INT1          GPIO_NUM_39
#define BOARD_MOTOR             GPIO_NUM_4

/* ---------- Gestion d'énergie ---------- */
/* Fréquences DFS. Au repos (écran éteint) le CPU tombe à MIN. Pendant le rendu
 * LVGL on prend un verrou CPU_FREQ_MAX. Si instabilité BLE constatée, passer
 * MIN à 80 pour isoler le problème. */
#define POWER_CPU_MAX_MHZ       240   /* uniquement écran allumé (verrou CPU_FREQ_MAX) */
/* 10 MHz = quartz 40 MHz / 4. Le contrôleur BT relâche son verrou APB_MAX
 * pendant le modem sleep (bt.c, btdm_sleep_enter_phase2) : écran éteint et
 * radio endormie, le CPU tombe à 10 MHz au lieu de 40. Mini accepté par
 * esp_pm sur ESP32 : 10 MHz (REF_TICK). Revenir à 40 si le BLE devient instable. */
#define POWER_CPU_MIN_MHZ       10

/* Batterie d'origine T-Watch 2020 */
#define BATTERY_CAPACITY_MAH    380
