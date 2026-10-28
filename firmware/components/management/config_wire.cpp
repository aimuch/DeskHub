#include "config_wire.hpp"
#include <cmath>
#include <cstring>
namespace dh {
static bool integer(cJSON *object,const char *key,int low,int high,int &value) {
    auto *item=cJSON_GetObjectItemCaseSensitive(object,key);
    if(!cJSON_IsNumber(item)||!std::isfinite(item->valuedouble)||item->valuedouble<low||item->valuedouble>high||std::floor(item->valuedouble)!=item->valuedouble)return false;
    value=item->valueint;return true;
}
static bool boolean(cJSON *object,const char *key,bool &value) {
    auto *item=cJSON_GetObjectItemCaseSensitive(object,key);if(!cJSON_IsBool(item))return false;value=cJSON_IsTrue(item);return true;
}
static bool exact(cJSON *object,const char *const *keys,size_t count) {
    if(!cJSON_IsObject(object)||static_cast<size_t>(cJSON_GetArraySize(object))!=count)return false;
    for(size_t i=0;i<count;++i){int found=0;cJSON *item;cJSON_ArrayForEach(item,object)if(item->string&&strcmp(item->string,keys[i])==0)++found;if(found!=1)return false;}
    return true;
}
bool configuration_parse(cJSON *object,DisplayConfig &out) {
    static const char *const root_keys[]={"schema_version","display","layout"};
    static const char *const display_keys[]={"brightness","clock_s","dim_s","off_s","night_enabled","night_start","night_end","night_cap","protection","rotation"};
    static const char *const layout_keys[]={"preset","action_ids","show_seconds","show_cpu","theme"};
    if(!exact(object,root_keys,3))return false;
    int value;
    if(!integer(object,"schema_version",1,1,value))return false;
    auto *d=cJSON_GetObjectItemCaseSensitive(object,"display"),*l=cJSON_GetObjectItemCaseSensitive(object,"layout");
    const bool has_rotation=cJSON_HasObjectItem(d,"rotation"),has_theme=cJSON_HasObjectItem(l,"theme");
    if(!exact(d,display_keys,has_rotation?10:9)||!exact(l,layout_keys,has_theme?5:4))return false;
    DisplayConfig c=out;
    c.rotation=0;c.layout.theme="mint";
    if(has_rotation){if(!integer(d,"rotation",0,270,value)||value%90)return false;c.rotation=value;}
    if(has_theme){auto *theme=cJSON_GetObjectItemCaseSensitive(l,"theme");if(!cJSON_IsString(theme))return false;c.layout.theme=theme->valuestring;}
    if(!integer(d,"brightness",3,100,value))return false;
    c.brightness=value;
    if(!integer(d,"clock_s",5,86400,value))return false;
    c.clock_s=value;
    if(!integer(d,"dim_s",5,86400,value))return false;
    c.dim_s=value;
    if(!integer(d,"off_s",0,86400,value))return false;
    c.off_s=value;
    if(!integer(d,"night_start",0,1439,value))return false;
    c.night_start=value;
    if(!integer(d,"night_end",0,1439,value))return false;
    c.night_end=value;
    if(!integer(d,"night_cap",3,100,value))return false;
    c.night_cap=value;
    if(!boolean(d,"night_enabled",c.night_enabled)||!boolean(d,"protection",c.protection)||!boolean(l,"show_seconds",c.layout.show_seconds)||!boolean(l,"show_cpu",c.layout.show_cpu))return false;
    auto *preset=cJSON_GetObjectItemCaseSensitive(l,"preset"),*ids=cJSON_GetObjectItemCaseSensitive(l,"action_ids");
    if(!cJSON_IsString(preset)||!cJSON_IsArray(ids)||cJSON_GetArraySize(ids)>4)return false;
    c.layout.preset=preset->valuestring;c.layout.action_count=0;c.layout.action_ids={};
    cJSON *id;cJSON_ArrayForEach(id,ids){if(!cJSON_IsString(id))return false;
    c.layout.action_ids[c.layout.action_count++]=id->valuestring;}
    if(!valid_config(c))return false;
    out=c;return true;
}
cJSON *configuration_json(const DisplayConfig &c) {
    auto *o=cJSON_CreateObject();cJSON_AddNumberToObject(o,"schema_version",1);auto *d=cJSON_AddObjectToObject(o,"display");
    cJSON_AddNumberToObject(d,"brightness",c.brightness);cJSON_AddNumberToObject(d,"clock_s",c.clock_s);cJSON_AddNumberToObject(d,"dim_s",c.dim_s);cJSON_AddNumberToObject(d,"off_s",c.off_s);
    cJSON_AddNumberToObject(d,"rotation",c.rotation);
    cJSON_AddBoolToObject(d,"night_enabled",c.night_enabled);cJSON_AddNumberToObject(d,"night_start",c.night_start);cJSON_AddNumberToObject(d,"night_end",c.night_end);cJSON_AddNumberToObject(d,"night_cap",c.night_cap);cJSON_AddBoolToObject(d,"protection",c.protection);
    auto *l=cJSON_AddObjectToObject(o,"layout");cJSON_AddStringToObject(l,"preset",c.layout.preset.c_str());cJSON_AddBoolToObject(l,"show_seconds",c.layout.show_seconds);cJSON_AddBoolToObject(l,"show_cpu",c.layout.show_cpu);
    cJSON_AddStringToObject(l,"theme",c.layout.theme.c_str());
    auto *ids=cJSON_AddArrayToObject(l,"action_ids");for(size_t i=0;i<c.layout.action_count;++i)cJSON_AddItemToArray(ids,cJSON_CreateString(c.layout.action_ids[i].c_str()));return o;
}
}
