// Compile and exercise the production board.cpp with deliberately slow I2C.
// These are host concurrency checks, not measurements of a physical board.
#include "board.hpp"
#include "core.hpp"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "freertos/semphr.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>

using namespace std::chrono_literals;
static void check(bool value,const char *message) {
    if(!value){std::fprintf(stderr,"board cache regression: %s\n",message);std::abort();}
}
struct TestSemaphore { std::mutex value; };
static int allocations=0,fail_allocation=0;
static std::atomic<int> null_lock_attempts{0},pmic_writes{0},rtc_writes{0};
static std::atomic<int64_t> current_ms{100};
static esp_err_t bus_result=ESP_OK;
static uint8_t power_status=0x28,charge_status=0x20;
// AXP2101 fuel-gauge percentage register 0xA4 (XPowersLib getBatteryPercent reads it raw).
static uint8_t gauge_register=67;
static bool gauge_read_fails=false;
static int gauge_reads=0;
static TestI2cDevice pmic_device{0x34},rtc_device{0x51};
static lv_display_t display;
static lv_indev_t input;
static lv_obj_t screen;
static std::mutex core_mutex;
static dh::Snapshot core_state;

struct SlowRead {
    std::mutex mutex;
    std::condition_variable condition;
    int address=-1,reg=-1;
    bool entered=false,released=false,timeout=false;
    void arm(int target,int register_address,bool fail=false){
        std::lock_guard<std::mutex> guard(mutex);
        address=target;reg=register_address;entered=released=false;timeout=fail;
    }
    bool enter(int target,int register_address){
        std::unique_lock<std::mutex> guard(mutex);
        if(address!=target||reg!=register_address)return false;
        entered=true;condition.notify_all();
        condition.wait(guard,[&]{return released;});
        address=reg=-1;return timeout;
    }
    void wait_entered(){
        std::unique_lock<std::mutex> guard(mutex);
        check(condition.wait_for(guard,2s,[&]{return entered;}),"sampler never entered fake I2C");
    }
    void release(){
        std::lock_guard<std::mutex> guard(mutex);
        released=true;condition.notify_all();
    }
} slow;

