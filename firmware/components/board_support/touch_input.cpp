#include "touch_input.hpp"
#include "touch_sample_queue.hpp"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <algorithm>

namespace dh {
namespace {
constexpr uint32_t scan_ms=10,poll_ms=20,retry_ms=500,max_read_ms=100;
struct Context {
    portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
    touch_detail::SampleQueue queue;
    lv_indev_t *input=nullptr;
    TaskHandle_t task=nullptr;
    esp_timer_handle_t timer=nullptr;
    std::atomic<esp_lcd_touch_handle_t> handle{nullptr};
    std::atomic<bool> enabled{false},irq_pending{false};
    uint32_t errors=0,no_packet=0,slow=0,irqs=0,max_read=0,max_callback=0;
    bool reading=false;
};
Context context;
uint32_t now_ms(){return static_cast<uint32_t>(esp_timer_get_time()/1000);}

esp_err_t cached_read(esp_lcd_touch_handle_t handle,esp_lcd_touch_point_data_t *points,
                      uint8_t *count,uint8_t max_count,void *opaque){
    auto &ctx=*static_cast<Context *>(opaque);
    const auto start=esp_timer_get_time();
    ctx.handle.store(handle,std::memory_order_relaxed);
    if(!count)return ESP_ERR_INVALID_ARG;
    *count=0;
    if(!ctx.enabled.load(std::memory_order_acquire))return ESP_OK;
    portENTER_CRITICAL(&ctx.mux);
    const auto output=ctx.queue.consume(now_ms());
    portEXIT_CRITICAL(&ctx.mux);
    // An expired/error gesture is cancelled, not converted into a click on the
    // stale last button. This runs in the normal LVGL input callback context.
    if(output.cancel)lv_indev_wait_release(ctx.input);
    if(output.pressed&&max_count&&points){
        points[0].track_id=output.point.track_id;points[0].x=output.point.x;
        points[0].y=output.point.y;points[0].strength=output.point.strength;*count=1;
    }
    const auto elapsed=static_cast<uint32_t>(esp_timer_get_time()-start);
    portENTER_CRITICAL(&ctx.mux);ctx.max_callback=std::max(ctx.max_callback,elapsed);portEXIT_CRITICAL(&ctx.mux);
    return ESP_OK;
}
void IRAM_ATTR interrupted(esp_lcd_touch_handle_t handle,void *opaque){
    auto &ctx=*static_cast<Context *>(opaque);
    ctx.handle.store(handle,std::memory_order_relaxed);
    if(!ctx.enabled.load(std::memory_order_acquire))return;
    ctx.irq_pending.store(true,std::memory_order_relaxed);
    portENTER_CRITICAL_ISR(&ctx.mux);++ctx.irqs;portEXIT_CRITICAL_ISR(&ctx.mux);
    BaseType_t higher=pdFALSE;vTaskNotifyGiveFromISR(ctx.task,&higher);
    if(higher)portYIELD_FROM_ISR();
}
void scan(void *opaque){
    auto &ctx=*static_cast<Context *>(opaque);
    if(!ctx.enabled.load(std::memory_order_acquire))return;
    // The notification is independent of the I2C task. Even if its driver is
    // stuck, LVGL can expire the cached press and continue rendering normally.
    esp_lv_adapter_touch_notify_interrupt(ctx.input);
    xTaskNotifyGive(ctx.task);
}
void sample(void *opaque){
    auto &ctx=*static_cast<Context *>(opaque);
    uint32_t previous=0,retry_at=0;bool attempted=false,retrying=false;
    for(;;){
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1000));
        if(!ctx.enabled.load(std::memory_order_acquire))continue;
        const auto handle=ctx.handle.load(std::memory_order_relaxed);if(!handle)continue;
        const auto start=now_ms();
        if(retrying&&static_cast<int32_t>(start-retry_at)<0)continue;
        const bool irq=ctx.irq_pending.exchange(false,std::memory_order_relaxed);
        if(attempted&&!irq&&static_cast<uint32_t>(start-previous)<poll_ms)continue;
        previous=start;attempted=true;retrying=false;
        portENTER_CRITICAL(&ctx.mux);ctx.reading=true;portEXIT_CRITICAL(&ctx.mux);
        auto error=esp_lcd_touch_read_data(handle);
        esp_lcd_touch_point_data_t point{};uint8_t count=0;
        if(error==ESP_OK)error=esp_lcd_touch_get_data(handle,&point,&count,1);
        const auto duration=static_cast<uint32_t>(now_ms()-start);
        portENTER_CRITICAL(&ctx.mux);
        ctx.reading=false;ctx.max_read=std::max(ctx.max_read,duration);
        if(error==ESP_ERR_INVALID_RESPONSE){
            // The CST driver leaves its previous point in memory on invalid
            // ACK. Treat it as no new packet: never read/freshen that point.
            ++ctx.no_packet;
        }else if(error!=ESP_OK||duration>max_read_ms){
            ++ctx.errors;if(duration>max_read_ms)++ctx.slow;
            ctx.queue.failure();retry_at=now_ms()+retry_ms;retrying=true;
        }else{
            ctx.queue.publish(count>0,{point.x,point.y,point.strength,point.track_id},start);
        }
        portEXIT_CRITICAL(&ctx.mux);
        esp_lv_adapter_touch_notify_interrupt(ctx.input);
    }
}
}

esp_err_t touch_input_start(){
    auto &ctx=context;
    if(ctx.enabled.load(std::memory_order_acquire))return ESP_OK;
    const auto lock=esp_lv_adapter_lock(1000);if(lock!=ESP_OK)return lock;
    struct Guard{~Guard(){esp_lv_adapter_unlock();}} guard;
    auto *input=bsp_display_get_input_dev();if(!input)return ESP_ERR_INVALID_STATE;
    auto *read_timer=lv_indev_get_read_timer(input);if(!read_timer)return ESP_ERR_INVALID_STATE;
    ctx.input=input;ctx.handle.store(nullptr,std::memory_order_relaxed);
    if(xTaskCreate(sample,"dh_touch",4096,&ctx,3,&ctx.task)!=pdPASS){ctx.input=nullptr;return ESP_ERR_NO_MEM;}
    esp_timer_create_args_t args{};args.callback=scan;args.arg=&ctx;args.name="dh_touch_scan";
    auto error=esp_timer_create(&args,&ctx.timer);
    if(error!=ESP_OK){vTaskDelete(ctx.task);ctx.task=nullptr;ctx.input=nullptr;return error;}
    esp_lv_adapter_touch_callbacks_t callbacks{};callbacks.custom_touch_read=cached_read;
    callbacks.on_interrupt=interrupted;callbacks.user_ctx=&ctx;
    error=esp_lv_adapter_set_touch_callbacks(input,&callbacks);
    if(error==ESP_OK){
        error=esp_timer_start_periodic(ctx.timer,scan_ms*1000);
    }
    if(error!=ESP_OK){
        // No worker may sample until enabled=true, so rollback never cancels
        // a task while it owns an I2C transaction.
        esp_lv_adapter_set_touch_callbacks(input,nullptr);
        esp_timer_delete(ctx.timer);vTaskDelete(ctx.task);
        ctx.timer=nullptr;ctx.task=nullptr;ctx.input=nullptr;return error;
    }
    // LVGL 9.4 has no public period getter. Change it only after all fallible
    // setup succeeds, so rollback leaves the BSP's original timer untouched.
    lv_timer_set_period(read_timer,scan_ms);
    lv_indev_wait_release(input);
    ctx.enabled.store(true,std::memory_order_release);
    esp_lv_adapter_touch_notify_interrupt(input);
    return ESP_OK;
}
TouchDiagnostics touch_input_diagnostics(){
    auto &ctx=context;TouchDiagnostics result;const auto now=now_ms();
    result.ready=ctx.enabled.load(std::memory_order_acquire);
    portENTER_CRITICAL(&ctx.mux);
    result.read_in_progress=ctx.reading;result.sample_valid=ctx.queue.has_valid();result.stale=!ctx.queue.fresh(now);
    result.recovering=ctx.queue.recovering();result.samples=ctx.queue.samples;
    result.read_errors=ctx.errors;result.no_packet_reads=ctx.no_packet;result.slow_reads=ctx.slow;
    result.irq_events=ctx.irqs;result.queue_overflows=ctx.queue.overflows;
    result.stale_cancellations=ctx.queue.stale_cancellations;result.max_read_ms=ctx.max_read;
    result.max_callback_us=ctx.max_callback;result.sample_age_ms=ctx.queue.age(now);
    portEXIT_CRITICAL(&ctx.mux);return result;
}
}
