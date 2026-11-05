#include "wifi_change_policy.hpp"
#include "core.hpp"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <cstring>
namespace dh::wifi_change {
namespace {
void wipe(std::string &s){volatile char *p=s.empty()?nullptr:&s[0];for(size_t i=0;i<s.size();++i)p[i]=0;s.clear();}
std::atomic<bool> scan_finished{false};
std::atomic<bool> scan_succeeded{false};
void scan_event(void*,esp_event_base_t,int32_t,void *data){auto *e=static_cast<wifi_event_sta_scan_done_t*>(data);scan_succeeded.store(e&&e->status==0);scan_finished.store(true);}
struct Device : Port {
    bool started=false;
    std::string expected;
    bool acquire() override {
        if(!dh::network_acquire(dh::NetworkOwner::WifiChange))return false;
        if(!dh::network_wifi_started()){
            if(esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK||esp_wifi_start()!=ESP_OK){dh::network_release(dh::NetworkOwner::WifiChange);return false;}
            started=true;
        }
        return true;
    }
    void release() override {
        auto stored=dh::secrets();
        const bool applied=has_ip()&&expected==stored.ssid;
        const bool no_previous_wifi=stored.ssid.empty();
        wipe(stored.password);wipe(stored.token);
        if(!applied&&no_previous_wifi){
            esp_wifi_disconnect();wifi_config_t empty{};esp_wifi_set_config(WIFI_IF_STA,&empty);
        }
        if(started&&!applied)esp_wifi_stop();
        started=false;expected.clear();dh::network_release(dh::NetworkOwner::WifiChange,applied);
    }
    bool scan_start() override {
        // An in-progress normal reconnect makes ESP-IDF reject a scan. Stop it
        // after acquiring ownership; an established connection stays up.
        if(!dh::snapshot().wifi_ready){esp_wifi_disconnect();vTaskDelay(pdMS_TO_TICKS(200));}
        scan_finished.store(false);scan_succeeded.store(false);wifi_scan_config_t cfg{};cfg.show_hidden=false;
        return esp_wifi_scan_start(&cfg,false)==ESP_OK;
    }
    bool scan_done(std::vector<Network> &out) override {
        if(!scan_finished.load()||!scan_succeeded.load())return false;
        uint16_t count=32;std::vector<wifi_ap_record_t> records(count);
        if(esp_wifi_scan_get_ap_records(&count,records.data())!=ESP_OK)return false;
        esp_wifi_clear_ap_list();
        auto current=dh::secrets();
        for(size_t i=0;i<count;++i){const auto &ap=records[i];Network n;
            n.ssid.assign(reinterpret_cast<const char*>(ap.ssid),strnlen(reinterpret_cast<const char*>(ap.ssid),32));
            n.signal=ap.rssi;n.current=n.ssid==current.ssid;
            n.security=ap.authmode==WIFI_AUTH_OPEN?Security::Open:
                ap.authmode==WIFI_AUTH_WPA_PSK||ap.authmode==WIFI_AUTH_WPA2_PSK||ap.authmode==WIFI_AUTH_WPA_WPA2_PSK||ap.authmode==WIFI_AUTH_WPA3_PSK||ap.authmode==WIFI_AUTH_WPA2_WPA3_PSK?Security::Personal:Security::Unsupported;
            out.push_back(std::move(n));
        }
        wipe(current.password);wipe(current.token);return true;
    }
    void scan_stop() override {esp_wifi_scan_stop();esp_wifi_clear_ap_list();}
    bool join_start(const std::string &ssid,const std::string &password) override {
        expected=ssid;esp_wifi_disconnect();
        // Drain the old disconnect before resetting evidence for this attempt.
        vTaskDelay(pdMS_TO_TICKS(200));dh::network_trial_reset();
        wifi_config_t cfg{};memcpy(cfg.sta.ssid,ssid.data(),ssid.size());memcpy(cfg.sta.password,password.data(),password.size());
        cfg.sta.pmf_cfg.capable=true;
        const bool ok=esp_wifi_set_storage(WIFI_STORAGE_RAM)==ESP_OK&&esp_wifi_set_config(WIFI_IF_STA,&cfg)==ESP_OK&&esp_wifi_connect()==ESP_OK;
        volatile unsigned char *p=reinterpret_cast<volatile unsigned char*>(&cfg);for(size_t i=0;i<sizeof(cfg);++i)p[i]=0;
        return ok;
    }
    bool has_ip() override {
        if(!dh::snapshot().wifi_ready)return false;
        wifi_ap_record_t ap{};if(esp_wifi_sta_get_ap_info(&ap)!=ESP_OK)return false;
        return expected==std::string(reinterpret_cast<const char*>(ap.ssid),strnlen(reinterpret_cast<const char*>(ap.ssid),32));
    }
    Reason join_failure() override {
        auto reason=dh::network_disconnect_reason();
        if(reason==WIFI_REASON_AUTH_FAIL||reason==WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT||reason==WIFI_REASON_HANDSHAKE_TIMEOUT)return Reason::WrongPassword;
        if(reason==WIFI_REASON_NO_AP_FOUND)return Reason::NotFound;
        return Reason::None;
    }
    bool is_paired() override {auto s=dh::secrets();bool paired=s.ready();wipe(s.password);wipe(s.token);return paired;}
    bool agent_ready() override {return dh::snapshot().agent_ready;}
    bool save(const std::string &ssid,const std::string &password) override {return dh::save_wifi_credentials(ssid,password)==ESP_OK;}
    bool restore_start() override {auto old=dh::secrets();bool ok=!old.ssid.empty()&&join_start(old.ssid,old.password);wipe(old.password);wipe(old.token);return ok;}
};
enum class Request { None, Scan, Begin, Keep, Revert, Cancel };
SemaphoreHandle_t lock=nullptr;
TaskHandle_t task=nullptr;
Status shown;
std::vector<Network> list;
Request request=Request::None;
std::string pending_ssid,pending_password;
bool pending_open=false;
int64_t last_touch=0;
struct Guard {Guard(){xSemaphoreTake(lock,portMAX_DELAY);}~Guard(){xSemaphoreGive(lock);}};
void run(void*){
    Device device;Session session(device);
    for(;;){
        Request op;std::string ssid,password;bool open;int64_t touched;
        {Guard g;op=request;request=Request::None;ssid.swap(pending_ssid);password.swap(pending_password);open=pending_open;touched=last_touch;last_touch=0;}
        auto now=dh::monotonic_ms();if(touched)session.touch(touched);
        switch(op){
        case Request::Scan:session.scan(now);break;
        case Request::Begin:session.begin(ssid,password,open,now);break;
        case Request::Keep:session.keep(now);break;
        case Request::Revert:session.revert(now);break;
        case Request::Cancel:session.cancel(now);break;
        default:break;
        }
        wipe(ssid);wipe(password);session.tick(dh::monotonic_ms());
        {Guard g;shown=session.status(dh::monotonic_ms());list=session.networks();}
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
bool enqueue(Request op){if(!lock||!task)return false;Guard g;if(request!=Request::None)return false;request=op;return true;}
}
void init(){
    if(lock)return;
    lock=xSemaphoreCreateMutex();if(!lock)return;
    if(esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_SCAN_DONE,scan_event,nullptr)!=ESP_OK)return;
    if(xTaskCreate(run,"dh_wifi_change",8192,nullptr,3,&task)!=pdPASS)task=nullptr;
}
bool available(){if(!task)return false;auto s=dh::snapshot();return dh::network_owner()==dh::NetworkOwner::None&&s.update_state!="downloading"&&s.update_state!="writing"&&s.update_state!="verifying"&&s.update_state!="rebooting";}
bool start_scan(){if(!task)return false;Guard g;if(request!=Request::None||(shown.busy&&shown.phase!=Phase::Typing))return false;auto owner=dh::network_owner();if(owner!=dh::NetworkOwner::None&&owner!=dh::NetworkOwner::WifiChange){shown.reason=Reason::HotspotOpen;return false;}request=Request::Scan;shown.phase=Phase::Scanning;shown.busy=true;return true;}
std::vector<Network> networks(){if(!lock)return {};Guard g;return list;}
bool begin(const std::string &ssid,const std::string &password,bool open){if(!task||!valid_credentials(ssid,password,open))return false;Guard g;if(request!=Request::None||shown.phase!=Phase::Typing)return false;pending_ssid=ssid;pending_password=password;pending_open=open;request=Request::Begin;return true;}
bool keep(){if(status().phase!=Phase::Asking)return false;return enqueue(Request::Keep);}
bool revert(){return enqueue(Request::Revert);}
void cancel(){if(!task)return;Guard g;wipe(pending_ssid);wipe(pending_password);request=Request::Cancel;}
void touch(){if(!task)return;Guard g;last_touch=dh::monotonic_ms();}
Status status(){if(!lock)return {};Guard g;return shown;}
}
