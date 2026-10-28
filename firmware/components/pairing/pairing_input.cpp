#include "pairing_input.hpp"
#include <algorithm>
#include <cstdio>

namespace dh::pairing {
namespace {
bool has_nul(const std::string &s) { return s.find('\0') != std::string::npos; }
bool alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int days_in(int y, int m) {
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : d[m - 1];
}
// Days since 1970-01-01 (Howard Hinnant's algorithm).
int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
}  // namespace

bool valid_ssid(const std::string &s) { return !s.empty() && s.size() <= 32 && !has_nul(s); }
bool valid_wifi_password(const std::string &s) { return (s.empty() || (s.size() >= 8 && s.size() <= 63)) && !has_nul(s); }
bool valid_identifier(const std::string &s) {
    if (s.empty() || s.size() > 96 || !alnum(s.front())) return false;
    for (char c : s) if (!alnum(c) && c != '_' && c != '-' && c != '.' && c != ':') return false;
    return true;
}
bool valid_token(const std::string &s) {
    if (s.size() < 32 || s.size() > 256) return false;
    for (unsigned char c : s) if (c < 33 || c > 126) return false;
    return true;
}
bool valid_ca_pem(const std::string &s) {
    return s.size() <= 8192 && s.find("-----BEGIN CERTIFICATE-----") != std::string::npos && s.find("PRIVATE KEY") == std::string::npos;
}
bool valid_setup_code(const std::string &s) {
    if (s.size() != 6) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

bool parse_utc(const std::string &s, int64_t &epoch) {
    // YYYY-MM-DDTHH:MM:SSZ
    if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') return false;
    auto number = [&](size_t at, size_t n, int &out) {
        out = 0;
        for (size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            out = out * 10 + (s[i] - '0');
        }
        return true;
    };
    int y, mo, d, h, mi, se;
    if (!number(0, 4, y) || !number(5, 2, mo) || !number(8, 2, d) || !number(11, 2, h) || !number(14, 2, mi) || !number(17, 2, se)) return false;
    if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > days_in(y, mo) || h > 23 || mi > 59 || se > 59) return false;
    epoch = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    return true;
}

std::string hotspot_name(const std::string &device_id) {
    std::string tail = device_id.size() >= 4 ? device_id.substr(device_id.size() - 4) : std::string(4 - device_id.size(), '0') + device_id;
    for (char &c : tail) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
        else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) c = '0';
    }
    return "DeskHub-" + tail;
}

int remaining_seconds(int64_t now_ms, int64_t deadline_ms, int64_t window_ms) {
    int64_t left = deadline_ms - now_ms;
    left = std::max<int64_t>(0, std::min<int64_t>(left, window_ms));
    return static_cast<int>((left + 999) / 1000);
}
std::string format_countdown(int seconds) {
    seconds = std::max(0, std::min(seconds, 99 * 60 + 59));
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", seconds / 60, seconds % 60);
    return buf;
}
}  // namespace dh::pairing
