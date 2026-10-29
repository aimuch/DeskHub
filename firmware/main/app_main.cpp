#include "core.hpp"
#include "board.hpp"
#include "ui.hpp"
#include "power_wire.hpp"
#include "charge.hpp"
#include "agent_client.hpp"
#include "management.hpp"
#include "json_util.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>
#include <cstring>
#include <ctime>
#include <cctype>
#include <cmath>

namespace {
std::string field(cJSON *o,const char *k){auto *v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:"";}
cJSON *result(bool success,const char *code=nullptr,const char *message=nullptr) {
    auto *o=cJSON_CreateObject();cJSON_AddBoolToObject(o,"ok",success);
    if(!success){auto *e=cJSON_AddObjectToObject(o,"error");cJSON_AddStringToObject(e,"code",code);cJSON_AddStringToObject(e,"message",message);}
    return o;
}
bool safe_id(const std::string &id) {
    if(id.empty()||id.size()>96||!std::isalnum(static_cast<unsigned char>(id[0])))return false;
    for(unsigned char c:id) {
        if(!std::isalnum(c)&&c!='_'&&c!='-'&&c!='.'&&c!=':')return false;
    }
    return true;
}
cJSON *handle(cJSON *request) {
    const auto op=field(request,"op");
    if(op=="identify" || op=="status") {
        auto s=dh::snapshot();auto *o=result(true),*r=cJSON_AddObjectToObject(o,"result");
        cJSON_AddStringToObject(r,"device_id",s.device_id.c_str());cJSON_AddStringToObject(r,"firmware_version",dh::VERSION);cJSON_AddStringToObject(r,"protocol_version","1.0");cJSON_AddStringToObject(r,"board_model","ESP32-S3-Touch-AMOLED-2.16");
        auto *caps=cJSON_AddObjectToObject(r,"capabilities");
        // Battery level is the PMIC gauge's own reading: readable now, but never validated against this cell.
        const auto power=dh::board_power();
        // Charge protection is an experiment (rules R1): implemented, never verified on a real cell.
        const auto charge=dh::charge::status();
        for(auto key:{"usb_provisioning","charge_limit","battery_soc","audio"}){
            const bool usb=strcmp(key,"usb_provisioning")==0,soc=strcmp(key,"battery_soc")==0,soc_ready=soc&&power.soc_percent>=0;
            const bool limit=strcmp(key,"charge_limit")==0,limit_ready=limit&&charge.charging_known&&power.battery=="present";
            auto *cap=cJSON_AddObjectToObject(caps,key);
            cJSON_AddStringToObject(cap,"support",usb||soc||limit?"unverified":"unsupported");
            cJSON_AddStringToObject(cap,"availability",usb||soc_ready||limit_ready?"ready":"unavailable");
            cJSON_AddStringToObject(cap,"reason_code",usb?"HARDWARE_TEST_PENDING":soc_ready?"PMIC_ESTIMATE_UNVERIFIED":soc?(power.battery=="absent"?"NO_BATTERY":"SENSOR_UNAVAILABLE"):
                limit_ready?"EXPERIMENTAL":limit?(power.battery=="absent"?"NO_BATTERY":"SWITCH_UNREADABLE"):"NOT_VERIFIED_OR_NOT_IMPLEMENTED");
        }
        if(op=="status"){cJSON_AddStringToObject(r,"wifi",s.wifi.c_str());cJSON_AddStringToObject(r,"agent",s.agent.c_str());cJSON_AddStringToObject(r,"time_quality",s.time_quality.c_str());cJSON_AddStringToObject(r,"error",s.error.c_str());cJSON_AddNumberToObject(r,"config_revision",dh::config().revision);cJSON_AddNumberToObject(r,"provision_revision",dh::secrets().revision);cJSON_AddBoolToObject(r,"configured",dh::secrets().ready());}
        if(op=="status"){
            cJSON_AddBoolToObject(r,"ui_alive",dh::ui_alive());
            cJSON_AddItemToObject(r,"power",dh::power_report_json(power,dh::monotonic_ms()));
            auto *chg=cJSON_AddObjectToObject(r,"charge");
            cJSON_AddBoolToObject(chg,"enabled",charge.enabled);cJSON_AddNumberToObject(chg,"limit",charge.limit);cJSON_AddNumberToObject(chg,"resume",charge.resume);
            cJSON_AddStringToObject(chg,"state",dh::charge::state_name(charge.state));cJSON_AddStringToObject(chg,"reason",charge.reason);
            cJSON_AddBoolToObject(chg,"faulted",charge.faulted);cJSON_AddNumberToObject(chg,"stop_failures",charge.stop_failures);
            cJSON_AddStringToObject(chg,"switch",!charge.charging_known?"unknown":charge.charging?"charging":"stopped");
            auto *events=cJSON_AddArrayToObject(chg,"events");   // the log of rules K6, newest last
            for(const auto &e:dh::charge::recent_events()){
                auto *item=cJSON_CreateObject();cJSON_AddNumberToObject(item,"at_ms",static_cast<double>(e.at_ms));
                cJSON_AddStringToObject(item,"request",e.charge?"charge":"stop");cJSON_AddStringToObject(item,"result",dh::charge::write_name(e.result));
                cJSON_AddStringToObject(item,"reason",e.reason);cJSON_AddItemToArray(events,item);
            }
            const auto configuration=dh::config();
            cJSON_AddStringToObject(r,"theme",configuration.layout.theme.c_str());
            cJSON_AddNumberToObject(r,"rotation",configuration.rotation);
        }
        return o;
    }
    if(op=="display_preferences") {
        auto c=dh::config();bool theme_seen=false,rotation_seen=false,op_seen=false;
        auto invalid=[](){return result(false,"INVALID_ARGUMENT","仅接受主题与 0/90/180/270 度屏幕方向");};
        cJSON *item;cJSON_ArrayForEach(item,request){
            if(!item->string)return invalid();
            if(strcmp(item->string,"op")==0){if(op_seen)return invalid();op_seen=true;}
            else if(strcmp(item->string,"theme")==0){if(theme_seen||!cJSON_IsString(item))return invalid();theme_seen=true;c.layout.theme=item->valuestring;}
            else if(strcmp(item->string,"rotation")==0){
                if(rotation_seen||!cJSON_IsNumber(item)||!std::isfinite(item->valuedouble)||item->valuedouble<0||item->valuedouble>270||std::floor(item->valuedouble)!=item->valuedouble||item->valueint%90)return invalid();
                rotation_seen=true;c.rotation=item->valueint;
            }else return invalid();
        }
        if((!theme_seen&&!rotation_seen)||!dh::valid_config(c))return invalid();
        if(dh::save_config(c)!=ESP_OK)return result(false,"CONFIG_COMMIT_FAILED","显示偏好未保存");
        auto *o=result(true),*r=cJSON_AddObjectToObject(o,"result");
        cJSON_AddBoolToObject(r,"saved",true);cJSON_AddBoolToObject(r,"application_pending",true);
        cJSON_AddStringToObject(r,"theme",c.layout.theme.c_str());cJSON_AddNumberToObject(r,"rotation",c.rotation);
        cJSON_AddNumberToObject(r,"config_revision",dh::config().revision);return o;
    }
    if(op=="provision") {
        auto *c=cJSON_GetObjectItemCaseSensitive(request,"config"),*wifi=cJSON_GetObjectItemCaseSensitive(c,"wifi"),*agent=cJSON_GetObjectItemCaseSensitive(c,"agent"),*tm=cJSON_GetObjectItemCaseSensitive(c,"time");
        dh::Secrets secrets;secrets.ssid=field(wifi,"ssid");secrets.password=field(wifi,"password");secrets.url=field(agent,"url");secrets.agent_id=field(agent,"agent_id");secrets.token=field(agent,"token");secrets.ca_pem=field(agent,"ca_pem");
        secrets.update_public_key_pem=dh::secrets().update_public_key_pem;
        auto *updates=cJSON_GetObjectItemCaseSensitive(c,"updates");
        if(updates){secrets.update_key_explicit=true;secrets.update_public_key_pem=field(updates,"public_key_pem");if(!dh::update_public_key_valid(secrets.update_public_key_pem))return result(false,"INVALID_ARGUMENT","更新公钥必须为 P-256 SPKI PEM");}
        auto ntp=field(tm,"ntp_server");if(!ntp.empty())secrets.ntp_server=ntp;
        const auto utc=field(tm,"utc");time_t parsed;
        bool token_ok=secrets.token.size()>=32&&secrets.token.size()<=256;for(unsigned char ch:secrets.token)if(ch<33||ch>126)token_ok=false;
        auto origin=secrets.url.substr(std::min<size_t>(8,secrets.url.size()));
        bool url_ok=secrets.url.rfind("https://",0)==0&&!origin.empty()&&origin.find_first_of("/?#@\r\n ")==std::string::npos;
        if(!url_ok||!safe_id(secrets.agent_id)||!token_ok||!dh::valid_ntp_server(secrets.ntp_server)||secrets.ssid.empty()||secrets.ssid.size()>32||secrets.password.size()>63||(!secrets.password.empty()&&secrets.password.size()<8)||
            secrets.ca_pem.size()>8192||secrets.ca_pem.find("-----BEGIN CERTIFICATE-----")==std::string::npos||!dh::parse_utc(utc.c_str(),parsed))
            return result(false,"INVALID_ARGUMENT","配置字段或时间无效");
        if(!dh::set_time_utc(utc.c_str(),"manual"))return result(false,"TIME_SET_FAILED","无法设置时间");
        auto e=dh::save_secrets(secrets);if(e!=ESP_OK)return result(false,e==ESP_ERR_NOT_SUPPORTED?"CONFIG_SCHEMA_UNSUPPORTED":"CONFIG_COMMIT_FAILED","配置未保存，原有配置已保留");
        bool rtc=dh::board_rtc_write(parsed);dh::network_reconfigure();
        auto *o=result(true),*r=cJSON_AddObjectToObject(o,"result");cJSON_AddBoolToObject(r,"saved",true);cJSON_AddNumberToObject(r,"config_revision",dh::secrets().revision);cJSON_AddBoolToObject(r,"rtc_written",rtc);auto fingerprint=dh::update_key_fingerprint(dh::secrets().update_public_key_pem);if(fingerprint.empty())cJSON_AddNullToObject(r,"update_key_fingerprint");else cJSON_AddStringToObject(r,"update_key_fingerprint",fingerprint.c_str());return o;
    }
    if(op=="set_time") {
        auto utc=field(cJSON_GetObjectItemCaseSensitive(request,"time"),"utc");time_t t;
        if(!dh::parse_utc(utc.c_str(),t)||!dh::set_time_utc(utc.c_str(),"manual"))return result(false,"INVALID_ARGUMENT","UTC时间无效");
        auto *o=result(true),*r=cJSON_AddObjectToObject(o,"result");cJSON_AddBoolToObject(r,"rtc_written",dh::board_rtc_write(t));cJSON_AddStringToObject(r,"time_quality","manual");return o;
    }
    return result(false,"UNSUPPORTED","未知USB操作");
}
void usb(void *) {
    std::string line;line.reserve(16384);bool overflow=false;uint8_t data[128];
    for(;;){int n=usb_serial_jtag_read_bytes(data,sizeof(data),pdMS_TO_TICKS(100));
        for(int i=0;i<n;++i){char ch=data[i];if(ch=='\n'){
            cJSON *o=nullptr;
            if(overflow)o=result(false,"PAYLOAD_TOO_LARGE","USB帧超过16KiB");
            else{auto *input=dh::parse_json_strict(line.c_str(),line.size());o=input?handle(input):result(false,"INVALID_JSON","无效JSON");cJSON_Delete(input);}
            char *output=cJSON_PrintUnformatted(o);cJSON_Delete(o);if(output){usb_serial_jtag_write_bytes(output,strlen(output),pdMS_TO_TICKS(1000));usb_serial_jtag_write_bytes("\n",1,pdMS_TO_TICKS(1000));free(output);}
            std::fill(line.begin(),line.end(),0);line.clear();overflow=false;
        }else if(ch!='\r'){if(line.size()<16383&&!overflow)line.push_back(ch);else overflow=true;}}
    }
}
void housekeeping(void *) {
    int64_t last_power=0;std::string time_quality;
    for(;;){auto now=dh::monotonic_ms();if(now-last_power>=2000){dh::board_sample_power();last_power=now;}
        dh::charge::tick();   // charge protection: one step with the newest sample; the only code that writes the charge switch
        auto s=dh::snapshot();if(s.time_valid&&s.time_quality!=time_quality){if(!dh::board_rtc_write(time(nullptr)))dh::mutate([](dh::Snapshot &v,void*){v.error="RTC_WRITE_FAILED：当前系统时间仍有效";});time_quality=s.time_quality;}
        // Power sampling continues during OTA. The charge switch is written only by charge protection, and restored first.
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
}
extern "C" void app_main() {
    // Never print frames, tokens, SSIDs, Authorization headers, or trust material.
    esp_log_level_set("esp-tls",ESP_LOG_NONE);esp_log_level_set("HTTP_CLIENT",ESP_LOG_NONE);
    dh::core_init();dh::board_init();dh::charge::init();
    usb_serial_jtag_driver_config_t usb_cfg{};usb_cfg.rx_buffer_size=2048;usb_cfg.tx_buffer_size=2048;
    if(usb_serial_jtag_driver_install(&usb_cfg)==ESP_OK)xTaskCreate(usb,"dh_usb",8192,nullptr,4,nullptr);
    if(dh::board_display_init()==ESP_OK)dh::ui_start();
    dh::network_start();dh::agent_start();dh::management_start();xTaskCreate(housekeeping,"dh_power",6144,nullptr,5,nullptr);
}
