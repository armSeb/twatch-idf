#include "i2c_bus.h"
#include <string.h>
#include "board.h"
#include "esp_log.h"

static const char *TAG = "i2c";
static i2c_master_bus_handle_t s_bus[2];

#define I2C_TIMEOUT_MS 50

static esp_err_t make_bus(int port, gpio_num_t sda, gpio_num_t scl, i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, out);
}

esp_err_t i2c_bus_init(void)
{
    ESP_ERROR_CHECK(make_bus(0, BOARD_I2C0_SDA, BOARD_I2C0_SCL, &s_bus[0]));
    ESP_ERROR_CHECK(make_bus(1, BOARD_I2C1_SDA, BOARD_I2C1_SCL, &s_bus[1]));
    return ESP_OK;
}

esp_err_t i2c_bus_add(i2c_bus_id_t bus, uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = (bus == I2C_BUS_SENSORS) ? BOARD_I2C0_HZ : BOARD_I2C1_HZ,
    };
    esp_err_t err = i2c_master_probe(s_bus[bus], addr, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bus %d : pas de réponse à 0x%02x (%s)", bus, addr, esp_err_to_name(err));
    }
    esp_err_t e2 = i2c_master_bus_add_device(s_bus[bus], &dc, out);
    return err != ESP_OK ? err : e2;
}

esp_err_t i2c_reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t i2c_reg_write(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[17];
    if (len > sizeof(tmp) - 1) return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(dev, tmp, len + 1, I2C_TIMEOUT_MS);
}

esp_err_t i2c_reg_write8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    return i2c_reg_write(dev, reg, &val, 1);
}

esp_err_t i2c_reg_read8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_reg_read(dev, reg, val, 1);
}

esp_err_t i2c_reg_update8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t v;
    esp_err_t err = i2c_reg_read8(dev, reg, &v);
    if (err != ESP_OK) return err;
    v = (v & ~mask) | (val & mask);
    return i2c_reg_write8(dev, reg, v);
}
