#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Deux bus I2C (driver « i2c_master » d'ESP-IDF 5.x, thread-safe par bus).
 * Le driver prend lui-même un verrou PM APB pendant les transactions. */
typedef enum { I2C_BUS_SENSORS = 0, I2C_BUS_TOUCH = 1 } i2c_bus_id_t;

esp_err_t i2c_bus_init(void);
esp_err_t i2c_bus_add(i2c_bus_id_t bus, uint8_t addr, i2c_master_dev_handle_t *out);

esp_err_t i2c_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t i2c_reg_write(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *buf, size_t len);
esp_err_t i2c_reg_write8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);
esp_err_t i2c_reg_read8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val);
/* lecture-modification-écriture : reg = (reg & ~mask) | (val & mask) */
esp_err_t i2c_reg_update8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask, uint8_t val);
