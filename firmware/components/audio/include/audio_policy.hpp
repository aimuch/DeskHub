#pragma once
// The rules of the sound of the device (docs/design Dev-Sound): what a tone is, which command text calls for which tone,
// and when a sound may play. No hardware here; the speaker is in audio.cpp. Tested on the host (firmware/tests/audio_test.cpp).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dh::audio {
enum class Tone { Confirm, Fail, Alarm };

constexpr int kMinRate = 8000, kMaxRate = 48000;   // the rates a codec of this kind is run at
constexpr int kMaxMs = 500;                         // no tone is longer than this
constexpr int kPeak = 9830;                         // 30 % of full scale: the loudest sample of a tone at level 100

// Mono signed 16-bit samples. Confirm: two notes, rising (880 Hz for 70 ms, 1320 Hz for 110 ms). Fail: two notes, falling
// (440 Hz for 120 ms, 330 Hz for 160 ms). Alarm (a to-do is due, 13.14): three beeps of 1320 Hz for 90 ms, 60 ms apart. Each note fades in and out over 8 ms, so there is no click. Empty for a rate or a
// level (1 to 100) out of range: no sound at all instead of a wrong one.
std::vector<int16_t> render(Tone, int rate, int level_percent);

// The tone a text of the command line of the device calls for. Only whole texts match. Steps in between and unknown texts: none.
std::optional<Tone> tone_for_command(const std::string &);

// The stored setting: only "1" is on; anything else, and a missing entry, is off.
bool parse_enabled(const std::string &);
std::string serialize_enabled(bool);

// The volume: three levels, 0 low (the default, the fixed volume of 0.3.7), 1 medium, 2 high. The codec's own volume for a
// level (its curve is 1 to 100 for -49.5 to 0 dB, so the levels are 10 dB apart); a level out of range plays at low.
constexpr int kVolumeLevels = 3;
int codec_volume(int level);
// The stored level: only "0", "1" and "2"; anything else, a missing entry included, is low. A level out of range is stored as low.
int parse_volume(const std::string &);
std::string serialize_volume(int level);

// When a sound may play. Not when it is switched off, not in the quiet hours of the night, and not twice within 600 ms.
// A test tone (the person pressed "listen") ignores the setting and the night, and is limited to one in 300 ms.
class Gate {
public:
    static constexpr int64_t kGapMs = 600, kTestGapMs = 300;
    bool allow(int64_t now_ms, bool enabled, bool quiet_hours);
    bool allow_test(int64_t now_ms);

private:
    bool take(int64_t now_ms, int64_t gap_ms);
    int64_t last_ms_ = 0;
    bool any_ = false;
};
}  // namespace dh::audio
