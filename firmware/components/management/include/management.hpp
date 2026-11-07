#pragma once
#include "update_policy.hpp"
#include <string>
namespace dh {
void management_start();
// The firmware update over Bluetooth (docs/desktop-app-design.md 13.11) uses the same trust and rules as the Agent's:
bool update_allowed(std::string &why);   // no other update, no self-test pending, journal and storage healthy, USB power steady
bool verify_update_envelope(const std::string &json, UpdateManifest &, std::string &error);   // update_key's signature, the 8 fields
void external_update(bool active);       // a Bluetooth update runs: the Agent's install waits
bool update_public_key_valid(const std::string &pem);
std::string update_key_fingerprint(const std::string &pem);
}
