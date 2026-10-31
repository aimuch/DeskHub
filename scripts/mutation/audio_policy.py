#!/usr/bin/env python3
"""Mutation check for the rules of the sound (components/audio/audio_policy.cpp): break the code on purpose, audio_test must go red.

Usage: scripts/mutation/audio_policy.py [name-part ...]
Works on a copy in a temporary directory. The working tree is never touched.
Exit 0 only if the baseline is green and every mutation is caught.
"""
from _engine import firmware_main

A = "components/audio/audio_policy.cpp"
H = "components/audio/include/audio_policy.hpp"
M = [
    ("a rate that is too low is accepted", A, "if (rate < kMinRate || rate > kMaxRate || level_percent < 1 || level_percent > 100) return {};", "if (rate < 1 || rate > kMaxRate || level_percent < 1 || level_percent > 100) return {};"),
    ("a rate that is too high is accepted", A, "rate > kMaxRate ||", "rate > 400000 ||"),
    ("level zero is accepted", A, "level_percent < 1 ||", "level_percent < 0 ||"),
    ("a level above 100 is accepted", A, "level_percent > 100) return {};", "level_percent > 1000) return {};"),
    ("the notes do not fade", A, "const double gain = std::min({1.0, static_cast<double>(i) / fade, static_cast<double>(count - 1 - i) / fade});", "const double gain = 1.0 + 0 * fade;"),
    ("the notes do not fade out", A, "static_cast<double>(i) / fade, static_cast<double>(count - 1 - i) / fade", "static_cast<double>(i) / fade, 1.0"),
    ("the fade is half as long", A, "constexpr int kFadeMs = 8;", "constexpr int kFadeMs = 1;"),
    ("the confirm falls", A, "constexpr Note kConfirm[2] = {{880, 70}, {1320, 110}};", "constexpr Note kConfirm[2] = {{1320, 70}, {880, 110}};"),
    ("the failure rises", A, "constexpr Note kFail[2] = {{440, 120}, {330, 160}};", "constexpr Note kFail[2] = {{330, 120}, {440, 160}};"),
    ("a confirm of another length", A, "{880, 70}, {1320, 110}", "{880, 70}, {1320, 130}"),
    ("a failure that is too long", A, "{440, 120}, {330, 160}", "{440, 120}, {330, 460}"),
    ("the level is not applied", A, "const double amplitude = static_cast<double>(kPeak) * level_percent / 100.0;", "const double amplitude = static_cast<double>(kPeak);"),
    ("the cap is louder", H, "constexpr int kPeak = 9830;", "constexpr int kPeak = 20000;"),
    ("the half second is a second", H, "constexpr int kMaxMs = 500;", "constexpr int kMaxMs = 1000;"),
    ("the confirmed switch is silent", A, 'if (text == "已切换（已确认）") return Tone::Confirm;', 'if (text == "已切换（已确认）") return std::nullopt;'),
    ("the confirmed switch is a failure", A, 'if (text == "已切换（已确认）") return Tone::Confirm;', 'if (text == "已切换（已确认）") return Tone::Fail;'),
    ("an unconfirmed command is silent", A, '"命令已发送，状态未确认", ', ""),
    ("an expired command is silent", A, '"操作已过期，请重新点击", ', ""),
    ("a failed switch is silent", A, '"切换失败，请查看原因", ', ""),
    ("an unknown result is silent", A, '"接收结果未知，请通过Agent查询",', ""),
    ("a text that only starts with the words matches", A, "if (text == f) return Tone::Fail;", "if (text.rfind(f, 0) == 0) return Tone::Fail;"),
    ("a step in between plays", A, "    return std::nullopt;\n}\n\nbool parse_enabled", "    if (text == \"正在切换\") return Tone::Confirm;\n    return std::nullopt;\n}\n\nbool parse_enabled"),
    ("anything stored is on", A, 'bool parse_enabled(const std::string &text) { return text == "1"; }', "bool parse_enabled(const std::string &text) { return !text.empty(); }"),
    ("off is stored as nothing", A, 'return on ? "1" : "0"; }', 'return on ? "1" : ""; }'),
    ("a disabled sound plays", A, "if (!enabled || quiet_hours) return false;", "if (quiet_hours || (enabled && false)) return false;"),
    ("the night is not quiet", A, "if (!enabled || quiet_hours) return false;", "if (!enabled || (quiet_hours && false)) return false;"),
    ("the gap is shorter", H, "kGapMs = 600", "kGapMs = 500"),
    ("the gap is longer", H, "kGapMs = 600", "kGapMs = 700"),
    ("the test gap is shorter", H, "kTestGapMs = 300", "kTestGapMs = 200"),
    ("the test tone ignores the gap", A, "bool Gate::allow_test(int64_t now_ms) { return take(now_ms, kTestGapMs); }", "bool Gate::allow_test(int64_t now_ms) { (void)now_ms; return true; }"),
    ("a clock that went back silences the device", A, "if (!any_ || now_ms < last_ms_ || now_ms - last_ms_ >= gap_ms) {", "if (!any_ || now_ms - last_ms_ >= gap_ms) {"),
    ("a refused sound pushes the next one away", A, "        return true;\n    }\n    return false;\n}\nbool Gate::allow", "        return true;\n    }\n    last_ms_ = now_ms;\n    return false;\n}\nbool Gate::allow"),
    ("the first sound waits", A, "if (!any_ || now_ms < last_ms_", "if (now_ms < last_ms_"),
    # the volume
    ("low is louder than in 0.3.7", A, "kCodec[kVolumeLevels] = {50, 70, 90};", "kCodec[kVolumeLevels] = {60, 70, 90};"),
    ("two levels sound the same", A, "kCodec[kVolumeLevels] = {50, 70, 90};", "kCodec[kVolumeLevels] = {50, 50, 90};"),
    ("high is the codec's full volume", A, "kCodec[kVolumeLevels] = {50, 70, 90};", "kCodec[kVolumeLevels] = {50, 70, 100};"),
    ("a level out of range plays loud", A, "level < kVolumeLevels ? kCodec[level] : kCodec[0];", "level < kVolumeLevels ? kCodec[level] : kCodec[kVolumeLevels - 1];"),
    ("a negative level reads outside the table", A, "return level >= 0 && level < kVolumeLevels ? kCodec[level]", "return level < kVolumeLevels ? kCodec[level]"),
    ("a padded stored level is read", A, "return text.size() == 1 && text[0] >= '0'", "return !text.empty() && text[0] >= '0'"),
    ("a stored level that does not exist is read", A, "text[0] < '0' + kVolumeLevels ? text[0] - '0' : 0;", "text[0] <= '9' ? text[0] - '0' : 0;"),
    ("a broken stored level is high", A, "text[0] < '0' + kVolumeLevels ? text[0] - '0' : 0;", "text[0] < '0' + kVolumeLevels ? text[0] - '0' : 2;"),
    ("a level out of range is stored", A, "(level >= 0 && level < kVolumeLevels ? level : 0)", "level"),
]

if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "audio_test"))
