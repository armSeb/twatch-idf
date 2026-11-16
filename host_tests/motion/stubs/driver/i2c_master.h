#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *i2c_master_dev_handle_t;
typedef struct { uint8_t *write_buffer; size_t buffer_size; } i2c_master_transmit_multi_buffer_info_t;
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t *, size_t, uint8_t *, size_t, int);
esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t, i2c_master_transmit_multi_buffer_info_t *, size_t, int);
