#include "config_wire.hpp"
#include "update_policy.hpp"
#include "json_util.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>

static cJSON *baseline(){dh::DisplayConfig c;return dh::configuration_json(c);}
static bool accepts(cJSON *o){dh::DisplayConfig c;return dh::configuration_parse(o,c);}
int main(){
    auto *o=baseline();assert(accepts(o));dh::DisplayConfig c;c.revision=7;c.managed_revision=4;c.managed_agent_id="agent-a";
    auto *layout=cJSON_GetObjectItemCaseSensitive(o,"layout");cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(layout,"preset"),"focus");
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(layout,"action_ids"),cJSON_CreateString("desktop.pc"));
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(layout,"action_ids"),cJSON_CreateString("desktop.mac"));
    assert(dh::configuration_parse(o,c));assert(c.layout.preset=="focus"&&c.layout.action_ids[0]=="desktop.pc"&&c.layout.action_ids[1]=="desktop.mac");
    assert(c.revision==7&&c.managed_revision==4&&c.managed_agent_id=="agent-a");
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(layout,"action_ids"),cJSON_CreateString("desktop.pc"));assert(!accepts(o));cJSON_Delete(o);
    o=baseline();cJSON_AddStringToObject(o,"script","arbitrary");assert(!accepts(o));cJSON_Delete(o);
    o=baseline();auto *display=cJSON_GetObjectItemCaseSensitive(o,"display");cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(display,"brightness"),0);assert(!accepts(o));cJSON_Delete(o);
    o=baseline();display=cJSON_GetObjectItemCaseSensitive(o,"display");cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(display,"off_s"),30);assert(!accepts(o));cJSON_Delete(o);
    o=baseline();layout=cJSON_GetObjectItemCaseSensitive(o,"layout");cJSON_ReplaceItemInObject(layout,"show_cpu",cJSON_CreateNumber(1));assert(!accepts(o));cJSON_Delete(o);
    o=baseline();layout=cJSON_GetObjectItemCaseSensitive(o,"layout");cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(layout,"preset"),"javascript");assert(!accepts(o));cJSON_Delete(o);
    o=baseline();display=cJSON_GetObjectItemCaseSensitive(o,"display");layout=cJSON_GetObjectItemCaseSensitive(o,"layout");
    for(int angle:{0,90,180,270}){
        cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(display,"rotation"),angle);
        for(const char *theme:{"mint","ocean","amber"}){
            cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(layout,"theme"),theme);
            assert(dh::configuration_parse(o,c)&&c.rotation==angle&&c.layout.theme==theme);
            auto *round_trip=dh::configuration_json(c);assert(accepts(round_trip));cJSON_Delete(round_trip);
        }
    }
    for(double angle:{-90.,1.,45.,90.5,360.}){cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(display,"rotation"),angle);assert(!accepts(o));}
    cJSON_ReplaceItemInObject(display,"rotation",cJSON_CreateBool(true));assert(!accepts(o));
    cJSON_ReplaceItemInObject(display,"rotation",cJSON_CreateNumber(0));
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(layout,"theme"),"arbitrary");assert(!accepts(o));
    // A legacy complete configuration explicitly receives defaults, rather
    // than inheriting the receiver's previous orientation or theme.
    cJSON_DeleteItemFromObject(display,"rotation");cJSON_DeleteItemFromObject(layout,"theme");
    c.rotation=270;c.layout.theme="amber";assert(dh::configuration_parse(o,c)&&c.rotation==0&&c.layout.theme=="mint");
    cJSON_Delete(o);
    const std::string payload=R"({"schema_version":1,"board_model":"ESP32-S3-Touch-AMOLED-2.16","chip":"esp32s3","version":"0.2.1-dev","config_schema_min":1,"config_schema_max":1,"image_size":1000000,"image_sha256":")"+std::string(64,'a')+"\"}";
    o=dh::parse_json_strict(payload.c_str(),payload.size());dh::UpdateManifest m;assert(dh::update_manifest_metadata(o,"0.2.0-dev",m));assert(m.version=="0.2.1-dev"&&m.size==1000000);
    assert(!dh::update_manifest_metadata(o,"0.2.1-dev",m));
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(o,"board_model"),"other-board");assert(!dh::update_manifest_metadata(o,"0.2.0-dev",m));
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(o,"board_model"),"ESP32-S3-Touch-AMOLED-2.16");cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(o,"image_size"),6*1024*1024+1);assert(!dh::update_manifest_metadata(o,"0.2.0-dev",m));
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(o,"image_size"),1000000);cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(o,"config_schema_max"),2);assert(!dh::update_manifest_metadata(o,"0.2.0-dev",m));cJSON_Delete(o);
    auto hash=std::string(64,'a');assert(dh::valid_update_path(hash,"/api/v1/device/firmware/"+hash+"/image"));
    assert(!dh::valid_update_path(hash,"https://evil.invalid/image"));assert(!dh::valid_update_path(hash,"/api/v1/device/firmware/../../secret/image"));
    assert(dh::update_restart_state("writing","0.2.1-dev","0.2.0-dev",false)=="failed");
    assert(dh::update_restart_state("rebooting","0.2.1-dev","0.2.0-dev",false,true,true)=="rolled_back");
    // A stale ABORTED flag from a previous attempt cannot prove this job booted.
    assert(dh::update_restart_state("rebooting","0.2.1-dev","0.2.0-dev",false,true,false)=="unconfirmed");
    assert(dh::update_restart_state("rebooting","0.2.1-dev","0.2.0-dev",false,false)=="unconfirmed");
    assert(dh::update_restart_state("rebooting","0.2.1-dev","0.2.1-dev",true)=="pending_confirmation");
    assert(dh::update_restart_state("pending_confirmation","0.2.1-dev","0.2.1-dev",false)=="unconfirmed");
    assert(dh::update_restart_state("succeeded","0.2.1-dev","0.2.1-dev",false)=="succeeded");
    puts("management: strict layout/settings, order, preserved local metadata, forbidden scripts/invalid ranges, signed metadata bounds, same-version rejection, same-origin path, interrupted-update recovery passed");
}
