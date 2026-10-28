#include "core.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <utility>

namespace dh {
namespace {
SemaphoreHandle_t jobs_lock = nullptr;
TaskHandle_t worker = nullptr;
ConfigDraft pending;
ConfigJobResult result;
uint32_t next_ticket = 0;
bool reset_pending = false;

struct JobGuard {
    JobGuard() { xSemaphoreTake(jobs_lock, portMAX_DELAY); }
    ~JobGuard() { xSemaphoreGive(jobs_lock); }
};

void run(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ConfigDraft draft;
        uint32_t ticket;
        bool reset;
        {
            JobGuard guard;
            if(result.state!=ConfigJobState::Pending)continue;
            draft=pending;ticket=result.ticket;reset=reset_pending;
        }
        // The worker owns a C++ copy; no FreeRTOS queue copies std::string bytes.
        const auto error=reset ? reset_config(draft) : save_config(draft);
        {
            JobGuard guard;
            result.ticket=ticket;
            result.state=error==ESP_OK ? ConfigJobState::Succeeded : ConfigJobState::Failed;
            result.error=error;
            result.revision=error==ESP_OK ? (reset ? 0 : draft.value.revision+1) : 0;
        }
    }
}

esp_err_t enqueue(ConfigDraft draft, bool reset, uint32_t &ticket) {
    ticket=0;
    if(!jobs_lock||!worker)return ESP_ERR_NO_MEM;
    if(!reset&&!valid_config(draft.value))return ESP_ERR_INVALID_ARG;
    {
        JobGuard guard;
        if(result.state==ConfigJobState::Pending)return ESP_ERR_TIMEOUT;
        ++next_ticket;if(!next_ticket)++next_ticket;
        pending=std::move(draft);reset_pending=reset;
        result={next_ticket,ConfigJobState::Pending,ESP_OK,0};
        ticket=next_ticket;
    }
    xTaskNotifyGive(worker);
    return ESP_OK;
}
}

void config_jobs_init() {
    jobs_lock=xSemaphoreCreateMutex();
    if(jobs_lock&&xTaskCreate(run,"dh_config",6144,nullptr,2,&worker)!=pdPASS)worker=nullptr;
}
esp_err_t queue_config_save(ConfigDraft draft,uint32_t &ticket) { return enqueue(std::move(draft),false,ticket); }
esp_err_t queue_config_reset(ConfigDraft draft,uint32_t &ticket) { return enqueue(std::move(draft),true,ticket); }
ConfigJobResult config_job_result(uint32_t ticket) {
    if(!jobs_lock)return {ticket,ConfigJobState::Unknown,ESP_ERR_NO_MEM,0};
    JobGuard guard;
    if(!ticket||ticket!=result.ticket)return {ticket,ConfigJobState::Unknown,ESP_ERR_NOT_FOUND,0};
    return result;
}
}
