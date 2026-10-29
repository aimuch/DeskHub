// Failure modes of the sound of the device (docs/design Dev-Sound), written before the code. Only the rules are tested here
// (what a tone is, when one may play); the speaker itself can only be heard.
//
// S1  A tone is a short, quiet, click-free signal: at most half a second, never louder than the cap of its level, silent at
//     its first and last sample, with a fade in and out at each note, no DC offset, and the pitch it is meant to have
//     (confirm rises, failure falls). A rate or a level out of range makes no sound at all instead of a wrong one.
// S2  A command text calls for exactly the right tone: the confirmed switch a confirm; every text that says it did not work,
//     or may not have, a failure; the steps in between and anything unknown, nothing.
// S3  The gate: a disabled sound never plays; no sound in the quiet hours of the night; two sounds are at least 600 ms
//     apart; a clock that runs backwards cannot silence it for good; a test tone ignores the setting and the night but
//     not a flood of presses.
// S4  The stored setting: only "1" is on. Anything else, a missing entry included, is off: the default is silence.
#include "audio_policy.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dh::audio;

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

static int crossings(const std::vector<int16_t> &v, size_t from, size_t to) {
    int n = 0;
    for (size_t i = from + 1; i < to; ++i) if ((v[i - 1] < 0) != (v[i] < 0) && v[i] != 0) ++n;
    return n;
}
static int peak(const std::vector<int16_t> &v) {
    int p = 0;
    for (int16_t s : v) p = std::max(p, std::abs(static_cast<int>(s)));
    return p;
}

static void s1_render() {
    const int rate = 16000;
    CHECK(kPeak <= 32767 * 35 / 100 && kPeak >= 32767 * 20 / 100);   // quiet: between a fifth and a third of full scale
    CHECK(kMaxMs <= 500 && Gate::kGapMs == 600 && Gate::kTestGapMs == 300);   // the numbers of the design
    for (Tone t : {Tone::Confirm, Tone::Fail}) {
        const auto w = render(t, rate, 100);
        CHECK(!w.empty());
        CHECK(w.size() <= static_cast<size_t>(rate * kMaxMs / 1000));          // at most half a second
        CHECK(w.size() >= static_cast<size_t>(rate * 100 / 1000));             // and not a click
        CHECK(peak(w) <= kPeak && peak(w) > kPeak * 8 / 10);                   // quiet, but really there
        CHECK(w.front() == 0 && w.back() == 0);                                // no click at either end
        long sum = 0;
        for (int16_t s : w) sum += s;
        CHECK(std::labs(sum) < static_cast<long>(w.size()) * 40);              // no DC offset
        // A fade at the start: the first 8 ms rise, they never jump to full scale.
        for (size_t i = 1; i < static_cast<size_t>(rate * 8 / 1000); ++i) CHECK(std::abs(static_cast<int>(w[i])) <= std::abs(static_cast<int>(kPeak)) * static_cast<int>(i + 1) / static_cast<int>(rate * 8 / 1000) + 40);
    }
    // Shape: confirm is 70 ms then 110 ms, failure 120 ms then 160 ms; the pitch is read from the zero crossings.
    const auto ok = render(Tone::Confirm, rate, 100);
    CHECK(ok.size() == static_cast<size_t>(rate * 70 / 1000 + rate * 110 / 1000));
    const size_t cut = rate * 70 / 1000;
    const double low = crossings(ok, 0, cut) / 2.0 / 0.070, high = crossings(ok, cut, ok.size()) / 2.0 / 0.110;
    CHECK(low > 800 && low < 960 && high > 1200 && high < 1440 && high > low * 1.3);   // rises
    const auto bad = render(Tone::Fail, rate, 100);
    CHECK(bad.size() == static_cast<size_t>(rate * 120 / 1000 + rate * 160 / 1000));
    const size_t cut2 = rate * 120 / 1000;
    const double first = crossings(bad, 0, cut2) / 2.0 / 0.120, second = crossings(bad, cut2, bad.size()) / 2.0 / 0.160;
    CHECK(first > 400 && first < 480 && second > 300 && second < 360 && second < first * 0.85);   // falls
    // Level: proportional, never above the cap, and never zero for a level that is asked for.
    CHECK(peak(render(Tone::Confirm, rate, 50)) < peak(render(Tone::Confirm, rate, 100)));
    CHECK(peak(render(Tone::Confirm, rate, 50)) > kPeak * 4 / 10 && peak(render(Tone::Confirm, rate, 50)) < kPeak * 6 / 10);
    CHECK(peak(render(Tone::Confirm, rate, 1)) >= 1);
    // Out of range: nothing, not something wrong.
    for (int bad_rate : {0, -1, 7999, 48001, 1000000}) CHECK(render(Tone::Confirm, bad_rate, 100).empty());
    for (int bad_level : {0, -5, 101, 1000}) CHECK(render(Tone::Fail, rate, bad_level).empty());
    // Other legal rates give the same duration.
    for (int r : {8000, 22050, 44100, 48000}) {
        const auto w = render(Tone::Confirm, r, 100);
        CHECK(std::abs(static_cast<long>(w.size()) - static_cast<long>(r * 180 / 1000)) <= 2);
        CHECK(w.front() == 0 && w.back() == 0 && peak(w) <= kPeak);
    }
}

