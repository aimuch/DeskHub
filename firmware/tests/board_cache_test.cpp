// Compile and exercise the production board.cpp with deliberately slow I2C.
// These are host concurrency checks, not measurements of a physical board.
//
// Power-chip measurements and the whitelist of PMIC writes (docs/rules.md, R3).
// Ways this can fail, written before the code. Every failure must end as "unknown",
// never 0 V / 0 C / a stale number, and must never become a power fault:
//   W1  a write outside the whitelist (only ADC enable 0x30 bits 0/2/4 and battery detection 0x68 bit 0)
//   W2  a write when the bits are already set (the driver must be idempotent)
//   W3  a read-modify-write that drops or flips another bit (TS pin bit 1 of 0x30 in particular)
//   W4  a write the chip ignores: the driver must notice by reading back, and stop after 3 attempts
//   W5  a write that fails on the bus: same
//   W6  a failed read before the write: no write may be attempted on a guess
//   W7  a chip that does not identify itself as AXP2101: not a single write
//   W8  a PMIC reset that clears the bits later: they must be enabled again
//   W9  a slow write must not block the cached read the UI uses
//   M1  the ADC channel is not enabled (and cannot be): unknown, not 0
//   M2  only some channels are enabled: the others stay available
//   M3  the enable register cannot be read: nothing is measured, nothing is written
//   M4  a channel read fails, also right after a good sample (no stale carry-over)
//   M5  the ADC returns 0 (not converted yet) or an implausible value (battery 2500..4600 mV,
//       USB 3000..6500 mV, chip -40..150 C), also at both edges
//   M6  the reserved high bits of a result register are set (they must be masked)
//   M7  no battery / no USB: nothing to measure, and no VBAT/VBUS read is even attempted
//   M8  a measurement problem must never change USB/battery/charging/level or raise a power fault
//   M9  a slow ADC read must not block the cached read the UI uses
#include "board.hpp"
#include "core.hpp"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <vector>

using namespace std::chrono_literals;
static void check(bool value,const char *message) {
    if(!value){std::fprintf(stderr,"board cache regression: %s\n",message);std::abort();}
}
struct TestSemaphore { std::mutex value; };
static int allocations=0,fail_allocation=0;
static std::atomic<int> null_lock_attempts{0},rtc_writes{0},unlisted_writes{0};
static std::atomic<int64_t> current_ms{100};
static esp_err_t bus_result=ESP_OK;
static uint8_t power_status=0x28,charge_status=0x20;
// AXP2101 fuel-gauge percentage register 0xA4 (XPowersLib getBatteryPercent reads it raw).
static uint8_t gauge_register=67;
static bool gauge_read_fails=false;
static int gauge_reads=0;
// AXP2101 read-only measurements (XPowersLib): ADC enable 0x30 (bit0 VBAT, bit2 VBUS, bit4 die temperature),
// VBAT 0x34/0x35 (13 bit mV), VBUS 0x38/0x39 (14 bit mV), die temperature 0x3C/0x3D (14 bit, 22 + (7274 - raw) / 20 C).
// The reserved high bits of each high byte are set on purpose: a reader that does not mask them reads garbage.
// 0x30 and 0x68 are the two registers the driver may write, so they are a real register file here.
static uint8_t pmic_regs[256];
enum class WriteMode { Apply, Ignore, Error };
static WriteMode write_mode=WriteMode::Apply;
struct PmicWrite { uint8_t reg,old,value; };
static std::vector<PmicWrite> pmic_write_log;
static uint8_t pmic_id_value=0x4a;
static uint16_t vbat_raw=4120,vbus_raw=5030,temp_raw=6874;
static bool pmic_id_fails=false,adc_enable_read_fails=false,detection_read_fails=false,vbat_read_fails=false,vbus_read_fails=false,temp_read_fails=false;
static int adc_enable_reads=0,vbat_reads=0,vbus_reads=0,temp_reads=0;
static TestI2cDevice pmic_device{0x34},rtc_device{0x51};
static lv_display_t display;
static lv_indev_t input;
static lv_obj_t screen;
static std::mutex core_mutex;
static dh::Snapshot core_state;

