#pragma once
#include "update_policy.hpp"
#include <string>
namespace dh {
// Reads the boot state on the caller's (internal) stack: whether the running firmware still waits for its self-test is known
// before anything else asks, even when the management task cannot start (internal memory is short once Bluetooth is up).
void management_start();
// The boot self-test (60 s after the first start of a new firmware), from the housekeeping task. Its flash writes run on the
// flash worker. It must not depend on the management task: without it, a new firmware was never confirmed (0.3.27, 0.3.28).
void management_tick();
bool boot_pending();                     // the running firmware has not passed its self-test yet
// The firmware update over Bluetooth (docs/desktop-app-design.md 13.11) uses the same trust and rules as the Agent's:
bool update_allowed(std::string &why);   // no other update, no self-test pending, journal and storage healthy, USB power steady
bool verify_update_envelope(const std::string &json, UpdateManifest &, std::string &error);   // update_key's signature, the 8 fields
void external_update(bool active);       // a Bluetooth update runs: the Agent's install waits
bool update_public_key_valid(const std::string &pem);
std::string update_key_fingerprint(const std::string &pem);
}
