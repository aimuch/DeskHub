#pragma once
#include "core.hpp"
#include "cJSON.h"
namespace dh {
cJSON *configuration_json(const DisplayConfig &);
bool configuration_parse(cJSON *, DisplayConfig &);
}
