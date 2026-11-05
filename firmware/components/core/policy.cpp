#include "policy.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
namespace dh {
bool valid_layout(const LayoutConfig &c) {
    if(c.preset!="desk"&&c.preset!="clock"&&c.preset!="focus")return false;
    if(c.theme!="mint"&&c.theme!="ocean"&&c.theme!="amber")return false;
    if(c.action_count>4)return false;
    for(size_t i=0;i<c.action_count;++i){const auto &id=c.action_ids[i];if(id.empty()||id.size()>96||!((id[0]>='A'&&id[0]<='Z')||(id[0]>='a'&&id[0]<='z')||(id[0]>='0'&&id[0]<='9')))return false;
        for(unsigned char ch:id)if(!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.'||ch==':'))return false;
        for(size_t j=0;j<i;++j)if(c.action_ids[j]==id)return false;}
    return true;
}

namespace {
constexpr int kZones[] = {-720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60, 0, 60, 120, 180, 210, 240,
                          270, 300, 330, 345, 360, 390, 420, 480, 525, 540, 570, 600, 630, 660, 720, 765, 780, 840};
constexpr int kZoneCount = static_cast<int>(sizeof(kZones) / sizeof(kZones[0]));
int zone_index(int minutes) {
    for (int i = 0; i < kZoneCount; ++i) if (kZones[i] == minutes) return i;
    return -1;
}
}  // namespace
bool valid_tz(int minutes) { return zone_index(minutes) >= 0; }
int tz_step(int minutes, int delta) {
    const int i = zone_index(minutes);
    if (i < 0) return kDefaultTzMinutes;
    return kZones[std::min(kZoneCount - 1, std::max(0, i + delta))];
}
std::string tz_env(int minutes) {
    const int size = minutes < 0 ? -minutes : minutes;
    char text[16];
    snprintf(text, sizeof(text), "UTC%c%02d:%02d", minutes > 0 ? '-' : '+', size / 60, size % 60);
    return text;
}

bool valid_config(const DisplayConfig &c) {
    return valid_tz(c.tz_minutes) && valid_layout(c.layout) && c.schema == 1 && c.rotation<=270 && c.rotation%90==0 && c.brightness <= 100 && c.night_cap <= 100 &&
        c.night_start < 1440 && c.night_end < 1440 && c.clock_s >= 5 &&
        c.clock_s <= 86400 && c.dim_s >= c.clock_s && c.dim_s <= 86400 &&
        (c.off_s == 0 || (c.off_s >= c.dim_s && c.off_s <= 86400));
}
bool in_night(uint16_t t, uint16_t a, uint16_t b) {
    if (a == b) return false;
    return a < b ? t >= a && t < b : t >= a || t < b;
}
bool valid_ntp_server(const std::string &s) {
    if(s.empty()||s.size()>253)return false;
    bool numeric=true;for(unsigned char c:s)if(c!='.'&&(c<'0'||c>'9'))numeric=false;
    size_t start=0, labels=0;
    while(start<s.size()){
        auto end=s.find('.',start);if(end==std::string::npos)end=s.size();auto len=end-start;
        if(!len||len>63||s[start]=='-'||s[end-1]=='-')return false;
        if(numeric&&len>3)return false;
        int number=0;
        for(size_t i=start;i<end;++i){unsigned char c=s[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))return false;if(numeric)number=number*10+c-'0';}
        if(numeric&&(len>3||number>255))return false;
        ++labels;start=end+1;
    }
    return s.back()!='.'&&(!numeric||labels==4);
}
uint32_t crc32(const void *data, size_t n) {
    uint32_t c = ~0u;
    const auto *p = static_cast<const uint8_t *>(data);
    while (n--) { c ^= *p++; for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
    return ~c;
}
void DisplayPolicy::update(const DisplayConfig &c, int64_t now, bool tv, uint16_t minute) {
    auto idle = std::max<int64_t>(0, now - last_activity_ms) / 1000;
    // off_s == 0 is the user's explicit "screen always on": no screen-off, but also no
    // clock page and no dimming, so the screen really stays as it is. Manual off, zero
    // brightness, the night cap and the fault/critical caps below still apply.
    const bool always_on = c.off_s == 0;
    mode = user_off || c.brightness == 0 || (c.off_s && idle >= c.off_s) ? DisplayMode::Off :
        always_on ? DisplayMode::Active :
        idle >= c.dim_s ? DisplayMode::Dimmed : idle >= c.clock_s ? DisplayMode::Clock : DisplayMode::Active;
    int cap = 100;
    if (c.night_enabled && (!tv || in_night(minute, c.night_start, c.night_end))) cap = c.night_cap;
    if (mode == DisplayMode::Clock) cap = std::min(cap, 25);
    if (mode == DisplayMode::Dimmed) cap = std::min(cap, 8);
    if (critical) cap = std::min(cap, 8);
    if (fault) cap = std::min(cap, 5);
    effective = mode == DisplayMode::Off ? 0 : std::min<int>(c.brightness, cap);
}
bool DisplayPolicy::touch_down(int64_t now) {
    if (mode == DisplayMode::Off || mode == DisplayMode::Dimmed) gesture_blocked = true;
    user_off = false;
    last_activity_ms = now;
    mode = DisplayMode::Active;
    return gesture_blocked;
}
uint32_t Backoff::next_ms(uint32_t random) {
    uint32_t base = std::min<uint32_t>(30000, 1000u << std::min<int>(failures++, 5));
    if (failures > 6) failures = 6;
    return std::min<uint32_t>(30000, base + random % (base / 5 + 1));
}
}
