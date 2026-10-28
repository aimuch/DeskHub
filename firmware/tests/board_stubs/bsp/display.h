#pragma once
#include "esp_err.h"
enum bsp_display_rotation_t { BSP_DISPLAY_ROTATE_0,BSP_DISPLAY_ROTATE_90,BSP_DISPLAY_ROTATE_180,BSP_DISPLAY_ROTATE_270 };
esp_err_t bsp_display_rotation_set(bsp_display_rotation_t);
esp_err_t bsp_display_brightness_set(int);
