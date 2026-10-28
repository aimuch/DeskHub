#pragma once
#include "esp_err.h"
#include <cstdint>
esp_err_t esp_lv_adapter_lock(int32_t timeout_ms);
void esp_lv_adapter_unlock();
