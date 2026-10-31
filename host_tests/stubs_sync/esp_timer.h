#pragma once
/* esp_timer simulé pour test_sync.c (horloge et échéances pilotées par le test) */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
extern int64_t g_fake_time_us;
static inline int64_t esp_timer_get_time(void) { return g_fake_time_us; }
typedef struct esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct { esp_timer_cb_t callback; void *arg; int dispatch_method; const char *name; bool skip_unhandled_events; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h);
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t h);
