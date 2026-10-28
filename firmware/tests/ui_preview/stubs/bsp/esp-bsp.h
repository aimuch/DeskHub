#pragma once
#include "lvgl.h"
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock();
lv_indev_t *bsp_display_get_input_dev();
