#pragma once
// Hotspot pairing on the device (docs/pairing-design.md sections 12 and 13): the glue between the pure session
// (components/pairing) and the hardware: the Wi-Fi hotspot, the setup page, the TLS redemption, the save path, the button.
// Not verified on a device: see the list at the end of section 13.
namespace dh::pairing_device {
struct Status {
    bool open = false;
    const char *phase = "closed";  // closed, open, joining, redeeming, save_failed, done
    const char *why = "none";      // the last reason a session ended or an attempt failed; a name, never a secret
};
// Starts the task that watches the button and the pairing state. Call after network_start().
void start();
// For the USB status and nothing else. Safe from any task.
Status status();
}  // namespace dh::pairing_device
