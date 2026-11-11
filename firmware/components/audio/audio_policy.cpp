#include "audio_policy.hpp"
#include <algorithm>
#include <cmath>

namespace dh::audio {
namespace {
struct Note {
    int hz, ms;
};
constexpr Note kConfirm[2] = {{880, 70}, {1320, 110}};
constexpr Note kFail[2] = {{440, 120}, {330, 160}};
constexpr Note kAlarm[3] = {{1320, 90}, {1320, 90}, {1320, 90}};
constexpr int kAlarmGapMs = 60;
constexpr int kFadeMs = 8;
constexpr double kPi = 3.14159265358979323846;
}  // namespace

std::vector<int16_t> render(Tone tone, int rate, int level_percent) {
    if (rate < kMinRate || rate > kMaxRate || level_percent < 1 || level_percent > 100) return {};
    const Note *notes = tone == Tone::Confirm ? kConfirm : tone == Tone::Fail ? kFail : kAlarm;
    const int count_notes = tone == Tone::Alarm ? 3 : 2;
    const double amplitude = static_cast<double>(kPeak) * level_percent / 100.0;
    const int fade = std::max(1, rate * kFadeMs / 1000);
    std::vector<int16_t> out;
    for (int n = 0; n < count_notes; ++n) {
        if (tone == Tone::Alarm && n > 0) out.insert(out.end(), static_cast<size_t>(rate * kAlarmGapMs / 1000), 0);   // the silence between beeps
        const int count = rate * notes[n].ms / 1000;
        for (int i = 0; i < count; ++i) {
            const double gain = std::min({1.0, static_cast<double>(i) / fade, static_cast<double>(count - 1 - i) / fade});   // zero at both ends of a note
            const double value = std::sin(2.0 * kPi * notes[n].hz * i / rate) * amplitude * gain;
            out.push_back(static_cast<int16_t>(std::lround(value)));
        }
    }
    if (out.size() > static_cast<size_t>(rate) * kMaxMs / 1000) out.clear();   // cannot happen with the notes above; a guard for the next change
    return out;
}

std::optional<Tone> tone_for_command(const std::string &text) {
    if (text == "已切换（已确认）") return Tone::Confirm;
    static const char *const failures[] = {"命令已发送，状态未确认", "操作已过期，请重新点击", "切换失败，请查看原因", "接收结果未知，请通过Agent查询",
                                           "状态未确认，可通过Agent查询结果", "当前连接不可用，请重新点击", "当前不可用或已有操作，请稍后点击"};
    for (const char *f : failures) {
        if (text == f) return Tone::Fail;
    }
    return std::nullopt;
}

bool parse_enabled(const std::string &text) { return text == "1"; }
std::string serialize_enabled(bool on) { return on ? "1" : "0"; }

int codec_volume(int level) {
    static constexpr int kCodec[kVolumeLevels] = {50, 70, 90};
    return level >= 0 && level < kVolumeLevels ? kCodec[level] : kCodec[0];
}
int parse_volume(const std::string &text) {
    return text.size() == 1 && text[0] >= '0' && text[0] < '0' + kVolumeLevels ? text[0] - '0' : 0;
}
std::string serialize_volume(int level) { return std::string(1, static_cast<char>('0' + (level >= 0 && level < kVolumeLevels ? level : 0))); }

bool Gate::take(int64_t now_ms, int64_t gap_ms) {
    if (!any_ || now_ms < last_ms_ || now_ms - last_ms_ >= gap_ms) {   // a clock that went back does not silence the device for good
        any_ = true;
        last_ms_ = now_ms;
        return true;
    }
    return false;
}
bool Gate::allow(int64_t now_ms, bool enabled, bool quiet_hours) {
    if (!enabled || quiet_hours) return false;
    return take(now_ms, kGapMs);
}
bool Gate::allow_test(int64_t now_ms) { return take(now_ms, kTestGapMs); }
}  // namespace dh::audio
