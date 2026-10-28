#pragma once
#include <cstdint>
using BaseType_t = int;
using TickType_t = uint32_t;
constexpr int pdTRUE = 1, pdFALSE = 0;
#define pdMS_TO_TICKS(value) (value)