// The whitelist, as the test knows it: the target bits end up set and every other bit keeps its value.
static bool write_is_whitelisted(const PmicWrite &w) {
    if(w.reg==0x30)return (w.value&0x15)==0x15&&(w.value&~0x15)==(w.old&~0x15);
    if(w.reg==0x68)return (w.value&0x01)==0x01&&(w.value&~0x01)==(w.old&~0x01);
    return false;
}

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
} slow,slow_write;

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
        const uint8_t reg=*write;
        if(reg==0x03&&size==1){if(pmic_id_fails)return ESP_ERR_TIMEOUT;*out=pmic_id_value;}
        else if(reg==0&&size==2){out[0]=power_status;out[1]=charge_status;}
        else if(reg==0x68&&size==1){if(detection_read_fails)return ESP_ERR_TIMEOUT;*out=pmic_regs[0x68];}
        else if(reg==0xa4&&size==1){++gauge_reads;if(gauge_read_fails)return ESP_ERR_TIMEOUT;*out=gauge_register;}
        else if(reg==0x30&&size==1){++adc_enable_reads;if(adc_enable_read_fails)return ESP_ERR_TIMEOUT;*out=pmic_regs[0x30];}
        else if(reg==0x34&&size==2){++vbat_reads;if(vbat_read_fails)return ESP_ERR_TIMEOUT;out[0]=0xe0|((vbat_raw>>8)&0x1f);out[1]=vbat_raw&0xff;}
        else if(reg==0x38&&size==2){++vbus_reads;if(vbus_read_fails)return ESP_ERR_TIMEOUT;out[0]=0xc0|((vbus_raw>>8)&0x3f);out[1]=vbus_raw&0xff;}
        else if(reg==0x3c&&size==2){++temp_reads;if(temp_read_fails)return ESP_ERR_TIMEOUT;out[0]=0xc0|((temp_raw>>8)&0x3f);out[1]=temp_raw&0xff;}
        else check(false,"unexpected PMIC register read");
    }else if(*write==0x04&&size==7){
        const uint8_t date[]={0x10,0x09,0x08,0x28,0x03,0x10,0x26};
        std::memcpy(out,date,sizeof(date));
    }
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,const uint8_t *buffer,size_t size,int timeout){
    check(timeout==100,"unexpected I2C write timeout");
    if(dev->address==0x34){
        check(size==2,"a PMIC write must be exactly one register and one value");
        if(slow_write.enter(0x34,buffer[0]))return ESP_ERR_TIMEOUT;
        const PmicWrite w{buffer[0],pmic_regs[buffer[0]],buffer[1]};
        pmic_write_log.push_back(w);
        if(!write_is_whitelisted(w))++unlisted_writes;
        if(write_mode==WriteMode::Error)return ESP_ERR_TIMEOUT;
        if(write_mode==WriteMode::Apply)pmic_regs[buffer[0]]=buffer[1];
    }else ++rtc_writes;
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

template<class F> static dh::PowerState cached_read_while_io_blocked(F operation,SlowRead &gate=slow){
    auto worker=std::async(std::launch::async,operation);
    gate.wait_entered();
    auto cached=std::async(std::launch::async,[]{return dh::board_power();});
    const bool ready=cached.wait_for(100ms)==std::future_status::ready;
    dh::PowerState result;
    if(ready)result=cached.get();
    gate.release();worker.get();
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
static bool near(float a,float b){return std::isfinite(a)&&std::fabs(a-b)<0.01f;}
int main(int argc,char **argv) {
    pmic_regs[0x30]=0x15;pmic_regs[0x68]=0x01;   // an AXP2101 whose ADC channels and battery detection are already on
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
    check(initial.battery_mv==4120&&initial.vbus_mv==5030&&near(initial.chip_temp_c,42.0f)&&initial.measure_reason=="ADC_OK","enabled channels were not measured");
    {const dh::PowerState fresh;check(fresh.battery_mv==-1&&fresh.vbus_mv==-1&&std::isnan(fresh.chip_temp_c)&&fresh.measure_reason.empty(),"an unmeasured state must be unknown");}
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

    // ---- whitelist of PMIC writes ----
    // A healthy chip again, and the driver's failure counter cleared, with the logs emptied.
    auto healthy=[]{
        write_mode=WriteMode::Apply;pmic_id_fails=false;pmic_id_value=0x4a;
        adc_enable_read_fails=detection_read_fails=vbat_read_fails=vbus_read_fails=temp_read_fails=false;
        pmic_regs[0x30]=0x15;pmic_regs[0x68]=0x01;
        vbat_raw=4120;vbus_raw=5030;temp_raw=6874;
        power_status=0x28;charge_status=0x20;gauge_register=67;gauge_read_fails=false;
        dh::board_sample_power();
        adc_enable_reads=vbat_reads=vbus_reads=temp_reads=gauge_reads=0;pmic_write_log.clear();
    };
    healthy();dh::board_sample_power();
    check(pmic_write_log.empty(),"W2: a chip with the bits already set was written to");
    // cold chip: both whitelisted registers are enabled, once
    healthy();pmic_regs[0x30]=0x00;pmic_regs[0x68]=0x00;dh::board_sample_power();
    check(pmic_write_log.size()==2,"W1/W2: a cold chip needs exactly two writes");
    check(pmic_regs[0x30]==0x15&&pmic_regs[0x68]==0x01,"the whitelisted bits were not enabled");
    {const auto v=dh::board_power();
     check(v.battery=="present"&&v.battery_mv==4120&&v.vbus_mv==5030&&near(v.chip_temp_c,42.0f)&&v.measure_reason=="ADC_OK"&&!v.fault,"a freshly enabled chip was not measured in the same sample");}
    pmic_write_log.clear();dh::board_sample_power();
    check(pmic_write_log.empty(),"W2: the enable was repeated although the bits stayed set");
    // W3: other bits survive. 0x0A has the TS pin bit (1) and VSYS (3); 0xF0 has the upper nibble of the detection register.
    healthy();pmic_regs[0x30]=0x0a;pmic_regs[0x68]=0xf0;dh::board_sample_power();
    check(pmic_regs[0x30]==0x1f&&pmic_regs[0x68]==0xf1,"W3: a read-modify-write dropped or flipped another bit");
    check((pmic_regs[0x30]&0x02)==0x02,"W3: the TS pin bit changed");
    // W4: the chip ignores writes. Unknown, not a fault, bounded retries, then normal again when the bits appear.
    healthy();write_mode=WriteMode::Ignore;pmic_regs[0x30]=0x00;dh::board_sample_power();
    {const auto v=dh::board_power();
     check(v.measure_reason=="ADC_ENABLE_FAILED"&&v.battery_mv==-1&&v.vbus_mv==-1&&std::isnan(v.chip_temp_c),"W4: an ignored write still produced numbers");
     check(!v.fault&&v.vbus=="present"&&v.battery=="present"&&v.charging=="charging"&&v.soc_percent==67,"W4: a failed enable changed the power state");}
    check(pmic_write_log.size()==1,"W4: first attempt must be one write");
    dh::board_sample_power();dh::board_sample_power();
    check(pmic_write_log.size()==3,"W4: three attempts expected");
    for(int i=0;i<3;++i)dh::board_sample_power();
    check(pmic_write_log.size()==3,"W4: the driver kept writing after three failed attempts");
    check(vbat_reads+vbus_reads+temp_reads==0,"M1: disabled channels were read");
    pmic_regs[0x30]=0x15;pmic_write_log.clear();dh::board_sample_power();
    check(pmic_write_log.empty()&&dh::board_power().measure_reason=="ADC_OK"&&dh::board_power().battery_mv==4120,"W4: measurements did not come back when the bits appeared");
    write_mode=WriteMode::Apply;pmic_regs[0x30]=0x00;dh::board_sample_power();   // W8: a PMIC reset later
    check(pmic_write_log.size()==1&&pmic_regs[0x30]==0x15&&dh::board_power().measure_reason=="ADC_OK","W8: the bits were not enabled again after a PMIC reset");
    // W5: the write fails on the bus
    healthy();write_mode=WriteMode::Error;pmic_regs[0x30]=0x00;dh::board_sample_power();
    {const auto v=dh::board_power();
     check(v.measure_reason=="ADC_ENABLE_FAILED"&&!v.fault&&v.battery_mv==-1&&v.soc_percent==67,"W5: a failed write was not reported as unknown");}
    for(int i=0;i<5;++i)dh::board_sample_power();
    check(pmic_write_log.size()==3,"W5: the driver did not stop after three failed writes");
    // W6: a failed read before the write: no write on a guess
    healthy();pmic_regs[0x30]=0x00;adc_enable_read_fails=true;dh::board_sample_power();
    {const auto v=dh::board_power();
     check(pmic_write_log.empty()&&v.measure_reason=="ADC_READ_FAILED"&&!v.fault&&v.battery_mv==-1&&v.vbus_mv==-1&&std::isnan(v.chip_temp_c),"W6: an unreadable enable register led to a write or to numbers");}
    healthy();pmic_regs[0x68]=0x00;detection_read_fails=true;dh::board_sample_power();
    check(pmic_write_log.empty()&&dh::board_power().fault&&dh::board_power().reason=="PMIC_COMMUNICATION_FAULT","W6: an unreadable detection register led to a write or was hidden");
    // W7: not an AXP2101, or not answering: not a single write, not a single measurement read
    healthy();pmic_regs[0x30]=0x00;pmic_id_fails=true;dh::board_sample_power();
    {const auto v=dh::board_power();
     check(pmic_write_log.empty()&&adc_enable_reads==0&&v.fault&&v.battery_mv==-1&&v.vbus_mv==-1&&std::isnan(v.chip_temp_c),"W7: an unanswering chip was written to or measured");}
    healthy();pmic_regs[0x30]=0x00;pmic_id_value=0x12;dh::board_sample_power();
    {const auto v=dh::board_power();
     check(pmic_write_log.empty()&&adc_enable_reads==0&&v.fault&&v.battery_mv==-1,"W7: a different chip was written to or measured");}
    // W9: a slow write must not block the cached read
    healthy();current_ms=500;dh::board_sample_power();
    pmic_regs[0x30]=0x00;current_ms=600;slow_write.arm(0x34,0x30);
    {auto while_write=cached_read_while_io_blocked([]{dh::board_sample_power();},slow_write);
     check(while_write.observed_ms==500&&while_write.battery_mv==4120,"W9: a half-finished sample leaked into the cache during a slow write");
     check(dh::board_power().observed_ms==600&&dh::board_power().measure_reason=="ADC_OK","W9: the finished sample was not published whole");}

    // ---- measurements ----
    healthy();dh::board_sample_power();
    const auto base=dh::board_power();
    check(base.battery_mv==4120&&base.vbus_mv==5030&&near(base.chip_temp_c,42.0f)&&base.measure_reason=="ADC_OK","M6: baseline sample wrong (reserved high bits not masked?)");
    auto unaffected=[&](const dh::PowerState &v,const char *what){
        check(v.fault==base.fault&&v.vbus==base.vbus&&v.battery==base.battery&&v.charging==base.charging&&v.soc_percent==base.soc_percent,what);};
    // M1/M2: one channel at a time, and the driver cannot enable it
    for(const auto mask:{0x01,0x04,0x10}){
        healthy();write_mode=WriteMode::Ignore;pmic_regs[0x30]=static_cast<uint8_t>(0x15&~mask);dh::board_sample_power();
        const auto v=dh::board_power();
        check(v.measure_reason=="ADC_ENABLE_FAILED","M1: a disabled channel was not reported");
        check((mask==0x01)==(v.battery_mv==-1)&&(mask==0x04)==(v.vbus_mv==-1)&&(mask==0x10)==std::isnan(v.chip_temp_c),"M2: a disabled channel was shown, or another one was hidden");
        check((mask!=0x01||vbat_reads==0)&&(mask!=0x04||vbus_reads==0)&&(mask!=0x10||temp_reads==0),"M1: a disabled channel was read anyway");
        unaffected(v,"M8: a disabled ADC channel changed the power state");
    }
    healthy();write_mode=WriteMode::Ignore;pmic_regs[0x30]=0;dh::board_sample_power();
    {const auto v=dh::board_power();
     check(v.battery_mv==-1&&v.vbus_mv==-1&&std::isnan(v.chip_temp_c)&&v.measure_reason=="ADC_ENABLE_FAILED"&&vbat_reads+vbus_reads+temp_reads==0,"M1: all channels off still produced numbers");
     unaffected(v,"M8: disabled ADC changed the power state");}
    // M3: the enable register cannot be read
    healthy();adc_enable_read_fails=true;dh::board_sample_power();
    {const auto v=dh::board_power();
     check(v.battery_mv==-1&&v.vbus_mv==-1&&std::isnan(v.chip_temp_c)&&v.measure_reason=="ADC_READ_FAILED"&&vbat_reads+vbus_reads+temp_reads==0&&pmic_write_log.empty(),"M3: an unknown ADC enable state still read channels or wrote");
     unaffected(v,"M8: an unreadable ADC enable register changed the power state");}
    // M4: a good sample first, then each read failing, so a stale value would show
    for(int channel=0;channel<3;++channel){
        healthy();dh::board_sample_power();
        check(dh::board_power().battery_mv==4120,"setup sample missing");
        vbat_read_fails=channel==0;vbus_read_fails=channel==1;temp_read_fails=channel==2;dh::board_sample_power();
        const auto v=dh::board_power();
        check((channel==0)==(v.battery_mv==-1)&&(channel==1)==(v.vbus_mv==-1)&&(channel==2)==std::isnan(v.chip_temp_c),"M4: a failed read kept a stale value or hid a good one");
        check(v.measure_reason=="ADC_READ_FAILED","M4: a failed read was not reported");
        unaffected(v,"M8: a failed ADC read changed the power state");
    }
    // M5: zero and the plausible range, both edges
    struct Case{uint16_t *raw;uint16_t value;bool valid;int which;};  // which: 0 battery, 1 USB, 2 chip
    const Case cases[]={{&vbat_raw,0,false,0},{&vbat_raw,2499,false,0},{&vbat_raw,2500,true,0},{&vbat_raw,4600,true,0},{&vbat_raw,4601,false,0},{&vbat_raw,8191,false,0},
                        {&vbus_raw,0,false,1},{&vbus_raw,2999,false,1},{&vbus_raw,3000,true,1},{&vbus_raw,6500,true,1},{&vbus_raw,6501,false,1},{&vbus_raw,16383,false,1},
                        {&temp_raw,0,false,2},{&temp_raw,4713,false,2},{&temp_raw,4714,true,2},{&temp_raw,8514,true,2},{&temp_raw,8515,false,2},{&temp_raw,16383,false,2}};
    for(const auto &c:cases){
        healthy();*c.raw=c.value;dh::board_sample_power();
        const auto v=dh::board_power();
        const bool shown=c.which==0?v.battery_mv>=0:c.which==1?v.vbus_mv>=0:std::isfinite(v.chip_temp_c);
        check(shown==c.valid,"M5: plausible range edge wrong");
        check(c.valid?v.measure_reason=="ADC_OK":v.measure_reason=="ADC_OUT_OF_RANGE","M5: range verdict not reported");
        if(c.valid&&c.which==0)check(v.battery_mv==c.value,"battery value changed on the way");
        if(c.valid&&c.which==2)check(near(v.chip_temp_c,22.0f+(7274-c.value)/20.0f),"temperature formula wrong");
        unaffected(v,"M8: an implausible ADC value changed the power state");
    }
    // M7: nothing to measure
    healthy();power_status=0x20;dh::board_sample_power();   // USB only, no battery
    {const auto v=dh::board_power();
     check(v.battery_mv==-1&&vbat_reads==0&&v.vbus_mv==5030&&near(v.chip_temp_c,42.0f)&&v.measure_reason=="ADC_OK","M7: no battery: a battery voltage appeared or was read");}
    healthy();power_status=0x08;dh::board_sample_power();   // battery, no USB
    {const auto v=dh::board_power();
     check(v.vbus_mv==-1&&vbus_reads==0&&v.battery_mv==4120&&near(v.chip_temp_c,42.0f)&&v.measure_reason=="ADC_OK","M7: no USB: a USB voltage appeared or was read");}
    healthy();power_status=0;dh::board_sample_power();      // neither
    {const auto v=dh::board_power();
     check(v.battery_mv==-1&&v.vbus_mv==-1&&near(v.chip_temp_c,42.0f)&&vbat_reads+vbus_reads==0&&v.measure_reason=="ADC_OK","M7: nothing connected: absent channels were read or reported as a problem");}
    // M9: a slow ADC read (the last register read of a sample) must not block the cached read the UI uses
    healthy();current_ms=500;dh::board_sample_power();
    check(near(dh::board_power().chip_temp_c,42.0f)&&dh::board_power().observed_ms==500,"setup sample before the slow ADC read missing");
    temp_raw=7074;current_ms=600;slow.arm(0x34,0x3c);
    {auto while_adc=cached_read_while_io_blocked([]{dh::board_sample_power();});
     check(while_adc.observed_ms==500&&near(while_adc.chip_temp_c,42.0f)&&while_adc.battery_mv==4120,"M9: a half-finished sample leaked into the cache during a slow ADC read");
     check(dh::board_power().observed_ms==600&&near(dh::board_power().chip_temp_c,32.0f),"M9: the finished sample was not published whole");}
    // Back to the state the remaining checks expect: one healthy sample at t = 100 ms.
    current_ms=100;healthy();

    // Block on a PMIC register in the middle of a sample: partial hardware reads cannot leak into
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
    check(unlisted_writes.load()==0,"a PMIC write outside the whitelist happened during the tests");
    check(rtc_writes.load()==2,"test changed the RTC write contract");
    check(null_lock_attempts.load()==0,"normal operation touched null mutex");
    std::puts("production board cache: slow PMIC/RTC do not block UI cache reads; PMIC writes stay on the whitelist; measurements are unknown, never invented; publication, fault/timestamp and initialization failures passed");
}
