#pragma once
typedef void (*shutdown_handler_t)(void);
int esp_register_shutdown_handler(shutdown_handler_t h);
