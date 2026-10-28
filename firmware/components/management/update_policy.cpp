#include "update_policy.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace dh {
bool valid_update_hash(const std::string &s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](unsigned char c){return(c>='a'&&c<='f')||(c>='0'&&c<='9');});}
bool valid_update_id(const std::string &s){if(s.empty()||s.size()>96)return false;for(unsigned char c:s)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c==':'))return false;return true;}
bool valid_update_path(const std::string &package,const std::string &path){return valid_update_hash(package)&&path=="/api/v1/device/firmware/"+package+"/image";}
static std::string field(cJSON *o,const char *key){auto *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
bool update_manifest_metadata(cJSON *o,const std::string &running_version,UpdateManifest &result){
    static const char *const names[]={"schema_version","board_model","chip","version","config_schema_min","config_schema_max","image_size","image_sha256"};
    if(!cJSON_IsObject(o)||cJSON_GetArraySize(o)!=8)return false;
    for(auto name:names){int found=0;cJSON *item;cJSON_ArrayForEach(item,o)if(item->string&&strcmp(item->string,name)==0)++found;if(found!=1)return false;}
    auto *schema=cJSON_GetObjectItemCaseSensitive(o,"schema_version"),*size=cJSON_GetObjectItemCaseSensitive(o,"image_size"),*minimum=cJSON_GetObjectItemCaseSensitive(o,"config_schema_min"),*maximum=cJSON_GetObjectItemCaseSensitive(o,"config_schema_max");
    bool valid=cJSON_IsNumber(schema)&&schema->valuedouble==1&&field(o,"board_model")=="ESP32-S3-Touch-AMOLED-2.16"&&field(o,"chip")=="esp32s3"&&cJSON_IsNumber(minimum)&&minimum->valuedouble==1&&cJSON_IsNumber(maximum)&&maximum->valuedouble==1&&cJSON_IsNumber(size)&&std::isfinite(size->valuedouble)&&size->valuedouble>=288&&size->valuedouble<=6*1024*1024&&std::floor(size->valuedouble)==size->valuedouble;
    if(!valid)return false;
    UpdateManifest candidate;candidate.version=field(o,"version");candidate.hash=field(o,"image_sha256");candidate.size=size->valueint;
    if(candidate.version.empty()||candidate.version.size()>=32||candidate.version==running_version||!valid_update_hash(candidate.hash))return false;
    for(unsigned char c:candidate.version)if(c<33||c>126)return false;
    result=candidate;return true;
}
std::string update_restart_state(const std::string &saved,const std::string &target,const std::string &running,bool pending_verify,bool rollback_confirmed,bool boot_selection_committed){
    if(pending_verify)return "pending_confirmation";
    if(saved=="rebooting"||saved=="pending_confirmation")return target!=running&&rollback_confirmed&&boot_selection_committed?"rolled_back":"unconfirmed";
    if(saved=="downloading"||saved=="writing"||saved=="verifying")return "failed";
    return saved;
}
}
