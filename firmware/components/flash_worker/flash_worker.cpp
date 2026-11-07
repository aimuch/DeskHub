#include "flash_worker.hpp"
#include "esp_cpu.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace dh {
namespace {
struct Job {
    void (*fn)(void *);
    void *ctx;
    SemaphoreHandle_t done;
};
QueueHandle_t queue = nullptr;

void worker(void *) {
    Job job;
    for (;;) {
        if (xQueueReceive(queue, &job, portMAX_DELAY) != pdTRUE) continue;
        job.fn(job.ctx);
        xSemaphoreGive(job.done);
    }
}
}  // namespace

void flash_worker_start() {
    if (queue) return;
    QueueHandle_t q = xQueueCreate(4, sizeof(Job));
    if (!q) return;
    queue = q;
    // xTaskCreate takes the stack from internal memory: that is the point. 8 KB: the USB requests run here too.
    if (xTaskCreate(worker, "dh_flash", 8192, nullptr, 6, nullptr) != pdPASS) {
        queue = nullptr;
        vQueueDelete(q);
    }
}

bool flash_call(void (*fn)(void *), void *ctx) {
    if (esp_ptr_in_dram(reinterpret_cast<const void *>(esp_cpu_get_sp()))) {   // the same test ESP-IDF makes before a flash write
        fn(ctx);
        return true;
    }
    if (!queue) return false;
    StaticSemaphore_t storage;
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&storage);
    Job job{fn, ctx, done};
    if (xQueueSend(queue, &job, portMAX_DELAY) != pdTRUE) return false;
    xSemaphoreTake(done, portMAX_DELAY);
    return true;
}
}  // namespace dh
