/*
 * lv_mem_psram.c — allocateur de LVGL (CONFIG_LV_USE_CUSTOM_MALLOC) : objets,
 * styles, textes et calques en PSRAM. La RAM interne (≈ 100 Ko libres au
 * mieux à côté du Bluetooth) reste aux piles, aux tampons DMA de l'écran, au
 * Bluetooth, au Wi-Fi et au serveur web de la mise à jour. Repli en RAM
 * interne si la PSRAM manque.
 */
#include "lvgl.h"
#include "src/stdlib/lv_mem.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#define PS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, PS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    if (!p) return lv_malloc_core(new_size);
    /* reste dans la même mémoire que le bloc d'origine */
    uint32_t caps = esp_ptr_external_ram(p) ? PS : MALLOC_CAP_8BIT;
    void *q = heap_caps_realloc(p, new_size, caps);
    if (!q && caps == PS) q = heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
    return q;
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    lv_memzero(mon, sizeof *mon);
    mon->total_size = heap_caps_get_total_size(PS);
    mon->free_size = heap_caps_get_free_size(PS);
    mon->free_biggest_size = heap_caps_get_largest_free_block(PS);
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
