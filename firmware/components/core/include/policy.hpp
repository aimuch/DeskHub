#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <array>

namespace dh {
enum class DisplayMode { Active, Clock, Dimmed, Off };
struct LayoutConfig {
    std::string preset = "desk";
    std::string theme = "mint";
    std::array<std::string,4> action_ids{};
    uint8_t action_count = 0;
    bool show_seconds = false;
    bool show_cpu = false;
};
bool valid_layout(const LayoutConfig &);
struct DisplayConfig {
    uint32_t schema = 1;
    uint32_t revision = 0;
    uint8_t brightness = 50;
    uint16_t rotation = 0;
    uint32_t clock_s = 60;
    uint32_t dim_s = 120;
    uint32_t off_s = 300; // 0 requires explicit never-off setting.
    bool night_enabled = true;
    uint16_t night_start = 22 * 60;
    uint16_t night_end = 7 * 60;
    uint8_t night_cap = 15;
    bool protection = true;
    int16_t tz_minutes = 480;   // east of UTC; a fixed offset (no daylight saving). Local only: never sent to the Agent.
    LayoutConfig layout;
    uint32_t managed_revision = 0;
    std::string managed_agent_id;
};
bool valid_config(const DisplayConfig &c);
// The time zones the device offers: the offsets in use, from UTC-12 to UTC+14 (docs/design Dev-Time).
constexpr int kDefaultTzMinutes = 480;
bool valid_tz(int minutes);
int tz_step(int minutes, int delta);        // the next offset in the table; stops at the ends; a value that is no zone gives the default
std::string tz_env(int minutes);            // POSIX TZ, where the sign is the opposite: 480 is "UTC-08:00"
bool in_night(uint16_t minute, uint16_t start, uint16_t end);
bool valid_ntp_server(const std::string &);
uint32_t crc32(const void *data, size_t len);
struct DisplayPolicy {
    DisplayMode mode = DisplayMode::Active;
    int64_t last_activity_ms = 0;
    bool user_off = false;
    bool fault = false;
    bool critical = false;
    bool gesture_blocked = false;
    int effective = 50;
    void update(const DisplayConfig &, int64_t now_ms, bool time_valid, uint16_t minute);
    // Returns true when the entire gesture must be consumed.
    bool touch_down(int64_t now_ms);
    void touch_up() { gesture_blocked = false; }
};
struct Backoff {
    uint8_t failures = 0;
    uint32_t next_ms(uint32_t random);
    void reset() { failures = 0; }
};
}
