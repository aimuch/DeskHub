#include "ui_text.hpp"
#include <cstddef>
#include <cstring>

namespace dh::ui {
namespace {
const char *const kZh[] = {
#define S(name, zh, en) zh,
#include "ui_strings.def"
#undef S
};
const char *const kEn[] = {
#define S(name, zh, en) en,
#include "ui_strings.def"
#undef S
};
static_assert(sizeof(kZh) / sizeof(kZh[0]) == static_cast<size_t>(Str::Count), "zh table size");
static_assert(sizeof(kEn) / sizeof(kEn[0]) == static_cast<size_t>(Str::Count), "en table size");

const char *const kWeekZh[7] = {"日", "一", "二", "三", "四", "五", "六"};
const char *const kWeekEn[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *const kMonthEn[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct Pair { const char *zh, *en; };
// Sentences the baseline core and Agent client put into the snapshot.
const Pair kKnown[] = {
    {"尚无操作", "No action yet"},
    {"已接收，等待执行", "Received, waiting to run"},
    {"当前连接不可用，请重新点击", "Connection unavailable. Tap again."},
    {"接收结果未知，请通过Agent查询", "Result unknown. Check it on the Agent."},
    {"操作已过期，请重新点击", "Action expired. Tap again."},
    {"状态未确认，可通过Agent查询结果", "Not confirmed. Check the result on the Agent."},
    {"连接中断，正在查询结果", "Connection lost. Checking the result."},
    {"当前不可用或已有操作，请稍后点击", "Busy or unavailable. Try again later."},
    {"Agent不可达或认证失败", "Agent unreachable or authentication failed"},
    {"等待校时", "Waiting for time sync"},
    {"已连接", "Connected"},
    {"正在连接", "Connecting"},
    {"未配置", "Not set up"},
    {"未配对", "Not paired"},
    {"管理台不可用或未授权", "Console unavailable or not authorised"},
    {"管理台已连接", "Console connected"},
    {"尚未连接管理台", "Console not connected yet"},
    {"不可用", "Unavailable"},
};
// "CODE：说明" — the full-width colon (U+FF1A) after an upper-case code.
const char kFullColon[] = "\xEF\xBC\x9A";
}  // namespace

Lang lang_from_name(const std::string &name) { return name == "en-US" ? Lang::EnUS : Lang::ZhCN; }
const char *lang_name(Lang l) { return l == Lang::EnUS ? "en-US" : "zh-CN"; }

const char *tr(Lang l, Str id) {
    const auto i = static_cast<size_t>(id);
    if (i >= static_cast<size_t>(Str::Count)) return "";
    return l == Lang::EnUS ? kEn[i] : kZh[i];
}
const char *weekday_name(Lang l, int d) {
    if (d < 0 || d > 6) return "";
    return l == Lang::EnUS ? kWeekEn[d] : kWeekZh[d];
}
const char *month_name(Lang l, int m) {
    if (l != Lang::EnUS || m < 0 || m > 11) return "";
    return kMonthEn[m];
}

std::string dynamic_text(Lang l, const std::string &text) {
    if (l != Lang::EnUS || text.empty()) return text;
    for (const auto &p : kKnown)
        if (text == p.zh) return p.en;
    const auto at = text.find(kFullColon);
    if (at != std::string::npos && at > 0) {
        bool code = true;
        for (size_t i = 0; i < at; ++i) {
            const char c = text[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) code = false;
        }
        if (code) return text.substr(0, at);
    }
    return text;
}
}  // namespace dh::ui
