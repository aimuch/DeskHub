#include "pairing_glue.hpp"
#include <cctype>

namespace dh::pairing {
namespace {
struct Member {
    const char *name;
    size_t limit;
};
constexpr Member kMembers[5] = {{"code", 6}, {"ssid", 32}, {"password", 63}, {"package", 3300}, {"utc", 20}};

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ':'; }
bool all_alnum(const std::string &s) {
    if (s.empty()) return false;
    for (unsigned char c : s) if (!std::isalnum(c)) return false;
    return true;
}
}  // namespace

const char *form_error_name(FormError e) {
    switch (e) {
    case FormError::None: return "none";
    case FormError::TooLarge: return "too_large";
    case FormError::Syntax: return "syntax";
    case FormError::Duplicate: return "duplicate";
    case FormError::Unknown: return "unknown";
    case FormError::Missing: return "missing";
    case FormError::Control: return "control";
    }
    return "unknown";
}

FormError parse_form(const std::string &body, FormFields &out) {
    auto fail = [&](FormError e) {
        out = FormFields{};  // nothing of a refused form stays
        return e;
    };
    if (body.size() > kMaxFormBytes) return fail(FormError::TooLarge);
    if (body.empty()) return fail(FormError::Missing);
    std::string *slot[5] = {&out.code, &out.ssid, &out.wifi_password, &out.package, &out.utc};
    bool seen[5] = {false, false, false, false, false};
    size_t at = 0;
    for (;;) {
        size_t end = body.find('&', at);
        if (end == std::string::npos) end = body.size();
        const size_t eq = body.find('=', at);
        // An empty pair (a leading, doubled or trailing '&'), a pair without '=' and a pair without a name end here.
        if (eq == std::string::npos || eq >= end || eq == at) return fail(FormError::Syntax);
        const std::string key = body.substr(at, eq - at);
        std::string value;
        for (size_t i = eq + 1; i < end; ++i) {
            const char c = body[i];
            if (c == '+') {
                value += ' ';
            } else if (c == '%') {
                if (i + 2 >= end) return fail(FormError::Syntax);  // '%' needs two more characters inside this member
                const int hi = hex_value(body[i + 1]), lo = hex_value(body[i + 2]);
                if (hi < 0 || lo < 0) return fail(FormError::Syntax);
                value += static_cast<char>(hi * 16 + lo);
                i += 2;
            } else {
                value += c;
            }
        }
        int index = -1;
        for (int m = 0; m < 5; ++m) if (key == kMembers[m].name) index = m;
        if (index < 0) return fail(FormError::Unknown);
        if (seen[index]) return fail(FormError::Duplicate);
        seen[index] = true;
        if (index == 3) {  // a paste carries a line feed at its ends; nowhere else is white space allowed in a package
            size_t first = 0, last = value.size();
            while (first < last && is_space(value[first])) ++first;
            while (last > first && is_space(value[last - 1])) --last;
            value = value.substr(first, last - first);
        }
        for (unsigned char c : value) if (c < 0x20 || c == 0x7f) return fail(FormError::Control);
        if (value.size() > kMembers[index].limit) return fail(FormError::TooLarge);
        *slot[index] = value;
        if (end == body.size()) break;
        at = end + 1;
    }
    for (bool s : seen) if (!s) return fail(FormError::Missing);
    return FormError::None;
}

bool auto_open_hotspot(bool has_wifi, bool already_tried, int64_t now_ms, int64_t grace_ms) {
    return !has_wifi && !already_tried && now_ms >= grace_ms;
}

Tls tls_from_flags(bool connected, int flags) {
    if (flags == 0) return connected ? Tls::Ok : Tls::Unreachable;
    if (flags & 0x08) return Tls::ChainInvalid;       // not trusted: the rest is noise
    if (flags & 0x01) return Tls::CertificateExpired;
    if (flags & 0x04) return Tls::HostnameMismatch;
    return Tls::ChainInvalid;                          // revoked or anything this code does not know
}

bool ipv4_in_subnet(uint32_t addr, uint32_t network, uint32_t mask) {
    if (network == 0 || mask == 0) return false;
    return (addr & mask) == (network & mask);
}

std::string redeem_url(const Package &p) { return p.url + "/api/v1/pairing/redeem"; }

std::string redeem_request_body(const Package &p, const std::string &device_id) {
    if (!all_alnum(p.grant_id) || !all_alnum(p.code) || device_id.empty()) return std::string();
    for (char c : device_id) if (!name_char(c)) return std::string();
    return "{\"grant_id\":\"" + p.grant_id + "\",\"code\":\"" + p.code + "\",\"device_id\":\"" + device_id + "\"}";
}

const char *setup_page_html() {
    return "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>DeskHub 配网</title><style>body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:20px;background:#0b1d1a;color:#e6f7f2}"
           "h1{font-size:22px}label{display:block;margin:14px 0 4px;color:#8aa8a1}input,textarea{width:100%;box-sizing:border-box;padding:10px;font:inherit;"
           "border-radius:8px;border:1px solid #1b3d37;background:#03100e;color:inherit}textarea{height:7em}button{margin-top:18px;width:100%;padding:12px;font:inherit;"
           "font-weight:700;border:0;border-radius:8px;background:#5ee6b8;color:#04231b}p{color:#8aa8a1;font-size:14px}</style></head><body>"
           "<h1>DeskHub 配网</h1><p>在设备屏幕上找配网码。下面三项和电脑上管理台里的配对包，一起填进来。</p>"
           "<form method=\"post\" action=\"/setup\" autocomplete=\"off\">"
           "<label for=\"code\">配网码（设备屏幕上的 6 位数字）</label><input id=\"code\" name=\"code\" inputmode=\"numeric\" maxlength=\"6\" autocomplete=\"off\" required>"
           "<label for=\"ssid\">Wi-Fi 名称（只认 2.4 GHz）</label><input id=\"ssid\" name=\"ssid\" maxlength=\"32\" autocomplete=\"off\" required>"
           "<label for=\"password\">Wi-Fi 密码</label><input id=\"password\" name=\"password\" type=\"password\" maxlength=\"63\" autocomplete=\"off\">"
           "<label for=\"package\">配对包（管理台生成，以 DHP2. 开头）</label><textarea id=\"package\" name=\"package\" autocomplete=\"off\" spellcheck=\"false\" required></textarea>"
           "<input type=\"hidden\" id=\"utc\" name=\"utc\"><button type=\"submit\">配对</button></form>"
           "<p>提交后设备会去连你的 Wi-Fi，手机可能会断开热点，这是正常的。结果请看设备屏幕。</p>"
           "<script>document.getElementById('utc').value=new Date().toISOString().slice(0,19)+'Z'</script></body></html>";
}

std::string setup_result_html(bool accepted) {
    const char *head = "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<title>DeskHub 配网</title></head><body style=\"font:16px/1.5 system-ui,sans-serif;padding:20px\">";
    return std::string(head) + (accepted ? "<h1>已收到</h1><p>设备正在连接 Wi-Fi，并向电脑配对。手机可能会断开热点，这是正常的。请看设备屏幕。</p></body></html>"
                                         : "<h1>没有接受</h1><p>请检查配网码和各项内容，然后重试。</p></body></html>");
}
}  // namespace dh::pairing
