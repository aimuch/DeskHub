#pragma once
// Where the device keeps the charge protection settings (serialize/parse_settings in charge_policy.hpp): one small NVS
// entry in its own namespace, so a bad value here can never touch the pairing, Wi-Fi, display or appearance settings.
#include <string>

namespace dh::charge {
// "" when nothing is stored or the entry cannot be read. The caller parses it and falls back to the default (off).
std::string store_load();
// true only when the value is durably written.
bool store_save(const std::string &);
}  // namespace dh::charge
