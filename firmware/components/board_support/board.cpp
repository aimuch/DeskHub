#include "board.hpp"
#include "core.hpp"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "driver/i2c_master.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdio>
#include <utility>
namespace dh {
static i2c_master_dev_handle_t pmic, rtc;
static SemaphoreHandle_t io_mutex, power_mutex;
static PowerState power;
static bool display_ok = false;
static lv_display_t *display;
static esp_err_t read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t n) {
    if (!dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(dev,&reg,1,data,n,100);
}
static bool add(uint8_t addr, i2c_master_dev_handle_t *dev) {
    auto bus = bsp_i2c_get_handle(); if (!bus) return false;
    i2c_device_config_t c{}; c.dev_addr_length = I2C_ADDR_BIT_LEN_7; c.device_address = addr; c.scl_speed_hz = 400000;
    return i2c_master_bus_add_device(bus,&c,dev) == ESP_OK;
}
esp_err_t board_init() {
    // Initialization runs before the UI/tasks. Failed initialization must keep
    // an explicitly unknown cache, never a fresh-looking power observation.
    power = PowerState{};
    power.fault = true;
    power.reason = "BOARD_INITIALIZATION_FAILED";
    io_mutex = xSemaphoreCreateMutex();
    power_mutex = xSemaphoreCreateMutex();
    if (!io_mutex || !power_mutex) {
        if (io_mutex) vSemaphoreDelete(io_mutex);
        if (power_mutex) vSemaphoreDelete(power_mutex);
        io_mutex = power_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    uint32_t flash = 0; esp_flash_get_size(nullptr,&flash);
    if (flash != 16u*1024*1024 || esp_psram_get_size() < 8u*1024*1024) {
        mutate([](Snapshot &s,void *) { s.error = "硬件容量与目标板不符，详见USB诊断"; });
    }
    auto e = bsp_i2c_init(); if (e != ESP_OK) return e;
    add(0x34,&pmic); add(0x51,&rtc);
    board_sample_power();
    time_t t;
    if (board_rtc_read(t)) {
        char b[32]; struct tm v{}; gmtime_r(&t,&v); strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&v); set_time_utc(b,"rtc");
    }
    return ESP_OK;
}
esp_err_t board_display_init() {
    // Vendor BSP 2.0.1 initializes only panel/touch here. No PMIC charger writes.
    display_ok = false;
    bsp_display_cfg_t cfg{};
    // Match the adapter defaults, keeping callback pointers zero-initialized.
    // Its C aggregate macro omits callbacks and triggers C++ -Werror.
    auto &adapter = cfg.lv_adapter_cfg;
    adapter.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    adapter.task_priority = ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY;
    adapter.task_core_id = ESP_LV_ADAPTER_DEFAULT_TASK_CORE_ID;
    adapter.tick_period_ms = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS;
    adapter.task_min_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS;
    adapter.task_max_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS;
    adapter.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED;
    adapter.auto_sleep.idle_timeout_ms = ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS;
    cfg.rotation = ESP_LV_ADAPTER_ROTATE_0;
    cfg.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    // CST9220 native coordinates already match CO5300 MADCTL=0 on this board,
    // as in SmartHub's Waveshare implementation. The BSP convenience start
    // adds a swap/mirror intended for another orientation. Start explicitly
    // with identity touch coordinates; LVGL applies each runtime rotation once.
    cfg.touch_flags.swap_xy = 0;
    cfg.touch_flags.mirror_x = 0;
    cfg.touch_flags.mirror_y = 0;
    display = bsp_display_start_with_config(&cfg);
    if (!display) return ESP_FAIL;
    // The vendor panel command table ends with MADCTL=0xa0 independently of
    // the adapter's zero rotation. Establish a matching physical zero baseline
    // even when the saved UI angle is zero and its first tick makes no change.
    const auto error = bsp_display_rotation_set(BSP_DISPLAY_ROTATE_0);
    if (error != ESP_OK) return error;
    display_ok = true;
    bsp_display_brightness_set(5);
    return ESP_OK;
}
esp_err_t board_brightness(int v) { return display_ok ? bsp_display_brightness_set(v) : ESP_ERR_INVALID_STATE; }
esp_err_t board_rotation(int degrees) {
    if(degrees<0||degrees>270||degrees%90)return ESP_ERR_INVALID_ARG;
    if(!display_ok||!display)return ESP_ERR_INVALID_STATE;
    // CO5300 rotates pixels in hardware. The BSP's 90/270 MADCTL values use
    // the opposite convention to LVGL's input correction (0x60 vs 0xa0).
    // Keep the adapter profile at zero and apply that correction exactly once.
    const auto error=bsp_display_rotation_set(static_cast<bsp_display_rotation_t>(degrees/90));
    if(error!=ESP_OK)return error;
    lv_display_set_rotation(display,static_cast<lv_display_rotation_t>(((360-degrees)%360)/90));
    if(auto *input=bsp_display_get_input_dev())lv_indev_wait_release(input);
    lv_obj_invalidate(lv_screen_active());
    return ESP_OK;
}
void board_sample_power() {
    if (!io_mutex || !power_mutex || !pmic) return;
    xSemaphoreTake(io_mutex,portMAX_DELAY);
    uint8_t id=0, status[2]{}, detection=0;
    PowerState next;
    // Read semantics cross-checked against official example's MIT XPowersLib:
    // STATUS1 0x00 bit3 battery, bit5 VBUS good; STATUS2 0x01 bits7:5 charging.
    // Fuel-gauge percentage: register 0xA4 returned raw, only with a battery present
    // (XPowersLib getBatteryPercent). Read-only; no ADC/gauge/charging enable writes.
    if (read(pmic,0x03,&id,1) != ESP_OK || (id & 0xcf) != 0x4a || read(pmic,0x00,status,2) != ESP_OK || read(pmic,0x68,&detection,1) != ESP_OK) {
        next.fault = true; next.reason = "PMIC_COMMUNICATION_FAULT";
    } else {
        next.vbus = (status[0] & 0x20) && !(status[1] & 8) ? "present" : "absent";
        if (detection & 1) next.battery = status[0] & 8 ? "present" : "absent";
        next.charging = next.battery == "absent" ? "idle" : next.battery == "unknown" ? "unknown" :
            (status[1] >> 5) == 1 ? "charging" : (status[1] & 7) == 4 ? "full" : "idle";
        next.reason = next.battery == "unknown" ? "BATTERY_DETECTION_DISABLED" : "SOC_UNVERIFIED";
        if (next.battery == "present") {
            // A failed or implausible gauge read leaves the level unknown. It must not
            // become 0 %, a clamped 100 %, or the previous value, and it does not turn
            // a healthy VBUS/charging reading into a PMIC fault.
            uint8_t level = 0;
            if (read(pmic,0xa4,&level,1) != ESP_OK) next.reason = "SOC_READ_FAILED";
            else if (level > 100) next.reason = "SOC_OUT_OF_RANGE";
            else { next.soc_percent = level; next.reason = "SOC_PMIC_ESTIMATE"; }
        }
    }
    next.observed_ms = monotonic_ms();
    // The UI only takes power_mutex. All potentially slow I2C work above is
    // outside that cache lock, including fault/timeout paths. Keep io_mutex
    // until publication so concurrent samplers cannot publish out of order.
    xSemaphoreTake(power_mutex,portMAX_DELAY);
    power = std::move(next);
    xSemaphoreGive(power_mutex);
    xSemaphoreGive(io_mutex);
}
PowerState board_power() {
    if (!power_mutex) {
        PowerState unavailable;
        unavailable.fault = true;
        unavailable.reason = "BOARD_INITIALIZATION_FAILED";
        return unavailable;
    }
    xSemaphoreTake(power_mutex,portMAX_DELAY);
    auto value = power;
    xSemaphoreGive(power_mutex);
    return value;
}
static int bcd(uint8_t v) { if ((v&15)>9 || (v>>4)>9) return -1; return (v>>4)*10+(v&15); }
static uint8_t packed(int v) { return (v/10)*16+v%10; }
bool board_rtc_read(time_t &result) {
    if (!io_mutex || !rtc) return false;
    xSemaphoreTake(io_mutex,portMAX_DELAY); uint8_t control=0, v[7]{};
    bool ok = read(rtc,0x00,&control,1) == ESP_OK && read(rtc,0x04,v,7) == ESP_OK;
    xSemaphoreGive(io_mutex);
    // OS bit signals lost oscillator integrity; STOP/test/12-hour are unsupported.
    if (!ok || (v[0]&0x80) || (control&0xa2)) return false;
    int sec=bcd(v[0]&0x7f),min=bcd(v[1]&0x7f),hour=bcd(v[2]&0x3f),day=bcd(v[3]&0x3f),month=bcd(v[5]&0x1f),year=bcd(v[6]);
    if (sec<0||min<0||hour<0||day<1||month<1||year<24) return false;
    char iso[32]; snprintf(iso,sizeof(iso),"20%02d-%02d-%02dT%02d:%02d:%02dZ",year,month,day,hour,min,sec);
    return parse_utc(iso,result);
}
bool board_rtc_write(time_t t) {
    if (!io_mutex || !rtc) return false;
    struct tm v{}; gmtime_r(&t,&v); if (v.tm_year < 124 || v.tm_year>199) return false;
    uint8_t bytes[8] = {0x04,packed(v.tm_sec),packed(v.tm_min),packed(v.tm_hour),packed(v.tm_mday),packed(v.tm_wday),packed(v.tm_mon+1),packed(v.tm_year%100)};
    xSemaphoreTake(io_mutex,portMAX_DELAY); uint8_t control=0;
    bool ok = read(rtc,0,&control,1) == ESP_OK;
    if (ok && !(control & 0x80)) {
        uint8_t cfg[2] = {0,static_cast<uint8_t>(control & ~0x22)};
        ok = i2c_master_transmit(rtc,cfg,2,100) == ESP_OK && i2c_master_transmit(rtc,bytes,sizeof(bytes),100) == ESP_OK;
    } else ok = false;
    xSemaphoreGive(io_mutex); return ok;
}
}