static void s2_commands() {
    CHECK(tone_for_command("已切换（已确认）") == Tone::Confirm);
    for (const char *text : {"命令已发送，状态未确认", "操作已过期，请重新点击", "切换失败，请查看原因", "接收结果未知，请通过Agent查询", "状态未确认，可通过Agent查询结果",
                             "当前连接不可用，请重新点击", "当前不可用或已有操作，请稍后点击"})
        CHECK(tone_for_command(text) == Tone::Fail);
    for (const char *text : {"", "尚无操作", "已接收，等待执行", "正在切换", "正在连接", "连接中断，正在查询结果", "操作已取消", "状态未知", "something else", "已切换", "已确认"})
        CHECK(!tone_for_command(text).has_value());
    // Words inside a longer text are not a match: the whole text has to be one of these.
    CHECK(!tone_for_command("已切换（已确认）。").has_value());
    CHECK(!tone_for_command("x已切换（已确认）").has_value());
    for (const char *text : {"命令已发送，状态未确认", "操作已过期，请重新点击", "切换失败，请查看原因", "接收结果未知，请通过Agent查询", "状态未确认，可通过Agent查询结果",
                             "当前连接不可用，请重新点击", "当前不可用或已有操作，请稍后点击"}) {
        CHECK(!tone_for_command(std::string(text) + "。").has_value());       // a longer text that starts with the words
        CHECK(!tone_for_command(std::string("x") + text).has_value());        // and one that ends with them
        CHECK(!tone_for_command(std::string(text).substr(0, 9)).has_value()); // and half of them
    }
}

static void s3_gate() {
    Gate g;
    CHECK(!g.allow(1000, false, false));                           // disabled
    CHECK(!g.allow(2000, true, true));                             // night
    CHECK(g.allow(3000, true, false));                             // first sound
    CHECK(!g.allow(3599, true, false));                            // 599 ms later
    CHECK(g.allow(3600, true, false));                             // 600 ms later
    CHECK(!g.allow(3700, true, false));
    // A refused sound does not push the next one away.
    Gate h;
    CHECK(h.allow(10000, true, false));
    CHECK(!h.allow(10300, true, false));
    CHECK(!h.allow(10599, true, false));
    CHECK(h.allow(10600, true, false));
    // A clock that goes back: the next sound is allowed, and the gap counts from there.
    Gate b;
    CHECK(b.allow(50000, true, false));
    CHECK(b.allow(100, true, false));
    CHECK(!b.allow(300, true, false));
    CHECK(b.allow(700, true, false));
    // The gate does not remember a refusal for a disabled sound.
    Gate d;
    CHECK(!d.allow(5000, false, false));
    CHECK(d.allow(5001, true, false));
    // The test tone ignores the setting and the night; a flood of presses is still limited to one in 300 ms.
    Gate t;
    CHECK(t.allow_test(1000));
    CHECK(!t.allow_test(1299));
    CHECK(t.allow_test(1300));
    Gate u;
    CHECK(u.allow_test(0));
    CHECK(!u.allow(300, true, false));                              // a test tone counts as a sound for the gap
    CHECK(u.allow(600, true, false));
}

static void s4_setting() {
    CHECK(parse_enabled("1"));
    for (const char *text : {"", "0", "true", "on", "2", "11", "01", " 1", "1 ", "yes"}) CHECK(!parse_enabled(text));
    CHECK(serialize_enabled(true) == "1" && serialize_enabled(false) == "0");
    CHECK(parse_enabled(serialize_enabled(true)) && !parse_enabled(serialize_enabled(false)));
}

int main() {
    s1_render();
    s2_commands();
    s3_gate();
    s4_setting();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("audio_test: all checks passed");
    return 0;
}
