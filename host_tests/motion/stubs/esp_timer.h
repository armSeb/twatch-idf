#pragma once
#include <stdint.h>
extern int64_t g_now;
static inline int64_t esp_timer_get_time(void) { return g_now; }
