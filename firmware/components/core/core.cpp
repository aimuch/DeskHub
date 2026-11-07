#include "core.hpp"
#include "json_util.hpp"
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <sys/time.h>

namespace dh {
const char *const VERSION=esp_app_get_description()->version;
static SemaphoreHandle_t lock;
static SemaphoreHandle_t storage_lock;
static Snapshot state;
static DisplayConfig display_cfg;
static Secrets credentials;
static uint64_t credential_generation=1;
static bool storage_ready = false;
static bool trust_read_fault = false;
static bool schema_unsupported = false;
static bool wifi_started = false;
static bool reconnect_requested = false;
static bool wifi_reconfigure = false;
static NetworkOwner radio_owner = NetworkOwner::None;
static SemaphoreHandle_t radio_lock;
static int disconnect_reason = 0;
static int64_t reconnect_at = 0;
static Backoff backoff;
static bool ntp_started = false;
static bool ntp_start_requested = false;
static std::string ntp_hostname = "pool.ntp.org";
struct Guard { Guard() { xSemaphoreTake(lock, portMAX_DELAY); } ~Guard() { xSemaphoreGive(lock); } };
// All persistent writers acquire storage before the Core state lock. Readers
// never take storage, so display commits cannot stall snapshot/config reads.
struct StorageGuard { StorageGuard() { xSemaphoreTake(storage_lock, portMAX_DELAY); } ~StorageGuard() { xSemaphoreGive(storage_lock); } };
static bool read_trusted_key(std::string &value) {
    value.clear();nvs_handle_t handle;
    auto e=nvs_open("dh_trust",NVS_READONLY,&handle);
    if(e==ESP_ERR_NVS_NOT_FOUND)return true;
    if(e!=ESP_OK)return false;
    size_t size=0;e=nvs_get_str(handle,"update_key",nullptr,&size);
    if(e==ESP_ERR_NVS_NOT_FOUND){nvs_close(handle);return true;}
    if(e!=ESP_OK||size<1||size>2049){nvs_close(handle);return false;}
    std::vector<char> key(size);e=nvs_get_str(handle,"update_key",key.data(),&size);nvs_close(handle);
    if(e!=ESP_OK||key.back()!=0||strlen(key.data())+1!=size)return false;
    value=key.data();return true;
}
int64_t monotonic_ms() { return esp_timer_get_time() / 1000; }
static std::string field(cJSON *o, const char *key) { auto *v = cJSON_GetObjectItemCaseSensitive(o, key); return cJSON_IsString(v) ? v->valuestring : ""; }
static int number(cJSON *o, const char *key, int fallback) { auto *v = cJSON_GetObjectItemCaseSensitive(o, key); return cJSON_IsNumber(v) ? v->valueint : fallback; }
static bool flag(cJSON *o, const char *key, bool fallback) { auto *v = cJSON_GetObjectItemCaseSensitive(o, key); return cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback; }
// Each NVS blob contains a complete JSON payload and CRC. A second slot retains
// the preceding commit; neither a torn write nor a bad schema destroys both.
static cJSON *load_slot(nvs_handle_t h, const char *key) {
    size_t len = 0;
    if (nvs_get_blob(h, key, nullptr, &len) != ESP_OK || len < 8 || len > 16384) return nullptr;
    std::vector<uint8_t> buf(len);
    if (nvs_get_blob(h, key, buf.data(), &len) != ESP_OK) return nullptr;
    uint32_t expected;
    memcpy(&expected, buf.data(), 4);
    if (expected != crc32(buf.data() + 4, len - 4) || buf.back() != 0) return nullptr;
    return parse_json_strict(reinterpret_cast<char *>(buf.data() + 4),len-5);
}
static esp_err_t store_blob(const char *ns, uint32_t revision, cJSON *json) {
    if (!storage_ready) return ESP_ERR_INVALID_STATE;
    if(schema_unsupported)return ESP_ERR_NOT_SUPPORTED;
    char *body = cJSON_PrintUnformatted(json);
    if (!body) return ESP_ERR_NO_MEM;
    size_t len = strlen(body) + 1;
    std::vector<uint8_t> buf(len + 4);
    uint32_t crc = crc32(body, len);
    memcpy(buf.data(), &crc, 4); memcpy(buf.data() + 4, body, len); free(body);
    nvs_handle_t h;
    auto e = nvs_open(ns, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, revision % 2 ? "slot_a" : "slot_b", buf.data(), buf.size());
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); return e;
}
static void apply_time_zone(int minutes) { setenv("TZ",tz_env(minutes).c_str(),1);tzset(); }
static bool decode_config(cJSON *o, DisplayConfig &c) {
    if (!cJSON_IsObject(o)) return false;
    int brightness = number(o, "brightness", -1), night_cap = number(o, "night_cap", -1);
    if (brightness < 0 || brightness > 100 || night_cap < 0 || night_cap > 100) return false;
    c.schema = number(o, "schema", 0); c.revision = number(o, "revision", 0);
    c.brightness = brightness; c.night_cap = night_cap;
    auto *rotation=cJSON_GetObjectItemCaseSensitive(o,"rotation");
    if(rotation){if(!cJSON_IsNumber(rotation)||rotation->valuedouble!=rotation->valueint||rotation->valueint<0||rotation->valueint>270||rotation->valueint%90)return false;c.rotation=rotation->valueint;}
    c.clock_s = number(o, "clock_s", 0); c.dim_s = number(o, "dim_s", 0); c.off_s = number(o, "off_s", 0);
    c.night_start = number(o, "night_start", 1440); c.night_end = number(o, "night_end", 1440);
    c.night_enabled = flag(o, "night_enabled", true); c.protection = flag(o, "protection", true);
    const int tz = number(o, "tz_minutes", kDefaultTzMinutes); c.tz_minutes = valid_tz(tz) ? tz : kDefaultTzMinutes;   // older configurations have none
    auto *layout=cJSON_GetObjectItemCaseSensitive(o,"layout");
    if(layout){
        if(!cJSON_IsObject(layout))return false;
        c.layout.preset=field(layout,"preset");
        auto *theme=cJSON_GetObjectItemCaseSensitive(layout,"theme");
        if(theme){if(!cJSON_IsString(theme))return false;c.layout.theme=theme->valuestring;}
        c.layout.show_seconds=flag(layout,"show_seconds",false);c.layout.show_cpu=flag(layout,"show_cpu",false);
        auto *ids=cJSON_GetObjectItemCaseSensitive(layout,"action_ids");if(!cJSON_IsArray(ids)||cJSON_GetArraySize(ids)>4)return false;
        c.layout.action_count=0;cJSON *id;cJSON_ArrayForEach(id,ids){if(!cJSON_IsString(id))return false;c.layout.action_ids[c.layout.action_count++]=id->valuestring;}
    }
    c.managed_revision=number(o,"managed_revision",0);c.managed_agent_id=field(o,"managed_agent_id");
    return c.revision > 0 && valid_config(c);
}
void core_init() {
    lock = xSemaphoreCreateMutex();
    storage_lock = xSemaphoreCreateMutex();
    radio_lock = xSemaphoreCreateMutex();
    config_jobs_init();
    apply_time_zone(kDefaultTzMinutes);
    uint8_t mac[6]{}; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[32]; snprintf(id, sizeof(id), "deskhub-%02x%02x%02x%02x%02x%02x", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    state.device_id = id;
    // Do not erase NVS on errors: preserved credentials can still be recovered.
    storage_ready = nvs_flash_init() == ESP_OK;
    if (!storage_ready) { state.error = "CONFIG_STORAGE_FAULT：请通过USB恢复"; return; }
    nvs_handle_t h;
    if (nvs_open("dh_config", NVS_READONLY, &h) == ESP_OK) {
        for (auto key : {"slot_a", "slot_b"}) {
            cJSON *o = load_slot(h, key); DisplayConfig c;
            if(cJSON_IsObject(o)&&number(o,"schema",0)!=1)schema_unsupported=true;
            if (decode_config(o, c) && c.revision > display_cfg.revision) { display_cfg = c; apply_time_zone(c.tz_minutes); }
            cJSON_Delete(o);
        }
        nvs_close(h);
    }
    trust_read_fault=!read_trusted_key(credentials.update_public_key_pem);
    bool unmatched_pairing=false;
    if (nvs_open("dh_secrets", NVS_READONLY, &h) == ESP_OK) {
        for (auto key : {"slot_a", "slot_b"}) {
            auto *o = load_slot(h, key);
            if(cJSON_IsObject(o)&&number(o,"schema",0)!=1)schema_unsupported=true;
            if (o && number(o, "schema", 0) == 1 && static_cast<uint32_t>(number(o, "revision", 0)) > credentials.revision) {
                Secrets s;
                s.ssid = field(o,"ssid"); s.password = field(o,"password"); s.url = field(o,"url");
                s.agent_id = field(o,"agent_id"); s.token = field(o,"token"); s.ca_pem = field(o,"ca_pem");s.update_public_key_pem=field(o,"expected_update_key_pem");
                auto ntp=field(o,"ntp_server");if(!ntp.empty())s.ntp_server=ntp;
                s.revision = number(o,"revision",0);
                if(s.update_public_key_pem!=credentials.update_public_key_pem)unmatched_pairing=true;
                else if (!s.ssid.empty() && s.ssid.size()<=32 && s.password.size()<=64 && (s.url.empty() || s.url.rfind("https://",0) == 0) && valid_ntp_server(s.ntp_server)) credentials = s;
            }
            cJSON_Delete(o);
        }
        nvs_close(h);
    }
    if(unmatched_pairing&&!credentials.ready())state.error="PAIRING_COMMIT_INCOMPLETE：请通过USB恢复";
    if(trust_read_fault){credentials.token.clear();state.error="UPDATE_TRUST_STORAGE_FAULT：请通过USB恢复";}
    if(schema_unsupported)state.error="CONFIG_SCHEMA_UNSUPPORTED：保留原配置，请升级固件或明确恢复出厂";
}
bool core_storage_healthy(){Guard g;return storage_ready&&!schema_unsupported&&!trust_read_fault;}
Snapshot snapshot() { Guard g; return state; }
void mutate(void (*fn)(Snapshot &, void *), void *ctx) { Guard g; fn(state,ctx); }
bool mutate_if_generation(uint64_t generation,void (*fn)(Snapshot &,void *),void *ctx){Guard g;if(generation!=credential_generation)return false;fn(state,ctx);return true;}
uint64_t config_generation(){Guard g;return credential_generation;}
esp_err_t with_generation(uint64_t expected,esp_err_t (*fn)(void *),void *ctx){Guard g;if(expected!=credential_generation)return ESP_ERR_INVALID_STATE;return fn(ctx);}
Secrets secrets() { Guard g; auto s=credentials;s.generation=credential_generation;return s; }
DisplayConfig config() { Guard g; return display_cfg; }
ConfigDraft config_draft() { Guard g; return {display_cfg, credential_generation}; }
static esp_err_t persist_config(const DisplayConfig &c) {
    auto *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o,"schema",c.schema); cJSON_AddNumberToObject(o,"revision",c.revision);
    cJSON_AddNumberToObject(o,"brightness",c.brightness); cJSON_AddNumberToObject(o,"clock_s",c.clock_s);
    cJSON_AddNumberToObject(o,"rotation",c.rotation);
    cJSON_AddNumberToObject(o,"dim_s",c.dim_s); cJSON_AddNumberToObject(o,"off_s",c.off_s);
    cJSON_AddNumberToObject(o,"night_start",c.night_start); cJSON_AddNumberToObject(o,"night_end",c.night_end);
    cJSON_AddNumberToObject(o,"night_cap",c.night_cap); cJSON_AddBoolToObject(o,"night_enabled",c.night_enabled);
    cJSON_AddBoolToObject(o,"protection",c.protection);
    cJSON_AddNumberToObject(o,"tz_minutes",c.tz_minutes);
    auto *layout=cJSON_AddObjectToObject(o,"layout");cJSON_AddStringToObject(layout,"preset",c.layout.preset.c_str());cJSON_AddBoolToObject(layout,"show_seconds",c.layout.show_seconds);cJSON_AddBoolToObject(layout,"show_cpu",c.layout.show_cpu);
    cJSON_AddStringToObject(layout,"theme",c.layout.theme.c_str());
    auto *ids=cJSON_AddArrayToObject(layout,"action_ids");for(size_t i=0;i<c.layout.action_count;++i)cJSON_AddItemToArray(ids,cJSON_CreateString(c.layout.action_ids[i].c_str()));
    cJSON_AddNumberToObject(o,"managed_revision",c.managed_revision);cJSON_AddStringToObject(o,"managed_agent_id",c.managed_agent_id.c_str());
    auto e = store_blob("dh_config",c.revision,o); cJSON_Delete(o);
    return e;
}
esp_err_t save_config(DisplayConfig c) {
    return save_config(ConfigDraft{std::move(c), config_generation()});
}
esp_err_t save_config(const ConfigDraft &draft) {
    auto c = draft.value;
    if(!valid_config(c))return ESP_ERR_INVALID_ARG;
    StorageGuard storage;
    {
        Guard g;
        if(radio_owner!=NetworkOwner::None||draft.generation!=credential_generation||c.revision!=display_cfg.revision||c.revision==UINT32_MAX)return ESP_ERR_INVALID_STATE;
        ++c.revision;
    }
    const auto e = persist_config(c);
    if(e==ESP_OK){
        // Identity/reset/managed writers are serialized by storage_lock. A Wi-Fi
        // disconnect may advance generation meanwhile, but cannot undo a durable
        // local commit or justify leaving RAM behind its persisted revision.
        Guard g;apply_time_zone(c.tz_minutes);display_cfg=std::move(c);
    }
    return e;
}
esp_err_t save_managed_config(DisplayConfig c,uint64_t generation) {
    if(!valid_config(c))return ESP_ERR_INVALID_ARG;
    StorageGuard storage;
    Guard g;
    if(radio_owner!=NetworkOwner::None||generation!=credential_generation||c.managed_agent_id!=credentials.agent_id||c.revision!=display_cfg.revision||c.revision==UINT32_MAX)return ESP_ERR_INVALID_STATE;
    ++c.revision;
    const auto e=persist_config(c);
    if(e==ESP_OK)display_cfg=std::move(c);
    return e;
}
static esp_err_t persist_credentials(const Secrets &s) {
    auto *o = cJSON_CreateObject(); cJSON_AddNumberToObject(o,"schema",1); cJSON_AddNumberToObject(o,"revision",s.revision);
    cJSON_AddStringToObject(o,"ssid",s.ssid.c_str()); cJSON_AddStringToObject(o,"password",s.password.c_str());
    cJSON_AddStringToObject(o,"url",s.url.c_str()); cJSON_AddStringToObject(o,"agent_id",s.agent_id.c_str());
    cJSON_AddStringToObject(o,"token",s.token.c_str()); cJSON_AddStringToObject(o,"ca_pem",s.ca_pem.c_str());
    cJSON_AddStringToObject(o,"ntp_server",s.ntp_server.c_str());
    cJSON_AddStringToObject(o,"expected_update_key_pem",s.update_public_key_pem.c_str());
    auto e=store_blob("dh_secrets",s.revision,o);cJSON_Delete(o);return e;
}
esp_err_t save_wifi_credentials(const std::string &ssid,const std::string &password) {
    if(ssid.empty()||ssid.size()>32||password.size()>64)return ESP_ERR_INVALID_ARG;
    StorageGuard storage;Guard g;
    if(radio_owner!=NetworkOwner::WifiChange||!state.wifi_ready||credentials.revision==UINT32_MAX)return ESP_ERR_INVALID_STATE;
    Secrets next=credentials;next.ssid=ssid;next.password=password;++next.revision;
    auto e=persist_credentials(next);
    if(e==ESP_OK){credentials=next;++credential_generation;state.agent_ready=false;state.action_count=0;}
    // A failed commit leaves both RAM credentials and the preceding NVS slot intact.
    volatile char *p=next.password.empty()?nullptr:&next.password[0];for(size_t i=0;i<next.password.size();++i)p[i]=0;
    return e;
}
esp_err_t save_secrets(const Secrets &input, NetworkOwner writer) {
    if (!input.ready() || input.ssid.size() > 32 || input.password.size() > 63 || input.token.size() < 32 || input.token.size() > 256 || input.agent_id.size() > 96 ||
        input.update_public_key_pem.size()>2048 || input.update_public_key_pem.find("PRIVATE KEY")!=std::string::npos || input.url.size() > 256 || input.ca_pem.size() > 8192 || input.agent_id.empty() || input.url.rfind("https://",0) != 0 ||
        input.url.find('@') != std::string::npos || input.url.find('#') != std::string::npos || input.url.find('?') != std::string::npos || !valid_ntp_server(input.ntp_server))
        return ESP_ERR_INVALID_ARG;
    StorageGuard storage;
    Guard g;
    if(radio_owner!=writer)return ESP_ERR_INVALID_STATE;
    // Any USB re-provision attempt invalidates network work, including a failed
    // multi-record commit whose final persistent result is not yet known.
    ++credential_generation;state.agent_ready=false;state.action_count=0;
    std::string persisted_key;
    trust_read_fault=!read_trusted_key(persisted_key);
    if(trust_read_fault){credentials.token.clear();state.error="UPDATE_TRUST_STORAGE_FAULT：请通过USB恢复";return ESP_FAIL;}
    credentials.update_public_key_pem=persisted_key;
    Secrets s = input; s.revision = credentials.revision + 1;
    if(!input.update_key_explicit)s.update_public_key_pem=persisted_key;
    auto e = persist_credentials(s);
    if(e==ESP_OK&&s.update_public_key_pem!=credentials.update_public_key_pem){
        nvs_handle_t trust;e=nvs_open("dh_trust",NVS_READWRITE,&trust);
        if(e==ESP_OK){e=nvs_set_str(trust,"update_key",s.update_public_key_pem.c_str());if(e==ESP_OK)e=nvs_commit(trust);nvs_close(trust);}
    }
    if (e == ESP_OK) { credentials = s;state.agent_ready = false; state.action_count = 0; }
    else {credentials.token.clear();trust_read_fault=!read_trusted_key(credentials.update_public_key_pem);state.error="PAIRING_COMMIT_FAILED：请重新通过USB配置";}
    return e;
}
esp_err_t save_update_key(const std::string &pem) {
    if (pem.empty() || pem.size() > 2048 || pem.find("PRIVATE KEY") != std::string::npos) return ESP_ERR_INVALID_ARG;
    StorageGuard storage;
    nvs_handle_t trust;
    esp_err_t e = nvs_open("dh_trust", NVS_READWRITE, &trust);
    if (e == ESP_OK) { e = nvs_set_str(trust, "update_key", pem.c_str()); if (e == ESP_OK) e = nvs_commit(trust); nvs_close(trust); }
    if (e == ESP_OK) { Guard g; credentials.update_public_key_pem = pem; trust_read_fault = false; }
    return e;
}
static esp_err_t reset_config_locked() {
    if(radio_owner!=NetworkOwner::None)return ESP_ERR_INVALID_STATE;
    if (!storage_ready) return ESP_ERR_INVALID_STATE;
    for (auto ns : {"dh_config", "dh_secrets"}) {
        nvs_handle_t h; auto e = nvs_open(ns,NVS_READWRITE,&h); if (e != ESP_OK) return e;
        e = nvs_erase_all(h); if (e == ESP_OK) e = nvs_commit(h); nvs_close(h); if (e != ESP_OK) return e;
    }
    auto trusted_key=credentials.update_public_key_pem;credentials = {};credentials.update_public_key_pem=trusted_key; ++credential_generation;display_cfg = {};apply_time_zone(kDefaultTzMinutes); state.agent_ready = false; state.action_count = 0;schema_unsupported=false;
    wifi_reconfigure = true; return ESP_OK;
}
esp_err_t reset_config() {
    StorageGuard storage;
    Guard g;
    return reset_config_locked();
}
esp_err_t reset_config(const ConfigDraft &draft) {
    StorageGuard storage;
    Guard g;
    if(draft.generation!=credential_generation||draft.value.revision!=display_cfg.revision)return ESP_ERR_INVALID_STATE;
    return reset_config_locked();
}
bool parse_utc(const char *s, time_t &out) {
    if (!s) return false;
    std::string canonical=s;
    if (canonical.size()>20 && canonical.size()<=30 && canonical[19]=='.' && canonical.back()=='Z') {
        for(size_t i=20;i+1<canonical.size();++i)if(canonical[i]<'0'||canonical[i]>'9')return false;
        if(canonical.size()==21)return false;
        canonical=canonical.substr(0,19)+"Z";
    }
    if(canonical.size()!=20)return false;
    s=canonical.c_str();
    struct tm t{}; char *end = strptime(s,"%Y-%m-%dT%H:%M:%SZ",&t);
    if (!end || *end || t.tm_year < 124 || t.tm_year > 199 || t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31 || t.tm_sec > 59) return false;
    out = timegm(&t); struct tm round{}; gmtime_r(&out,&round); char buf[32]; strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%SZ",&round);
    return strcmp(buf,s) == 0;
}
bool set_time_utc(const char *s, const char *quality) {
    time_t t; if (!parse_utc(s,t)) return false;
    struct timeval tv{t,0}; if (settimeofday(&tv,nullptr)) return false;
    Guard g; state.time_valid = true; state.time_quality = quality; return true;
}
std::string utc_now() { time_t t = time(nullptr); struct tm v{}; gmtime_r(&t,&v); char b[32]; strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&v); return b; }
static void ntp_sync(struct timeval *) {
    Guard g; state.time_valid = time(nullptr) >= 1704067200; state.time_quality = state.time_valid ? "ntp" : "unsynced";
}
static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
    Guard g;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data); char ip[20]; snprintf(ip,sizeof(ip),IPSTR,IP2STR(&ev->ip_info.ip));
        state.ip = ip; state.wifi = "已连接"; state.wifi_ready = true; reconnect_requested = false; backoff.reset();
        ntp_start_requested=true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ++credential_generation; // Connection loss also invalidates snapshots and queued clicks.
        auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
        state.wifi = ev->reason == WIFI_REASON_AUTH_FAIL || ev->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ? "Wi-Fi密码错误" :
            ev->reason == WIFI_REASON_NO_AP_FOUND ? "未找到Wi-Fi" : "Wi-Fi已断开";
        disconnect_reason=ev->reason;
        state.ip.clear();state.wifi_ready = false; state.agent_ready = false; state.action_count = 0;
        reconnect_at = monotonic_ms() + backoff.next_ms(esp_random()); reconnect_requested = true;
    }
}
static void wifi_task(void *) {
    while (true) {
        xSemaphoreTake(radio_lock,portMAX_DELAY);
        { bool held; { Guard g; held = radio_owner!=NetworkOwner::None; } if (held) { xSemaphoreGive(radio_lock);vTaskDelay(pdMS_TO_TICKS(50)); continue; } }
        bool reconfigure,start_ntp; { Guard g; reconfigure = wifi_reconfigure; wifi_reconfigure = false;start_ntp=ntp_start_requested;ntp_start_requested=false; }
        if (reconfigure) {
            auto s = secrets();
            if(ntp_started){esp_sntp_stop();ntp_started=false;}
            start_ntp=false;
            if (wifi_started) esp_wifi_disconnect();
            if (!s.ssid.empty()) {
                wifi_config_t cfg{}; memcpy(cfg.sta.ssid,s.ssid.data(),std::min<size_t>(s.ssid.size(),32)); memcpy(cfg.sta.password,s.password.data(),std::min<size_t>(s.password.size(),64));
                cfg.sta.pmf_cfg.capable = true; cfg.sta.pmf_cfg.required = false;
                esp_wifi_set_config(WIFI_IF_STA,&cfg); if (!wifi_started) { esp_wifi_start(); wifi_started = true; }
                esp_wifi_connect(); Guard g; state.wifi = "正在连接Wi-Fi";
            } else { if (wifi_started) esp_wifi_stop(); wifi_started = false; Guard g; state.wifi_ready = false; state.wifi = "未配置"; }
        }
        // SNTP setters synchronously call the TCP/IP task. Never hold core's
        // mutex here: its synchronization callback takes that same mutex.
        if(start_ntp&&!ntp_started){ntp_hostname=secrets().ntp_server;esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,ntp_hostname.data());esp_sntp_set_time_sync_notification_cb(ntp_sync);esp_sntp_init();ntp_started=true;}
        bool retry; { Guard g; retry = reconnect_requested && monotonic_ms() >= reconnect_at && !credentials.ssid.empty(); if (retry) { reconnect_requested = false; reconnect_at = monotonic_ms() + 15000; } }
        if (retry) { esp_wifi_connect(); Guard g; reconnect_requested = true; }
        if (snapshot().wifi_ready) { wifi_ap_record_t ap{}; if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { Guard g; state.rssi = ap.rssi; } }
        xSemaphoreGive(radio_lock);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
void network_start() {
    esp_netif_init(); esp_event_loop_create_default(); esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) { Guard g; state.wifi = "Wi-Fi初始化失败"; return; }
    esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,nullptr); esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,nullptr);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); esp_wifi_set_mode(WIFI_MODE_STA);
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); if (netif) esp_netif_set_hostname(netif,state.device_id.c_str());
    wifi_reconfigure = true; xTaskCreate(wifi_task,"dh_wifi",4096,nullptr,4,nullptr);
}
void network_reconfigure() { Guard g; wifi_reconfigure = true; }
bool network_acquire(NetworkOwner owner) {
    if(owner==NetworkOwner::None||!radio_lock)return false;
    xSemaphoreTake(radio_lock,portMAX_DELAY);
    bool ok=false;
    { StorageGuard storage;Guard g;
      if(radio_owner==NetworkOwner::None&&state.update_state!="downloading"&&state.update_state!="writing"&&state.update_state!="verifying"&&state.update_state!="rebooting") {radio_owner=owner;ok=true;}
    }
    xSemaphoreGive(radio_lock);return ok;
}
void network_release(NetworkOwner owner,bool connection_applied) {
    Guard g;if(radio_owner!=owner)return;
    if(connection_applied)wifi_started=true;
    wifi_reconfigure=!(connection_applied&&state.wifi_ready);
    radio_owner=NetworkOwner::None;
}
NetworkOwner network_owner(){Guard g;return radio_owner;}
bool network_begin_update(uint64_t generation){Guard g;if(radio_owner!=NetworkOwner::None||generation!=credential_generation)return false;state.update_state="verifying";return true;}
void network_trial_reset(){Guard g;++credential_generation;state.wifi_ready=false;state.agent_ready=false;state.action_count=0;state.ip.clear();disconnect_reason=0;}
int network_disconnect_reason(){Guard g;return disconnect_reason;}
void network_hold(bool hold) {if(hold)network_acquire(NetworkOwner::Pairing);else network_release(NetworkOwner::Pairing);}
bool network_wifi_started() { Guard g; return wifi_started; }
}
