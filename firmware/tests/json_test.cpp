#include "json_util.hpp"
#include <cassert>
#include <string>
#include <cstdio>
static bool parses(const std::string &s){auto *j=dh::parse_json_strict(s.c_str(),s.size());bool ok=j!=nullptr;cJSON_Delete(j);return ok;}
int main(){
    assert(parses("{\"op\":\"set_time\",\"time\":{\"utc\":\"2026-10-27T03:00:00Z\"}}"));
    assert(parses("{\"state\":\"succeeded\"} \r\n\t"));
    assert(!parses("{\"op\":\"set_time\"}garbage"));
    assert(!parses("{\"state\":\"succeeded\"}{\"another\":true}"));
    assert(!parses(std::string("{}\0malicious",12)));
    assert(parses("{\"quoted\":\"[[[{}\\\"text\"}"));
    assert(parses(std::string(32,'[')+"0"+std::string(32,']')));
    assert(!parses(std::string(33,'[')+"0"+std::string(33,']')));
    assert(!parses(std::string(16385,' ')));
    assert(!parses("{\"incomplete\":"));
    puts("strict parser: valid frame, whitespace, prefix junk, concatenated documents, embedded NUL, quoted braces, depth limit, size limit passed");
}
