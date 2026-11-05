#pragma once
#include <cstdint>
#include <string>

namespace dh {
void ui_start();
bool ui_alive();
bool ui_configuration_applied(uint32_t local_revision);

// The pairing page (docs/design Dev-Pairing). The pairing backend (docs/rules.md R7) is not in this firmware yet;
// nothing calls these today, so the device never shows the page. The page itself is built and tested.
struct PairingInfo {
    std::string code;     // 4..8 characters of 0-9 A-Z (the device makes six digits); anything else is drawn as "------", never as garbage
    std::string hotspot;  // the SSID the phone joins
    std::string hotspot_password;  // its WPA2 password, shown on the screen: 8 to 12 printable characters without a space (the device makes twelve), else dashes; a long one would be cut off, and a cut-off password is wrong
    int remaining_s = 0;  // counted down on the screen; the backend decides when the page goes away
    int total_s = 300;
};
// May be called from any task. `on_cancel` runs in the UI task when the user taps "cancel".
void ui_pairing_show(const PairingInfo &, void (*on_cancel)());
void ui_pairing_hide();
// `request` runs in the UI task when the person taps "pair" on the Connect page (docs/rules.md R7). It must not block.
void ui_set_pairing_request(void (*request)());
}
