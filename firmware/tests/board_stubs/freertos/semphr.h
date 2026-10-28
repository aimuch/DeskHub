#pragma once
#include <cstdint>
struct TestSemaphore;
using SemaphoreHandle_t = TestSemaphore *;
constexpr uint32_t portMAX_DELAY=UINT32_MAX;
SemaphoreHandle_t xSemaphoreCreateMutex();
int xSemaphoreTake(SemaphoreHandle_t,uint32_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
