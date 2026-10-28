#pragma once
#include "cJSON.h"
#include <cstddef>

namespace dh {
// Input must have a terminator at data[len] (std::string::c_str()). Reject
// embedded NUL, trailing non-JSON bytes, and excessive recursion before cJSON.
inline cJSON *parse_json_strict(const char *data,size_t len) {
    if(!data || !len || len>16384)return nullptr;
    int depth=0;bool quoted=false,escaped=false;
    for(size_t i=0;i<len;++i){unsigned char c=data[i];if(!c)return nullptr;
        if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;continue;}
        if(c=='"')quoted=true;
        else if(c=='{'||c=='['){if(++depth>32)return nullptr;}
        else if(c=='}'||c==']'){if(--depth<0)return nullptr;}
    }
    if(quoted||depth)return nullptr;
    return cJSON_ParseWithLengthOpts(data,len+1,nullptr,true);
}
}
