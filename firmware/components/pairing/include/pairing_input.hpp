#pragma once
// What the setup page may hand to the device. The same limits as the USB provisioning handler in main/app_main.cpp,
// so a device paired over the hotspot is held to the rules of one paired over USB.
#include <cstdint>
#include <string>

namespace dh::pairing {
bool valid_ssid(const std::string &);           // 1 to 32 bytes, no NUL
bool valid_wifi_password(const std::string &);  // empty (open network) or 8 to 63 bytes, no NUL
bool valid_identifier(const std::string &);     // an Agent or device id: 1 to 96 of A-Z a-z 0-9 _ - . :, starting with a letter or digit
bool valid_token(const std::string &);          // 32 to 256 printable ASCII characters, no space
bool valid_ca_pem(const std::string &);         // at most 8 KiB and a CERTIFICATE block
bool valid_setup_code(const std::string &);     // the code on the device screen: six digits
// Strict `YYYY-MM-DDTHH:MM:SSZ`, a real calendar date in 2024..2099. The phone's clock, like the PC's over USB.
bool parse_utc(const std::string &, int64_t &epoch_seconds);
// `DeskHub-C598` from `deskhub-44bd8d60c598`: the last four hex digits, upper case. Never empty, always valid as an SSID.
std::string hotspot_name(const std::string &device_id);
// Seconds left, rounded up, never negative and never above the window; "04:32".
int remaining_seconds(int64_t now_ms, int64_t deadline_ms, int64_t window_ms);
std::string format_countdown(int seconds);
}  // namespace dh::pairing
