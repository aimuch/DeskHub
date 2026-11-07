#include <atomic>
#include "management.hpp"
#include "config_wire.hpp"
#include "update_policy.hpp"
#include "json_util.hpp"
#include "board.hpp"
#include "power_wire.hpp"
#include "ui.hpp"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_random.h"
#include "nvs.h"
#include "mbedtls/pk.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace dh {
namespace {
constexpr const char *BOARD="ESP32-S3-Touch-AMOLED-2.16";
struct Journal {
    std::string job_id, package_id, agent_id, version, previous_version, target_partition, state="idle", error;
    int progress=0;
    bool boot_selection_committed=false;
};
static Journal journal;
static std::string boot_id,config_error;
static bool journal_healthy=true;
// Read by the Bluetooth update from another task (update_allowed): atomic.
static std::atomic<bool> self_test_pending{false},agent_update_active{false},bluetooth_update_active{false};
static int64_t self_test_deadline=0;
static uint32_t failed_revision=0;
static int64_t failed_config_at=0;
static uint64_t failed_generation=0;
static std::string failed_agent_id;
static std::string text(cJSON *o,const char *key){auto *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
static std::string hex(const unsigned char *bytes,size_t size){static const char alphabet[]="0123456789abcdef";std::string out;out.reserve(size*2);for(size_t i=0;i<size;++i){out+=alphabet[bytes[i]>>4];out+=alphabet[bytes[i]&15];}return out;}
static bool sha(const unsigned char *bytes,size_t size,unsigned char out[32]){size_t written=0;return psa_hash_compute(PSA_ALG_SHA_256,bytes,size,out,32,&written)==PSA_SUCCESS&&written==32;}
static bool decode64(const std::string &encoded,std::vector<unsigned char> &out,size_t maximum){
    if(encoded.empty()||encoded.size()>maximum*2||encoded.size()%4)return false;
    out.resize(encoded.size()/4*3+1);size_t used=0;
    if(mbedtls_base64_decode(out.data(),out.size(),&used,reinterpret_cast<const unsigned char *>(encoded.data()),encoded.size())||used>maximum){out.clear();return false;}
    out.resize(used);return used>0;
}
static bool open_key(const std::string &pem,mbedtls_pk_context &key){
    if(pem.empty()||pem.size()>2048||pem.find("PRIVATE KEY")!=std::string::npos||pem.find("-----BEGIN PUBLIC KEY-----")!=0)return false;
    if(mbedtls_pk_parse_public_key(&key,reinterpret_cast<const unsigned char *>(pem.c_str()),pem.size()+1))return false;
    return mbedtls_pk_get_key_type(&key)==PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1)&&mbedtls_pk_get_bitlen(&key)==256;
}
static bool persist_journal(){
    auto *o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"schema_version",1);
    cJSON_AddStringToObject(o,"job_id",journal.job_id.c_str());cJSON_AddStringToObject(o,"package_id",journal.package_id.c_str());cJSON_AddStringToObject(o,"agent_id",journal.agent_id.c_str());
    cJSON_AddStringToObject(o,"version",journal.version.c_str());cJSON_AddStringToObject(o,"previous_version",journal.previous_version.c_str());cJSON_AddStringToObject(o,"target_partition",journal.target_partition.c_str());cJSON_AddStringToObject(o,"state",journal.state.c_str());cJSON_AddStringToObject(o,"error_code",journal.error.c_str());cJSON_AddNumberToObject(o,"progress_percent",journal.progress);
    cJSON_AddBoolToObject(o,"boot_selection_committed",journal.boot_selection_committed);
    char *raw=cJSON_PrintUnformatted(o);cJSON_Delete(o);if(!raw)return false;size_t size=strlen(raw)+1;
    std::vector<unsigned char> blob(size+4);auto checksum=crc32(raw,size);memcpy(blob.data(),&checksum,4);memcpy(blob.data()+4,raw,size);free(raw);
    nvs_handle_t h;auto e=nvs_open("dh_update",NVS_READWRITE,&h);if(e!=ESP_OK)return false;
    e=nvs_set_blob(h,"journal",blob.data(),blob.size());if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e==ESP_OK;
}
static void publish(){
    mutate([](Snapshot &s,void*){s.update_state=journal.state;s.update_progress=journal.progress;s.update_error=journal.error;});
}
static bool stage(const char *state,int progress=0,const char *error="",bool persistent=true){
    journal.state=state;journal.progress=std::clamp(progress,0,100);journal.error=error;publish();
    if(persistent&&!persist_journal()){journal_healthy=false;journal.state="failed";journal.error="UPDATE_JOURNAL_FAILED";publish();return false;}return true;
}
static void restore_journal(){
    esp_ota_img_states_t boot_state;auto *running=esp_ota_get_running_partition();
    self_test_pending=running&&esp_ota_get_state_partition(running,&boot_state)==ESP_OK&&boot_state==ESP_OTA_IMG_PENDING_VERIFY;
    if(self_test_pending)self_test_deadline=monotonic_ms()+60000;
    nvs_handle_t h;if(nvs_open("dh_update",NVS_READONLY,&h)!=ESP_OK){if(self_test_pending){journal.state="pending_confirmation";publish();}return;}
    size_t size=0;if(nvs_get_blob(h,"journal",nullptr,&size)==ESP_OK){
        if(size<8||size>2048){journal_healthy=false;}else{
            std::vector<unsigned char> blob(size);if(nvs_get_blob(h,"journal",blob.data(),&size)!=ESP_OK)journal_healthy=false;
            else {uint32_t checksum;memcpy(&checksum,blob.data(),4);
                auto *o=blob.back()==0&&checksum==crc32(blob.data()+4,size-4)?parse_json_strict(reinterpret_cast<char *>(blob.data()+4),size-5):nullptr;
                auto *schema=cJSON_GetObjectItemCaseSensitive(o,"schema_version");
                if(!cJSON_IsNumber(schema)||schema->valueint!=1)journal_healthy=false;
                else {journal.job_id=text(o,"job_id");journal.package_id=text(o,"package_id");journal.agent_id=text(o,"agent_id");journal.version=text(o,"version");journal.previous_version=text(o,"previous_version");journal.target_partition=text(o,"target_partition");journal.state=text(o,"state");journal.error=text(o,"error_code");auto *progress=cJSON_GetObjectItemCaseSensitive(o,"progress_percent");journal.progress=cJSON_IsNumber(progress)?std::clamp(progress->valueint,0,100):0;
                    journal.boot_selection_committed=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"boot_selection_committed"));
                    if(!journal.job_id.empty()&&(!valid_update_id(journal.job_id)||!valid_update_hash(journal.package_id)))journal_healthy=false;}
                cJSON_Delete(o);
            }
        }
    }nvs_close(h);
    if(!journal_healthy){journal.state="failed";journal.error="UPDATE_JOURNAL_INVALID";publish();return;}
    esp_ota_img_states_t target_state;bool rollback_confirmed=false;
    if(!journal.target_partition.empty()){
        auto *target=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_ANY,journal.target_partition.c_str());
        rollback_confirmed=target&&target!=running&&esp_ota_get_state_partition(target,&target_state)==ESP_OK&&(target_state==ESP_OTA_IMG_ABORTED||target_state==ESP_OTA_IMG_INVALID);
    }
    auto recovered=update_restart_state(journal.state,journal.version,VERSION,self_test_pending,rollback_confirmed,journal.boot_selection_committed);
    if(self_test_pending){journal.state="pending_confirmation";journal.progress=100;self_test_deadline=monotonic_ms()+60000;publish();}
    else if(journal.state=="rebooting"||journal.state=="pending_confirmation"){
        if(recovered=="rolled_back")stage("rolled_back",0,"BOOT_ROLLBACK");
        else stage("unconfirmed",100,"BOOT_SELECTION_OR_CONFIRMATION_UNKNOWN");
    }else if(journal.state=="downloading"||journal.state=="writing"||journal.state=="verifying")stage("failed",0,"UPDATE_INTERRUPTED");
}
struct Response {std::string body;bool overflow=false;};
static esp_err_t on_data(esp_http_client_event_t *event){
    auto *r=static_cast<Response *>(event->user_data);
    if(event->event_id==HTTP_EVENT_ON_DATA){if(event->data_len<0||r->body.size()+event->data_len>16384){r->overflow=true;return ESP_FAIL;}r->body.append(static_cast<char *>(event->data),event->data_len);}return ESP_OK;
}
static cJSON *post(const Secrets &s,cJSON *report){
    if(config_generation()!=s.generation)return nullptr;
    char *body=cJSON_PrintUnformatted(report);if(!body)return nullptr;
    if(strlen(body)>4096){free(body);return nullptr;}
    auto url=s.url+"/api/v1/device/sync";auto auth="Bearer "+s.token;Response response;
    esp_http_client_config_t cfg{};cfg.url=url.c_str();cfg.cert_pem=s.ca_pem.c_str();cfg.timeout_ms=5000;cfg.disable_auto_redirect=true;cfg.event_handler=on_data;cfg.user_data=&response;cfg.buffer_size=1024;
    auto client=esp_http_client_init(&cfg);if(!client){free(body);return nullptr;}
    esp_http_client_set_method(client,HTTP_METHOD_POST);esp_http_client_set_header(client,"Authorization",auth.c_str());esp_http_client_set_header(client,"Content-Type","application/json");esp_http_client_set_post_field(client,body,strlen(body));
    auto error=esp_http_client_perform(client);int status=esp_http_client_get_status_code(client);esp_http_client_cleanup(client);free(body);
    if(error!=ESP_OK||response.overflow||status!=200||config_generation()!=s.generation)return nullptr;
    auto *o=parse_json_strict(response.body.c_str(),response.body.size());
    if(!cJSON_IsObject(o)||text(o,"agent_id")!=s.agent_id){cJSON_Delete(o);return nullptr;}return o;
}
static bool stable_power(){auto power=board_power();return power.vbus=="present"&&!power.fault&&power.observed_ms>0&&monotonic_ms()-power.observed_ms<=6000;}
static cJSON *report(const Secrets &s){
    auto st=snapshot();auto c=config();auto power=board_power();auto *o=cJSON_CreateObject();
    cJSON_AddStringToObject(o,"firmware_version",VERSION);cJSON_AddStringToObject(o,"board_model",BOARD);cJSON_AddStringToObject(o,"boot_id",boot_id.c_str());cJSON_AddNumberToObject(o,"uptime_seconds",monotonic_ms()/1000);cJSON_AddNumberToObject(o,"free_heap_bytes",esp_get_free_heap_size());
    const bool ui_applied=ui_configuration_applied(c.revision);
    cJSON_AddNumberToObject(o,"applied_revision",c.managed_agent_id==s.agent_id&&ui_applied?c.managed_revision:0);cJSON_AddItemToObject(o,"configuration",configuration_json(c));
    const char *application_error=st.error.rfind("DISPLAY_ROTATION_FAILED:",0)==0?"DISPLAY_ROTATION_FAILED":nullptr;
    if(!config_error.empty())cJSON_AddStringToObject(o,"config_error",config_error.c_str());
    else if(application_error)cJSON_AddStringToObject(o,"config_error",application_error);
    else cJSON_AddNullToObject(o,"config_error");
    cJSON_AddItemToObject(o,"power",power_report_json(power,monotonic_ms()));
    auto *caps=cJSON_AddObjectToObject(o,"capabilities");cJSON_AddBoolToObject(caps,"management",true);auto fingerprint=update_key_fingerprint(s.update_public_key_pem);
    cJSON_AddBoolToObject(caps,"signed_ota",journal_healthy&&!fingerprint.empty());if(fingerprint.empty())cJSON_AddNullToObject(caps,"update_key_fingerprint");else cJSON_AddStringToObject(caps,"update_key_fingerprint",fingerprint.c_str());
    auto *layouts=cJSON_AddArrayToObject(caps,"layouts");for(auto value:{"desk","clock","focus"})cJSON_AddItemToArray(layouts,cJSON_CreateString(value));
    if(journal.job_id.empty()||journal.agent_id!=s.agent_id)cJSON_AddNullToObject(o,"update");
    else {auto *update=cJSON_AddObjectToObject(o,"update");cJSON_AddStringToObject(update,"job_id",journal.job_id.c_str());cJSON_AddStringToObject(update,"state",journal.state.c_str());cJSON_AddNumberToObject(update,"progress_percent",journal.progress);if(journal.error.empty())cJSON_AddNullToObject(update,"error_code");else cJSON_AddStringToObject(update,"error_code",journal.error.c_str());}
    return o;
}
static void send_progress(const Secrets &s){auto *o=report(s);auto *reply=post(s,o);cJSON_Delete(reply);cJSON_Delete(o);}
static bool manifest(cJSON *envelope,const Secrets &s,UpdateManifest &result,std::string &error){
    error="INVALID_UPDATE_MANIFEST";
    if(!cJSON_IsObject(envelope)||cJSON_GetArraySize(envelope)!=3||text(envelope,"algorithm")!="ecdsa-p256-sha256")return false;
    std::vector<unsigned char> payload,signature;
    if(!decode64(text(envelope,"payload"),payload,2048)||!decode64(text(envelope,"signature"),signature,80))return false;
    mbedtls_pk_context key;mbedtls_pk_init(&key);bool key_ok=open_key(s.update_public_key_pem,key);unsigned char digest[32];
    bool verified=key_ok&&sha(payload.data(),payload.size(),digest)&&mbedtls_pk_verify(&key,MBEDTLS_MD_SHA256,digest,sizeof(digest),signature.data(),signature.size())==0;mbedtls_pk_free(&key);
    if(!verified){error="UPDATE_SIGNATURE_INVALID";return false;}
    payload.push_back(0);auto *o=parse_json_strict(reinterpret_cast<char *>(payload.data()),payload.size()-1);
    bool valid=update_manifest_metadata(o,VERSION,result);
    cJSON_Delete(o);return valid;
}
static bool image_header(const std::vector<unsigned char> &prefix,const UpdateManifest &m){
    if(prefix.size()<sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t))return false;
    esp_image_header_t header{};esp_app_desc_t app{};memcpy(&header,prefix.data(),sizeof(header));memcpy(&app,prefix.data()+sizeof(header)+sizeof(esp_image_segment_header_t),sizeof(app));
    return header.magic==ESP_IMAGE_HEADER_MAGIC&&header.chip_id==ESP_CHIP_ID_ESP32S3&&app.magic_word==ESP_APP_DESC_MAGIC_WORD&&memchr(app.version,0,sizeof(app.version))&&m.version==app.version&&memchr(app.project_name,0,sizeof(app.project_name))&&strcmp(app.project_name,"deskhub")==0;
}
static void install_job(cJSON *job,const Secrets &s);
static void install(cJSON *job,const Secrets &s){
    if(bluetooth_update_active||!cJSON_IsObject(job))return;   // one update at a time (13.11 U5)
    agent_update_active=true;install_job(job,s);agent_update_active=false;
}
static void install_job(cJSON *job,const Secrets &s){
    auto job_id=text(job,"job_id"),package=text(job,"package_id"),path=text(job,"image_path"),expiry=text(job,"expires_at");
    if(!valid_update_id(job_id)||!valid_update_hash(package)||!valid_update_path(package,path)||job_id==journal.job_id)return;
    // Claim the transition under Core's mutex before any journal or flash write.
    // An in-flight management reply cannot start OTA during a Wi-Fi trial.
    if(!network_begin_update(s.generation))return;
    time_t expires;auto *next=esp_ota_get_next_update_partition(nullptr);
    journal={};journal.job_id=job_id;journal.package_id=package;journal.agent_id=s.agent_id;journal.previous_version=VERSION;if(next)journal.target_partition=next->label;
    if(!journal_healthy||self_test_pending||!core_storage_healthy()||!snapshot().time_valid||!parse_utc(expiry.c_str(),expires)||expires<=time(nullptr)||!next||!stable_power()){stage("failed",0,"UPDATE_PRECONDITION_FAILED");send_progress(s);return;}
    if(!stage("verifying"))return;
    UpdateManifest m;std::string error;
    if(!manifest(cJSON_GetObjectItemCaseSensitive(job,"manifest"),s,m,error)||m.hash!=package||m.size>next->size){stage("failed",0,error.c_str());send_progress(s);return;}
    journal.version=m.version;if(!stage("downloading"))return;send_progress(s);
    auto url=s.url+path,auth="Bearer "+s.token;
    esp_http_client_config_t cfg{};cfg.url=url.c_str();cfg.cert_pem=s.ca_pem.c_str();cfg.timeout_ms=5000;cfg.disable_auto_redirect=true;cfg.buffer_size=1024;
    auto client=esp_http_client_init(&cfg);if(!client){stage("failed",0,"UPDATE_CONNECTION_FAILED");return;}
    esp_http_client_set_header(client,"Authorization",auth.c_str());esp_http_client_set_header(client,"Accept","application/octet-stream");esp_http_client_set_header(client,"Accept-Encoding","identity");
    esp_ota_handle_t ota=0;bool begun=false,ended=false;psa_hash_operation_t hash=PSA_HASH_OPERATION_INIT;std::array<unsigned char,4096> buffer{};std::vector<unsigned char> prefix;prefix.reserve(288);
    const char *failure=nullptr;size_t received=0;int reported=0;int64_t deadline=monotonic_ms()+120000,last_progress_at=monotonic_ms();
    if(esp_http_client_open(client,0)!=ESP_OK||esp_http_client_fetch_headers(client)!=static_cast<int64_t>(m.size)||esp_http_client_get_status_code(client)!=200)failure="UPDATE_DOWNLOAD_REJECTED";
    if(!failure&&psa_hash_setup(&hash,PSA_ALG_SHA_256)!=PSA_SUCCESS)failure="UPDATE_HASH_FAILED";
    while(!failure&&received<m.size){
        if(monotonic_ms()>deadline||config_generation()!=s.generation||!stable_power()){failure="UPDATE_POWER_OR_CONNECTION_CHANGED";break;}
        int count=esp_http_client_read(client,reinterpret_cast<char *>(buffer.data()),std::min(buffer.size(),m.size-received));
        if(count<=0){failure="UPDATE_DOWNLOAD_TRUNCATED";break;}
        // A read can block while power or pairing changes. Recheck after the
        // blocking boundary before any Flash side effect.
        if(monotonic_ms()>deadline||config_generation()!=s.generation||!stable_power()){failure="UPDATE_POWER_OR_CONNECTION_CHANGED";break;}
        if(psa_hash_update(&hash,buffer.data(),count)!=PSA_SUCCESS){failure="UPDATE_HASH_FAILED";break;}
        size_t consumed=0;
        if(!begun){size_t amount=std::min<size_t>(288-prefix.size(),count);prefix.insert(prefix.end(),buffer.data(),buffer.data()+amount);consumed=amount;
            if(prefix.size()==288){if(!image_header(prefix,m)){failure="UPDATE_IMAGE_MISMATCH";break;}
                if(esp_ota_begin(next,m.size,&ota)!=ESP_OK){failure="UPDATE_BEGIN_FAILED";break;}begun=true;
                if(!stage("writing",0)){failure="UPDATE_JOURNAL_FAILED";break;}
                if(esp_ota_write(ota,prefix.data(),prefix.size())!=ESP_OK){failure="UPDATE_WRITE_FAILED";break;}}
        }
        if(begun&&consumed<static_cast<size_t>(count)&&esp_ota_write(ota,buffer.data()+consumed,count-consumed)!=ESP_OK){failure="UPDATE_WRITE_FAILED";break;}
        received+=count;journal.progress=static_cast<int>(received*99/m.size);publish();
        if(journal.progress>=reported+10&&monotonic_ms()-last_progress_at>=5000){reported=journal.progress;last_progress_at=monotonic_ms();send_progress(s);}
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    unsigned char digest[32];size_t length=0;
    if(!failure&&(!begun||!esp_http_client_is_complete_data_received(client)||psa_hash_finish(&hash,digest,sizeof(digest),&length)!=PSA_SUCCESS||length!=32||hex(digest,32)!=m.hash))failure="UPDATE_HASH_MISMATCH";
    esp_http_client_close(client);esp_http_client_cleanup(client);psa_hash_abort(&hash);
    if(!failure){stage("verifying",99,"",false);send_progress(s);auto e=esp_ota_end(ota);ended=true;if(e!=ESP_OK)failure="UPDATE_IMAGE_INVALID";}
    if(!failure&&(config_generation()!=s.generation||!stable_power()))failure="UPDATE_POWER_OR_CONNECTION_CHANGED";
    if(failure){if(begun&&!ended)esp_ota_abort(ota);stage("failed",journal.progress,failure);send_progress(s);return;}
    if(!stage("rebooting",100))return;
    // This short local flash operation is the installation commit boundary.
    // It shares Core's lock with USB provisioning/reset, never with HTTP/I2C.
    auto selected=with_generation(s.generation,[](void *ctx){return esp_ota_set_boot_partition(static_cast<const esp_partition_t *>(ctx));},const_cast<esp_partition_t *>(next));
    if(selected!=ESP_OK){stage("failed",100,selected==ESP_ERR_INVALID_STATE?"UPDATE_CREDENTIALS_CHANGED":"UPDATE_BOOT_SELECTION_FAILED");send_progress(s);return;}
    // Old ABORTED metadata in a reused slot is not evidence that this job ran.
    // Only a durable record after successful boot selection links that evidence
    // to this installation; a lost record remains conservatively unconfirmed.
    journal.boot_selection_committed=true;
    if(!persist_journal()){journal_healthy=false;stage("unconfirmed",100,"UPDATE_COMMIT_RECORD_FAILED",false);}
    send_progress(s);vTaskDelay(pdMS_TO_TICKS(250));esp_restart();
}
static void apply(cJSON *desired,const Secrets &s){
    if(!cJSON_IsObject(desired))return;
    auto *r=cJSON_GetObjectItemCaseSensitive(desired,"revision");
    if(!cJSON_IsNumber(r)||r->valuedouble<1||r->valuedouble>2147483647||std::floor(r->valuedouble)!=r->valuedouble)return;
    uint32_t revision=r->valueint;auto c=config();
    if(c.managed_agent_id==s.agent_id&&revision<=c.managed_revision)return;
    if(revision==failed_revision&&failed_generation==s.generation&&failed_agent_id==s.agent_id&&monotonic_ms()-failed_config_at<60000)return;
    config_error.clear();
    if(!configuration_parse(cJSON_GetObjectItemCaseSensitive(desired,"configuration"),c))config_error="INVALID_CONFIGURATION";
    auto st=snapshot();
    for(size_t i=0;config_error.empty()&&i<c.layout.action_count;++i){bool allowed=false;for(size_t j=0;j<st.action_count;++j)if(st.actions[j].id==c.layout.action_ids[i]&&st.actions[j].generation==s.generation)allowed=true;if(!allowed)config_error="LAYOUT_ACTION_UNAVAILABLE";}
    if(config_error.empty()){c.managed_revision=revision;c.managed_agent_id=s.agent_id;auto e=save_managed_config(c,s.generation);if(e!=ESP_OK)config_error=e==ESP_ERR_NOT_SUPPORTED?"CONFIG_SCHEMA_UNSUPPORTED":"CONFIG_COMMIT_FAILED";}
    if(!config_error.empty()){failed_revision=revision;failed_config_at=monotonic_ms();failed_generation=s.generation;failed_agent_id=s.agent_id;}
    else failed_revision=0;
}
static void self_test(){
    if(!self_test_pending||monotonic_ms()<self_test_deadline)return;
    auto power=board_power();bool good=journal_healthy&&ui_alive()&&core_storage_healthy()&&power.observed_ms>0&&monotonic_ms()-power.observed_ms<=6000&&!power.fault;
    if(good&&esp_ota_mark_app_valid_cancel_rollback()==ESP_OK){self_test_pending=false;stage("succeeded",100);}
    else {
        // Rollback can fail without rebooting (e.g. no usable previous slot).
        // Try once, then latch maintenance state; never loop Flash writes.
        self_test_pending=false;stage("failed",100,"BOOT_SELF_TEST_FAILED");
        if(esp_ota_mark_app_invalid_rollback_and_reboot()!=ESP_OK){journal_healthy=false;stage("failed",100,"UPDATE_ROLLBACK_FAILED",false);}
    }
}
static void task(void *){
    restore_journal();int64_t next=0;
    for(;;){self_test();auto st=snapshot();auto s=secrets();
        if(network_owner()==NetworkOwner::None&&st.wifi_ready&&st.agent_ready&&st.time_valid&&s.ready()&&monotonic_ms()>=next){
            auto *request=report(s);auto *reply=post(s,request);cJSON_Delete(request);
            if(reply){apply(cJSON_GetObjectItemCaseSensitive(reply,"desired"),s);mutate_if_generation(s.generation,[](Snapshot &v,void*){v.management="管理台已连接";});install(cJSON_GetObjectItemCaseSensitive(reply,"update"),s);cJSON_Delete(reply);next=monotonic_ms()+10000;}
            else {mutate_if_generation(s.generation,[](Snapshot &v,void*){v.management="管理台不可用或未授权";});next=monotonic_ms()+30000;}
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
} // namespace
bool update_public_key_valid(const std::string &pem){psa_crypto_init();mbedtls_pk_context key;mbedtls_pk_init(&key);bool ok=open_key(pem,key);mbedtls_pk_free(&key);return ok;}
bool update_allowed(std::string &why){
    if(agent_update_active||bluetooth_update_active){why="UPDATE_BUSY";return false;}
    if(self_test_pending){why="UPDATE_SELF_TEST_PENDING";return false;}
    if(!journal_healthy||!core_storage_healthy()){why="UPDATE_STORAGE_FAULT";return false;}
    if(!stable_power()){why="UPDATE_POWER_UNSTABLE";return false;}
    why.clear();return true;
}
bool verify_update_envelope(const std::string &json,UpdateManifest &m,std::string &error){
    psa_crypto_init();
    auto *o=parse_json_strict(json.c_str(),json.size());
    bool ok=o&&manifest(o,secrets(),m,error);
    if(!o)error="INVALID_UPDATE_MANIFEST";
    cJSON_Delete(o);return ok;
}
void external_update(bool active){bluetooth_update_active=active;}
std::string update_key_fingerprint(const std::string &pem){
    psa_crypto_init();mbedtls_pk_context key;mbedtls_pk_init(&key);unsigned char encoded[256],digest[32];std::string result;
    if(open_key(pem,key)){int length=mbedtls_pk_write_pubkey_der(&key,encoded,sizeof(encoded));if(length>0&&sha(encoded+sizeof(encoded)-length,length,digest))result=hex(digest,32);}
    mbedtls_pk_free(&key);return result;
}
void management_start(){psa_crypto_init();uint8_t nonce[16];esp_fill_random(nonce,sizeof(nonce));boot_id="boot-"+hex(nonce,sizeof(nonce));xTaskCreate(task,"dh_management",16384,nullptr,3,nullptr);}
} // namespace dh