SemaphoreHandle_t xSemaphoreCreateMutex(){
    ++allocations;return allocations==fail_allocation?nullptr:new TestSemaphore;
}
int xSemaphoreTake(SemaphoreHandle_t handle,uint32_t){
    if(!handle){++null_lock_attempts;check(false,"attempted to take a null mutex");}
    handle->value.lock();return 1;
}
int xSemaphoreGive(SemaphoreHandle_t handle){
    check(handle!=nullptr,"attempted to release a null mutex");handle->value.unlock();return 1;
}
void vSemaphoreDelete(SemaphoreHandle_t handle){delete handle;}
esp_err_t bsp_i2c_init(){return bus_result;}
i2c_master_bus_handle_t bsp_i2c_get_handle(){return bus_result==ESP_OK?&pmic_device:nullptr;}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t,const i2c_device_config_t *cfg,i2c_master_dev_handle_t *out){
    *out=cfg->device_address==0x34?&pmic_device:&rtc_device;return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,const uint8_t *write,size_t write_size,uint8_t *out,size_t size,int timeout){
    check(dev&&write_size==1&&timeout==100,"unexpected I2C read contract");
    if(slow.enter(dev->address,*write))return ESP_ERR_TIMEOUT;
    std::memset(out,0,size);
    if(dev->address==0x34){
        if(*write==0x03&&size==1)*out=0x4a;
        else if(*write==0&&size==2){out[0]=power_status;out[1]=charge_status;}
        else if(*write==0x68&&size==1)*out=1;
        else if(*write==0xa4&&size==1){++gauge_reads;if(gauge_read_fails)return ESP_ERR_TIMEOUT;*out=gauge_register;}
        else check(false,"unexpected PMIC register read");
    }else if(*write==0x04&&size==7){
        const uint8_t date[]={0x10,0x09,0x08,0x28,0x03,0x10,0x26};
        std::memcpy(out,date,sizeof(date));
    }
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,const uint8_t *,size_t,int timeout){
    check(timeout==100,"unexpected I2C write timeout");
    if(dev->address==0x34)++pmic_writes;else ++rtc_writes;
    return ESP_OK;
}
esp_err_t esp_flash_get_size(void *,uint32_t *out){*out=16u*1024*1024;return ESP_OK;}
size_t esp_psram_get_size(){return 8u*1024*1024;}
lv_display_t *bsp_display_start(){return &display;}
lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg){
    check(cfg&&cfg->rotation==ESP_LV_ADAPTER_ROTATE_0&&
          cfg->tear_avoid_mode==ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE&&
          !cfg->touch_flags.swap_xy&&!cfg->touch_flags.mirror_x&&!cfg->touch_flags.mirror_y,
          "board startup changed the native touch coordinate system");
    return &display;
}
lv_indev_t *bsp_display_get_input_dev(){return &input;}
esp_err_t bsp_display_brightness_set(int){return ESP_OK;}
esp_err_t bsp_display_rotation_set(bsp_display_rotation_t){return ESP_OK;}
void lv_display_set_rotation(lv_display_t *,lv_display_rotation_t){}
void lv_indev_wait_release(lv_indev_t *){}
lv_obj_t *lv_screen_active(){return &screen;}
void lv_obj_invalidate(lv_obj_t *){}
namespace dh {
int64_t monotonic_ms(){return current_ms.load();}
void mutate(void (*fn)(Snapshot &,void *),void *context){std::lock_guard<std::mutex> guard(core_mutex);fn(core_state,context);}
bool parse_utc(const char *,time_t &out){out=1793174950;return true;}
bool set_time_utc(const char *,const char *){return true;}
}

