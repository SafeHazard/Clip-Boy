/**
 * @file lv_mem_core_custom_psram.c
 * Custom LVGL memory allocator that routes to ESP32 PSRAM via heap_caps.
 * Active when LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM.
 */

#include "../lv_conf_internal.h"
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "lv_mem.h"
#include <esp_heap_caps.h>

void lv_mem_init(void)
{
    /* Nothing to init */
}

void lv_mem_deinit(void)
{
    /* Nothing to deinit */
}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void * lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void * lv_realloc_core(void * p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void * p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */
