#pragma once
// Language packs of the device UI. Two compiled-in languages (docs/rules.md R9): Chinese, whose every glyph must
// be in the shipped font subset, and English, which is ASCII only. Pure C++.
#include <cstdint>
#include <string>

namespace dh::ui {
enum class Lang { ZhCN, EnUS };
Lang lang_from_name(const std::string &name);  // anything unknown is ZhCN
const char *lang_name(Lang);                    // "zh-CN", "en-US"

enum class Str : uint16_t {
#define S(name, zh, en) name,
#include "ui_strings.def"
#undef S
    Count
};
const char *tr(Lang, Str);
const char *weekday_name(Lang, int tm_wday);  // zh: one character (日一二三四五六), en: Sun..Sat
const char *month_name(Lang, int tm_mon);     // en: Jan..Dec; zh: "" (months are numbers there)

// The baseline core and Agent client put Chinese sentences into the snapshot. In English the known ones are
// translated; an error code like "CONFIG_STORAGE_FAULT：请通过USB恢复" is shown as its code; anything else passes through.
std::string dynamic_text(Lang, const std::string &);
}  // namespace dh::ui