template<class F> static dh::PowerState cached_read_while_io_blocked(F operation){
    auto worker=std::async(std::launch::async,operation);
    slow.wait_entered();
    auto cached=std::async(std::launch::async,[]{return dh::board_power();});
    const bool ready=cached.wait_for(100ms)==std::future_status::ready;
    dh::PowerState result;
    if(ready)result=cached.get();
    slow.release();worker.get();
    if(!ready)(void)cached.get();
    check(ready,"cached UI read waited for slow hardware I/O");
    return result;
}
static void check_initialization_failure(){
    const auto value=dh::board_power();
    check(value.fault&&value.vbus=="unknown"&&value.battery=="unknown"&&value.observed_ms==0,"initialization failure invented power data");
    dh::board_sample_power();time_t t=0;
    check(!dh::board_rtc_read(t)&&!dh::board_rtc_write(1793174950),"uninitialized RTC accepted operation");
    check(dh::board_power().observed_ms==0,"missing hardware invented a sample timestamp");
    check(null_lock_attempts.load()==0,"initialization failure touched null mutex");
}
int main(int argc,char **argv){
    // Pre-init callers are safe even before any semaphore exists.
    check_initialization_failure();
    if(argc==2&&std::string(argv[1])=="--mutex-failure"){
        fail_allocation=2;check(dh::board_init()==ESP_ERR_NO_MEM,"mutex allocation failure hidden");
        check_initialization_failure();return 0;
    }
    if(argc==2&&std::string(argv[1])=="--i2c-failure"){
        bus_result=ESP_FAIL;check(dh::board_init()==ESP_FAIL,"I2C initialization failure hidden");
        check_initialization_failure();return 0;
    }
    check(dh::board_init()==ESP_OK,"board initialization failed");
    check(dh::board_display_init()==ESP_OK,"explicit display initialization failed");
    auto initial=dh::board_power();
    check(!initial.fault&&initial.vbus=="present"&&initial.battery=="present"&&initial.charging=="charging"&&initial.observed_ms==100,"initial sample incorrect");
    // Battery level is the PMIC gauge's own estimate, shown only with a present battery.
    check(initial.soc_percent==67&&initial.reason=="SOC_PMIC_ESTIMATE","present battery did not publish the gauge estimate");
    check(dh::PowerState{}.soc_percent==-1,"an unread level must be unknown, not a number");
    // Failure modes: every one of these must end as "unknown", never 0 % or a stale/clamped number.
    gauge_register=0;dh::board_sample_power();
    check(dh::board_power().soc_percent==0&&dh::board_power().battery=="present","a real 0 % reading was hidden");
    gauge_register=101;dh::board_sample_power();
    check(dh::board_power().soc_percent==-1&&dh::board_power().reason=="SOC_OUT_OF_RANGE","out-of-range level was clamped or shown");
    gauge_register=0x7f;dh::board_sample_power();
    check(dh::board_power().soc_percent==-1,"reserved gauge value was shown");
    gauge_register=67;gauge_read_fails=true;dh::board_sample_power();
    {const auto v=dh::board_power();check(v.soc_percent==-1&&v.reason=="SOC_READ_FAILED"&&!v.fault&&v.vbus=="present"&&v.charging=="charging","a failed gauge read leaked into the level or the VBUS/charging state");}
    gauge_read_fails=false;
    power_status=0x20;gauge_register=67;gauge_reads=0;dh::board_sample_power();  // VBUS only, no battery
    check(dh::board_power().battery=="absent"&&dh::board_power().soc_percent==-1&&gauge_reads==0,"level shown or even read without a battery");
    power_status=0x28;dh::board_sample_power();
    check(dh::board_power().soc_percent==67,"level did not come back with the battery");

    // Block on the last PMIC register: partial hardware reads cannot leak into
    // the cache, and a new timestamp is not visible before sampling completes.
    power_status=charge_status=0;current_ms=200;slow.arm(0x34,0x68);
    auto while_sampling=cached_read_while_io_blocked([]{dh::board_sample_power();});
    check(while_sampling.observed_ms==100&&while_sampling.vbus=="present"&&while_sampling.battery=="present"&&!while_sampling.fault,"partial sample replaced the previous cache");
    const auto sampled=dh::board_power();
    check(sampled.observed_ms==200&&sampled.vbus=="absent"&&sampled.battery=="absent"&&sampled.charging=="idle"&&!sampled.fault,"completed sample not published as a whole");

    current_ms=350;slow.arm(0x34,0x03,true);
    auto while_failing=cached_read_while_io_blocked([]{dh::board_sample_power();});
    check(while_failing.observed_ms==200&&!while_failing.fault,"failure altered cache before I2C returned");
    const auto failed=dh::board_power();
    check(failed.fault&&failed.reason=="PMIC_COMMUNICATION_FAULT"&&failed.observed_ms==350&&failed.vbus=="unknown"&&failed.battery=="unknown","failed read fabricated power presence or time");

    // RTC multi-step traffic still serializes on the I/O lock but cannot hold
    // the cached-power reader, and cannot freshen an unrelated PMIC sample.
    current_ms=400;slow.arm(0x51,0);
    auto while_rtc_write=cached_read_while_io_blocked([]{check(dh::board_rtc_write(1793174950),"RTC write failed");});
    check(while_rtc_write.fault&&while_rtc_write.observed_ms==350,"RTC write changed the power observation");
    slow.arm(0x51,0);
    auto while_rtc_read=cached_read_while_io_blocked([]{time_t t;check(dh::board_rtc_read(t),"RTC read failed");});
    check(while_rtc_read.observed_ms==350&&dh::board_power().observed_ms==350,"RTC read freshened power timestamp");
    check(pmic_writes.load()==0&&rtc_writes.load()==2,"test observed a PMIC write or changed RTC write contract");
    check(null_lock_attempts.load()==0,"normal operation touched null mutex");
    std::puts("production board cache: slow PMIC/RTC do not block UI cache reads; publication, fault/timestamp and initialization failures passed");
}
