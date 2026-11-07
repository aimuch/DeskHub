// LVGL's objects in PSRAM (docs/desktop-app-design.md 12.6b, H2; in every build since 0.3.18): the internal memory is
// left to the draw buffers, Wi-Fi and Bluetooth. Compiled when LVGL uses its "custom" allocator (sdkconfig.defaults).
#include "sdkconfig.h"

#if CONFIG_LV_USE_CUSTOM_MALLOC
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/stdlib/lv_mem.h"

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

// PSRAM first; internal memory only when PSRAM is full, so the screen still draws.
void *lv_malloc_core(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}
void *lv_realloc_core(void *p, size_t new_size) {
    void *q = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}
void lv_free_core(void *p) { heap_caps_free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t *mon) { LV_UNUSED(mon); }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif
