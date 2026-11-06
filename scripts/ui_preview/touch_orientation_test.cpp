// Host regression using the production board init/rotation and real LVGL 9.4
// pointer processing. Panel/touch transport is simulated; this is not hardware
// evidence. Physical pixel locations follow the CO5300 MADCTL values used by
// the Waveshare BSP, independently of LVGL's input rotation implementation.
#include "lvgl.h"
#include "board.hpp"
#include "core.hpp"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr int extent=480,last=extent-1;
lv_display_t *test_display=nullptr;
lv_indev_t *test_input=nullptr;
bsp_display_cfg_t startup{},active{};
unsigned legacy_starts=0;
uint8_t panel_madctl=0;
esp_err_t rotation_result=ESP_OK;
unsigned rotation_calls=0,brightness_calls=0;
bool pressed=false;
lv_point_t raw{};
std::array<uint8_t,extent*20*2> pixels{};
void check(bool value,const char *message){
    if(!value){std::fprintf(stderr,"touch orientation: %s\n",message);std::abort();}
}
bool equal(lv_point_t a,lv_point_t b){return a.x==b.x&&a.y==b.y;}
void touch_read(lv_indev_t *,lv_indev_data_t *data){
    // esp_lcd_touch_get_data mirrors against x/y_max before swap_xy. Reproduce
    // its exact 480-x behavior so the old BSP flags cannot pass by accident.
    auto point=raw;
    if(active.touch_flags.mirror_x)point.x=extent-point.x;
    if(active.touch_flags.mirror_y)point.y=extent-point.y;
    if(active.touch_flags.swap_xy)std::swap(point.x,point.y);
    data->point=point;data->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
lv_point_t physical_pixel(lv_point_t logical){
    // The panel scans swapped columns/rows for MV, then reversed physical
    // axes for MX/MY. These are panel output coordinates, not touch formulas.
    auto point=logical;
    if(panel_madctl&0x20)std::swap(point.x,point.y);
    if(panel_madctl&0x40)point.x=last-point.x;
    if(panel_madctl&0x80)point.y=last-point.y;
    return point;
}
lv_point_t sample(bool down,lv_point_t logical){
    raw=physical_pixel(logical);pressed=down;
    lv_tick_inc(40);lv_indev_read(test_input);
    lv_point_t observed{};lv_indev_get_point(test_input,&observed);return observed;
}
void flush(lv_display_t *display,const lv_area_t *,uint8_t *){lv_display_flush_ready(display);}
struct Target {lv_point_t point;unsigned clicks=0;};
std::array<Target,7> targets{{{{0,0}},{{479,0}},{{0,479}},{{479,479}},{{83,197}},{{346,61}},{{211,367}}}};
void clicked(lv_event_t *event){++static_cast<Target *>(lv_event_get_user_data(event))->clicks;}
unsigned verify_angle(int angle,bool expect_correct){
    check(dh::board_rotation(angle)==ESP_OK,"production board_rotation failed");
    check(lv_display_get_rotation(test_display)==static_cast<lv_display_rotation_t>(((360-angle)%360)/90),
          "LVGL rotation no longer matches the hardware rotation convention");
    sample(false,{240,240}); // clear production wait_release before a new press
    unsigned mismatches=0;
    for(auto &target:targets){
        const auto before=target.clicks;
        const auto observed=sample(true,target.point);
        sample(false,target.point);
        const bool exact=equal(observed,target.point)&&target.clicks==before+1;
        if(!exact)++mismatches;
        if(expect_correct)check(exact,"corner/interior press missed the rendered button");
    }
    // Both axes, both directions, with unequal coordinates: a swapped axis,
    // reversed swipe or coincidental diagonal alignment must be detected.
    const std::array<lv_point_t,16> swipe{{{45,137},{159,137},{281,137},{427,137},
                                        {427,137},{281,137},{159,137},{45,137},
                                        {301,433},{301,289},{301,173},{301,41},
                                        {301,41},{301,173},{301,289},{301,433}}};
    for(const auto point:swipe){
        const auto observed=sample(true,point);
        if(!equal(observed,point))++mismatches;
        if(expect_correct)check(equal(observed,point),"swipe coordinate or direction is reversed");
    }
    sample(false,swipe.back());
    if(expect_correct)check(mismatches==0,"correct map has mismatches");
    else check(mismatches>0,"old BSP swap/mirror flags escaped the negative control");
    return mismatches;
}
}

// Only display/input operations below are exercised. Other board dependencies
// are inert link stubs; no service, transport or physical hardware is accessed.
struct TestSemaphore {};
SemaphoreHandle_t xSemaphoreCreateMutex(){return nullptr;}
int xSemaphoreTake(SemaphoreHandle_t,uint32_t){return 0;}
int xSemaphoreGive(SemaphoreHandle_t){return 0;}
void vSemaphoreDelete(SemaphoreHandle_t){}
esp_err_t bsp_i2c_init(){return ESP_FAIL;}
i2c_master_bus_handle_t bsp_i2c_get_handle(){return nullptr;}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t,const i2c_device_config_t *,i2c_master_dev_handle_t *){return ESP_FAIL;}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t,const uint8_t *,size_t,uint8_t *,size_t,int){return ESP_FAIL;}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t,const uint8_t *,size_t,int){return ESP_FAIL;}
esp_err_t esp_flash_get_size(void *,uint32_t *){return ESP_FAIL;}
size_t esp_psram_get_size(){return 0;}
lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg){
    startup=active=*cfg;
    // Actual BSP startup does not reconcile these two orientations: its
    // panel init table finishes at 0xa0, while the adapter profile is zero.
    panel_madctl=0xa0;lv_display_set_rotation(test_display,LV_DISPLAY_ROTATION_0);
    return test_display;
}
lv_display_t *bsp_display_start(){
    ++legacy_starts;
    bsp_display_cfg_t vendor{};vendor.touch_flags={1,0,1};
    return bsp_display_start_with_config(&vendor);
}
lv_indev_t *bsp_display_get_input_dev(){return test_input;}
// board_display_init sends its first panel commands under the display lock; this test has one thread, so the lock always succeeds.
esp_err_t esp_lv_adapter_lock(int32_t){return ESP_OK;}
void esp_lv_adapter_unlock(){}
esp_err_t bsp_display_brightness_set(int){++brightness_calls;return ESP_OK;}
esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation){
    ++rotation_calls;if(rotation_result!=ESP_OK)return rotation_result;
    constexpr std::array<uint8_t,4> madctl{{0x00,0x60,0xc0,0xa0}};
    panel_madctl=madctl.at(static_cast<size_t>(rotation));return ESP_OK;
}
namespace dh {
int64_t monotonic_ms(){return lv_tick_get();}
void mutate(void (*)(Snapshot &,void *),void *){}
bool parse_utc(const char *,time_t &){return false;}
bool set_time_utc(const char *,const char *){return false;}
}

int main(){
    lv_init();test_display=lv_display_create(extent,extent);
    lv_display_set_color_format(test_display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(test_display,pixels.data(),nullptr,pixels.size(),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(test_display,flush);
    test_input=lv_indev_create();lv_indev_set_type(test_input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(test_input,test_display);lv_indev_set_read_cb(test_input,touch_read);
    rotation_result=ESP_ERR_TIMEOUT;
    check(dh::board_display_init()==ESP_ERR_TIMEOUT,"startup panel rotation failure was hidden");
    check(panel_madctl==0xa0&&brightness_calls==0,"failed startup exposed an unaligned display");
    check(dh::board_brightness(50)==ESP_ERR_INVALID_STATE&&dh::board_rotation(0)==ESP_ERR_INVALID_STATE,
          "failed startup marked the display ready");
    rotation_result=ESP_OK;
    check(dh::board_display_init()==ESP_OK,"production board_display_init failed");
    check(panel_madctl==0&&lv_display_get_rotation(test_display)==LV_DISPLAY_ROTATION_0&&
          rotation_calls==2&&brightness_calls==1,
          "cold boot left the vendor MADCTL orientation active before the first UI tick");
    check(legacy_starts==0,"production init used the BSP convenience rotation");
    check(!startup.touch_flags.swap_xy&&!startup.touch_flags.mirror_x&&!startup.touch_flags.mirror_y,
          "native CST9220 coordinates were changed during startup");
    check(startup.rotation==ESP_LV_ADAPTER_ROTATE_0&&startup.tear_avoid_mode==ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
          "adapter performs another pixel rotation");
    const auto &adapter=startup.lv_adapter_cfg;
    // The LVGL stack is the one intended change: 12 KB in PSRAM since 0.3.16 (8 KB overflowed with internal draw buffers).
    check(adapter.task_stack_size==12*1024&&adapter.task_priority==6&&adapter.task_core_id==-1&&
          adapter.tick_period_ms==1&&adapter.task_min_delay_ms==1&&adapter.task_max_delay_ms==15&&
          adapter.stack_in_psram&&!adapter.auto_sleep.enable&&
          adapter.auto_sleep.mode==ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED&&adapter.auto_sleep.idle_timeout_ms==5000&&
          !adapter.auto_sleep.callbacks.on_enter_sleep&&!adapter.auto_sleep.callbacks.on_exit_sleep&&!adapter.auto_sleep.callbacks.user_ctx,
          "explicit startup changed unrelated adapter defaults");
    auto *screen=lv_screen_active();lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    for(auto &target:targets){
        auto *button=lv_button_create(screen);lv_obj_remove_style_all(button);
        lv_obj_set_pos(button,std::clamp(static_cast<int>(target.point.x)-8,0,extent-17),
                       std::clamp(static_cast<int>(target.point.y)-8,0,extent-17));
        lv_obj_set_size(button,17,17);lv_obj_add_event_cb(button,clicked,LV_EVENT_CLICKED,&target);
    }
    lv_obj_update_layout(screen);
    // A zero-angle UI skips board_rotation(0) on its first tick. Prove startup
    // alone aligns a rendered button before any runtime rotation is requested.
    auto &cold_boot_target=targets[4];
    sample(false,cold_boot_target.point);
    const auto cold_boot_point=sample(true,cold_boot_target.point);
    sample(false,cold_boot_target.point);
    check(equal(cold_boot_point,cold_boot_target.point)&&cold_boot_target.clicks==1,
          "cold-boot zero-angle touch missed before the first runtime rotation");
    for(const int angle:{0,90,180,270})verify_angle(angle,true);
    // Demonstrate this regression detects the shipped 2.0.1 BSP's convenience
    // defaults, even though LVGL display rotation itself remains unchanged.
    active.touch_flags={1,0,1};unsigned caught=0;
    for(const int angle:{0,90,180,270})caught+=verify_angle(angle,false);
    std::printf("PASS: production cold-boot zero-angle hit + 28 corner/interior clicks, 64 swipe points; startup rotation failure stays closed; old BSP flags rejected (%u mismatches). Host simulation only.\n",caught);
    return 0;
}
