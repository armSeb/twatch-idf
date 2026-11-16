#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_18 18
#define GPIO_NUM_19 19
#define GPIO_NUM_21 21
#define GPIO_NUM_22 22
#define GPIO_NUM_23 23
#define GPIO_NUM_27 27
#define GPIO_NUM_32 32
#define GPIO_NUM_35 35
#define GPIO_NUM_37 37
#define GPIO_NUM_38 38
#define GPIO_NUM_39 39
#define GPIO_NUM_NC -1
enum { GPIO_MODE_INPUT, GPIO_INTR_HIGH_LEVEL = 5 };
typedef struct { unsigned long long pin_bit_mask; int mode; int pull_up_en, pull_down_en; int intr_type; } gpio_config_t;
static inline int gpio_config(const gpio_config_t *c) { return 0; }
static inline int gpio_isr_handler_add(int p, void (*f)(void *), void *a) { return 0; }
static inline int gpio_isr_handler_remove(int p) { return 0; }
static inline int gpio_wakeup_enable(int p, int t) { return 0; }
static inline int gpio_wakeup_disable(int p) { return 0; }
static inline int gpio_intr_enable(int p) { return 0; }
static inline int gpio_intr_disable(int p) { return 0; }
