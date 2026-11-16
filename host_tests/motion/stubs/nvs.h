#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
enum { NVS_READONLY, NVS_READWRITE };
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
