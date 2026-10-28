#include "agent_client.hpp"
#include "json_util.hpp"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>

namespace dh {
struct Pending { char id[97], target[97], input[97]; int64_t deadline_ms; uint64_t generation; };
static QueueHandle_t actions;
static std::atomic<bool> busy{false};
struct Response { std::string body; int status = 0; bool overflow = false; };
static std::string str(cJSON *o,const char *key) { auto *v = cJSON_GetObjectItemCaseSensitive(o,key); return cJSON_IsString(v) ? v->valuestring : ""; }
static esp_err_t event(esp_http_client_event_t *ev) {
    auto *r = static_cast<Response *>(ev->user_data);
    if (ev->event_id == HTTP_EVENT_ON_DATA) {
        if (r->body.size() + ev->data_len > 16384) { r->overflow = true; return ESP_FAIL; }
        r->body.append(static_cast<char *>(ev->data),ev->data_len);
    }
    return ESP_OK;
}
static cJSON *request(const Secrets &s,const std::string &path,const char *body=nullptr,int timeout=5000) {
    if(config_generation()!=s.generation)return nullptr;
    Response r; auto url = s.url + path;
    esp_http_client_config_t cfg{}; cfg.url = url.c_str(); cfg.cert_pem = s.ca_pem.c_str(); cfg.timeout_ms = timeout;
    cfg.event_handler = event; cfg.user_data = &r; cfg.disable_auto_redirect = true; cfg.buffer_size = 1024;
    auto h = esp_http_client_init(&cfg); if (!h) return nullptr;
    auto auth = std::string("Bearer ") + s.token;
    esp_http_client_set_header(h,"Authorization",auth.c_str()); esp_http_client_set_header(h,"Accept","application/json");
    if (body) { esp_http_client_set_method(h,HTTP_METHOD_POST); esp_http_client_set_header(h,"Content-Type","application/json"); esp_http_client_set_post_field(h,body,strlen(body)); }
    auto e = esp_http_client_perform(h); r.status = esp_http_client_get_status_code(h);
    int tls_code=0,tls_flags=0;auto tls_error=esp_http_client_get_and_clear_last_tls_error(h,&tls_code,&tls_flags);esp_http_client_cleanup(h);
    if(config_generation()!=s.generation)return nullptr;
    if (r.overflow || e != ESP_OK) {
        std::string code=r.overflow?"SNAPSHOT_TOO_LARGE":tls_error==ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME?"DNS_RESOLUTION_FAILED":
            tls_flags?"TLS_CERTIFICATE_INVALID":tls_code?"TLS_CONNECTION_FAILED":e==ESP_ERR_TIMEOUT||tls_error==ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT?"AGENT_TIMEOUT":"AGENT_UNREACHABLE";
        mutate_if_generation(s.generation,[](Snapshot &st,void *p){st.error=*static_cast<std::string *>(p);},&code);return nullptr;
    }
    auto *json = parse_json_strict(r.body.c_str(),r.body.size());
    if(!cJSON_IsObject(json)){cJSON_Delete(json);mutate_if_generation(s.generation,[](Snapshot &st,void *){st.error="INVALID_AGENT_JSON";});return nullptr;}
    if (r.status < 200 || r.status >= 300) {
        std::string error = str(cJSON_GetObjectItemCaseSensitive(json,"error"),"code");
        if (error.empty()) error = r.status == 401 ? "UNAUTHORIZED" : r.status == 403 ? "FORBIDDEN" : "AGENT_REQUEST_FAILED";
        mutate_if_generation(s.generation,[](Snapshot &st,void *p){ st.error = *static_cast<std::string *>(p); },&error); cJSON_Delete(json); return nullptr;
    }
    return json;
}
static std::string uuid() {
    uint8_t b[16]; esp_fill_random(b,sizeof(b)); b[6] = (b[6]&15)|0x40; b[8]=(b[8]&63)|0x80;
    char s[40]; snprintf(s,sizeof(s),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]); return s;
}
static std::string command_text(const std::string &s) {
    if (s=="queued") return "已接收，等待执行";
    if (s=="executing") return "正在切换";
    if (s=="succeeded") return "已切换（已确认）";
    if (s=="unconfirmed") return "命令已发送，状态未确认";
    if (s=="expired") return "操作已过期，请重新点击";
    if (s=="failed") return "切换失败，请查看原因";
    if (s=="cancelled") return "操作已取消";
    return "状态未知";
}
static void execute(const Pending &p, const Secrets &s) {
    if(p.generation!=s.generation || p.generation!=config_generation())return;
    auto st = snapshot();
    if (!st.agent_ready || !st.wifi_ready || !st.time_valid || st.update_state=="downloading"||st.update_state=="writing"||st.update_state=="verifying"||st.update_state=="rebooting"||monotonic_ms()>=p.deadline_ms) {
        mutate_if_generation(s.generation,[](Snapshot &v,void*){v.command="当前连接不可用，请重新点击";}); return;
    }
    int64_t remaining=(p.deadline_ms-monotonic_ms())/1000;
    if(remaining<1){mutate_if_generation(s.generation,[](Snapshot &v,void*){v.command="操作已过期，请重新点击";});return;}
    auto id = uuid(), key = uuid(); auto now = utc_now(); time_t expires = time(nullptr)+remaining; struct tm t{}; gmtime_r(&expires,&t); char expiry[32]; strftime(expiry,sizeof(expiry),"%Y-%m-%dT%H:%M:%SZ",&t);
    auto *o = cJSON_CreateObject(); cJSON_AddStringToObject(o,"request_id",id.c_str()); cJSON_AddStringToObject(o,"idempotency_key",key.c_str());
    cJSON_AddStringToObject(o,"action_id",p.id); cJSON_AddStringToObject(o,"target_id",p.target); cJSON_AddStringToObject(o,"sent_at",now.c_str()); cJSON_AddStringToObject(o,"expires_at",expiry);
    auto *params = cJSON_AddObjectToObject(o,"parameters"); cJSON_AddStringToObject(params,"input_id",p.input);
    char *body = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    mutate_if_generation(s.generation,[](Snapshot &v,void*){v.command="正在连接";v.command_id.clear();});
    auto *response = request(s,"/api/v1/commands",body,2000); free(body);
    std::string command_id = str(response,"command_id"); cJSON_Delete(response);
    // Lost response: query the original key; never blindly mint another write.
    if (command_id.empty()) {
        response = request(s,"/api/v1/commands?idempotency_key="+key);
        command_id = str(response,"command_id"); cJSON_Delete(response);
    }
    if (command_id.empty()) { mutate_if_generation(s.generation,[](Snapshot &v,void*){v.command="接收结果未知，请通过Agent查询";}); return; }
    mutate_if_generation(s.generation,[](Snapshot &v,void *p){v.command_id=*static_cast<std::string *>(p);v.command="已接收，等待执行";},&command_id);
    int64_t deadline = monotonic_ms()+12000;
    while (monotonic_ms() < deadline) {
        if(config_generation()!=s.generation)return;
        response = request(s,"/api/v1/commands/"+command_id);
        auto result = str(response,"state"); auto error = str(cJSON_GetObjectItemCaseSensitive(response,"error"),"code"); cJSON_Delete(response);
        if (!result.empty()) {
        auto label = command_text(result); mutate_if_generation(s.generation,[](Snapshot &v,void *p){v.command=*static_cast<std::string *>(p);},&label);
            if (!error.empty()) mutate_if_generation(s.generation,[](Snapshot &v,void *p){v.error=*static_cast<std::string *>(p);},&error);
            if (result!="queued" && result!="executing") return;
        } else mutate_if_generation(s.generation,[](Snapshot &v,void*){v.command="连接中断，正在查询结果";});
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    mutate_if_generation(s.generation,[](Snapshot &v,void*){v.command="状态未确认，可通过Agent查询结果";});
}
static void parse_metrics(cJSON *json,uint64_t generation) {
    auto *hosts = cJSON_GetObjectItemCaseSensitive(json,"hosts"); if (!cJSON_IsArray(hosts)) return;
    auto *host = cJSON_GetArrayItem(hosts,0); if (!host) return;
    auto *metrics = cJSON_GetObjectItemCaseSensitive(host,"metrics"); if (!cJSON_IsArray(metrics)) return;
    struct Metrics { std::string cpu="不可用",memory="不可用",network="不可用",send="不可用"; bool valid=false;int64_t remaining=INT64_MAX; } m;
    cJSON *v;
    cJSON_ArrayForEach(v,metrics) {
        auto key=str(v,"key"); auto quality=str(v,"quality"); auto *value=cJSON_GetObjectItemCaseSensitive(v,"value");
        auto *ttl=cJSON_GetObjectItemCaseSensitive(v,"ttl_ms");time_t observed;
        auto date=str(v,"observed_at");
        if (quality!="valid" || !cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || !cJSON_IsNumber(ttl) || ttl->valueint<=0 ||
            !parse_utc(date.c_str(),observed) || difftime(time(nullptr),observed)*1000>ttl->valuedouble || difftime(observed,time(nullptr))>5) continue;
        char n[64];
        if (key=="cpu.utilization" || key=="memory.utilization") { snprintf(n,sizeof(n),"%.1f%%",value->valuedouble); if(key=="cpu.utilization")m.cpu=n;else m.memory=n; m.valid=true; }
        else if (key=="network.receive_rate" || key=="network.rx_rate" || key=="network.rx") { snprintf(n,sizeof(n),"%.2f MiB/s",value->valuedouble/1048576);m.network=n; m.valid=true; }
        else if (key=="network.send_rate") { snprintf(n,sizeof(n),"%.2f MiB/s",value->valuedouble/1048576);m.send=n; m.valid=true; }
        else continue;
        // A truncated fractional second is treated conservatively as up to a
        // full extra second old, never as a fresh TTL beginning at receipt.
        int64_t age=std::max<int64_t>(0,static_cast<int64_t>(difftime(time(nullptr),observed)*1000)+1000);
        m.remaining=std::min<int64_t>(m.remaining,std::max<int64_t>(0,ttl->valueint-age));
    }
    mutate_if_generation(generation,[](Snapshot &v,void *p){auto &m=*static_cast<Metrics *>(p);v.cpu=m.cpu;v.memory=m.memory;v.network=m.network;v.network_send=m.send;v.sampled_ms=m.valid?monotonic_ms():0;v.metrics_expires_ms=m.valid?monotonic_ms()+m.remaining:0;},&m);
}
static bool refresh(const Secrets &s) {
    auto *status = request(s,"/api/v1/status");
    if (!status) return false;
    auto identity=str(status,"agent_id"); auto version=str(status,"protocol_version"); auto server_time=str(status,"server_time");
    if (server_time.empty()) server_time=str(status,"server_utc");
    time_t remote; bool skew = parse_utc(server_time.c_str(),remote) && std::abs(difftime(remote,time(nullptr)))<=5;
    cJSON_Delete(status);
    if (identity!=s.agent_id || (version!="1" && version!="1.0" && version!="1.0.0")) {
        mutate_if_generation(s.generation,[](Snapshot &v,void*){v.error="AGENT_IDENTITY_OR_PROTOCOL_MISMATCH";}); return false;
    }
    if (!skew) { mutate_if_generation(s.generation,[](Snapshot &v,void*){v.error="CLOCK_SKEW：请通过USB校时";}); return false; }
    auto *caps=request(s,"/api/v1/capabilities"), *displays=request(s,"/api/v1/displays"), *list=request(s,"/api/v1/actions"), *hosts=request(s,"/api/v1/hosts");
    bool valid = caps && displays && list && hosts;
    if(config_generation()!=s.generation)valid=false;
    if(valid) {
        Snapshot next=snapshot(); next.action_count=0; auto *items=cJSON_GetObjectItemCaseSensitive(list,"actions");
        cJSON *a; cJSON_ArrayForEach(a,items) {
            if(next.action_count>=12) break;
            Action item; item.id=str(a,"action_id"); if(item.id.empty())item.id=str(a,"id"); item.label=str(a,"label"); if(item.label.empty())item.label=str(a,"name");
            item.generation=s.generation;
            item.target=str(a,"target_id"); item.input=str(cJSON_GetObjectItemCaseSensitive(a,"parameters"),"input_id");
            if(item.input.empty())item.input=str(a,"input_id");
            if(item.id.size()>96 || item.target.size()>96 || item.input.size()>96 || item.id.empty() || item.target.empty() || item.input.empty()) continue;
            next.actions[next.action_count++]=item;
        }
        valid=mutate_if_generation(s.generation,[](Snapshot &v,void *p){auto &next=*static_cast<Snapshot *>(p);v.actions=next.actions;v.action_count=next.action_count;v.agent_ready=true;v.agent="已连接";v.error.clear();},&next);
        parse_metrics(hosts,s.generation);
    }
    cJSON_Delete(caps);cJSON_Delete(displays);cJSON_Delete(list);cJSON_Delete(hosts);return valid;
}
static void task(void *) {
    Backoff retry;
    int64_t next=0, discovery=0;
    while(true) {
        Pending pending{}; if(xQueueReceive(actions,&pending,0)==pdTRUE) { execute(pending,secrets());busy=false; }
        auto st=snapshot(); auto s=secrets();
        if(st.update_state=="downloading"||st.update_state=="writing"||st.update_state=="verifying"||st.update_state=="rebooting"){vTaskDelay(pdMS_TO_TICKS(250));continue;}
        if (st.wifi_ready && st.time_valid && s.ready() && monotonic_ms()>=next) {
            bool ok=false;
            if(!st.agent_ready || monotonic_ms()>=discovery) {ok=refresh(s);if(ok)discovery=monotonic_ms()+30000;}
            else {auto *hosts=request(s,"/api/v1/hosts");ok=hosts!=nullptr;if(hosts){parse_metrics(hosts,s.generation);cJSON_Delete(hosts);}}
            if(ok) { retry.reset();next=monotonic_ms()+(snapshot().metrics_visible?2000:15000); }
            else { mutate_if_generation(s.generation,[](Snapshot &v,void*){v.agent_ready=false;v.action_count=0;v.agent="Agent不可达或认证失败";});next=monotonic_ms()+retry.next_ms(esp_random()); }
        } else if (!st.time_valid) { mutate([](Snapshot &v,void*){v.agent_ready=false;v.agent="等待校时";}); }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
void agent_start() { actions=xQueueCreate(1,sizeof(Pending));xTaskCreate(task,"dh_agent",10240,nullptr,3,nullptr); }
bool agent_submit(const Action &a) {
    auto state=snapshot();
    if (!actions || !state.agent_ready || !state.wifi_ready || state.update_state=="downloading"||state.update_state=="writing"||state.update_state=="verifying"||state.update_state=="rebooting") return false;
    bool expected=false;if(!busy.compare_exchange_strong(expected,true))return false;
    Pending p{}; snprintf(p.id,sizeof(p.id),"%s",a.id.c_str());snprintf(p.target,sizeof(p.target),"%s",a.target.c_str());snprintf(p.input,sizeof(p.input),"%s",a.input.c_str());
    p.generation=config_generation();
    bool eligible=false;
    struct Context {const Action *action;bool *eligible;}context{&a,&eligible};
    mutate_if_generation(p.generation,[](Snapshot &v,void *p){auto &c=*static_cast<Context *>(p);if(!v.agent_ready||!v.wifi_ready)return;for(size_t i=0;i<v.action_count;++i)if(v.actions[i].id==c.action->id&&v.actions[i].target==c.action->target&&v.actions[i].input==c.action->input)*c.eligible=true;},&context);
    if(!eligible||a.generation!=p.generation){busy=false;return false;}
    p.deadline_ms=monotonic_ms()+10000;
    bool accepted=xQueueSend(actions,&p,0)==pdTRUE;if(!accepted)busy=false;
    else mutate_if_generation(p.generation,[](Snapshot &v,void*){v.command="正在连接";});
    return accepted;
}
}
