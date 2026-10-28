#pragma once
#include "cJSON.h"
#include <string>
#include <cstddef>
namespace dh {
struct UpdateManifest {std::string version,hash;size_t size=0;};
bool valid_update_hash(const std::string &);
bool valid_update_id(const std::string &);
bool valid_update_path(const std::string &package,const std::string &path);
bool update_manifest_metadata(cJSON *,const std::string &running_version,UpdateManifest &);
std::string update_restart_state(const std::string &saved,const std::string &target,const std::string &running,bool pending_verify,bool rollback_confirmed=false,bool boot_selection_committed=false);
}
