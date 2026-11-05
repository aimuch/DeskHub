#pragma once
// Where the device keeps the Appearance text (ui_model.hpp serialize/parse_appearance). One small NVS entry in its
// own namespace, so a bad value here can never touch the pairing, Wi-Fi or display configuration.
#include <string>

namespace dh::ui {
// "" when nothing is stored or the entry cannot be read. The caller parses and falls back to the default.
std::string store_load();
// true only when the value is durably written.
bool store_save(const std::string &);
// Factory reset: forget it. true when nothing is stored afterwards (also when nothing was stored before).
bool store_clear();
}  // namespace dh::ui
