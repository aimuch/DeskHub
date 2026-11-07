#pragma once
// Firmware update over Bluetooth (docs/desktop-app-design.md 13.11): the frames, the rules and the order. Pure: the flash,
// the signature check and the radio are the caller's (ble_link.cpp, management). Host tested (tests/ble_update_test.cpp).
#include <cstddef>
#include <cstdint>
#include <string>

namespace dh::ble::update {

constexpr size_t kMaxManifest = 4096;     // the signed envelope, JSON
constexpr size_t kPrefix = 288;           // image header + segment header + app descriptor: checked before the first write
constexpr size_t kAckEvery = 16 * 1024;   // "written" goes back to the App this often
constexpr int64_t kIdleMs = 15000;        // no frame for this long: the update is dropped

enum class Kind : uint8_t { Manifest, Begin, Data, End, Abort, Bad };
struct Frame {
    Kind kind = Kind::Bad;
    uint32_t offset = 0;
    const uint8_t *bytes = nullptr;
    size_t len = 0;
};
// 'M' off16 bytes | 'B' | 'D' off32 bytes | 'E' | 'X'. Anything else, or a frame too short for its fields, is Bad.
Frame parse_frame(const uint8_t *p, size_t n);

// -1, 0, 1 for dotted numbers ("0.3.21" > "0.3.9"); *ok false (and 0) when either is anything else.
int compare_versions(const std::string &a, const std::string &b, bool *ok);

// The first 288 bytes of the image: ESP32-S3, the app descriptor of project "deskhub", of this version.
bool image_prefix_ok(const uint8_t *p, size_t n, const std::string &version);

// What the manifest check (signature and the 8 fields, done by management) found.
struct Checked {
    bool ok = false;
    std::string why;          // when not ok
    std::string version, sha256;
    size_t size = 0;
};
using CheckFn = Checked (*)(const std::string &envelope, void *ctx);

// The order of one update. Each call says what the caller must do now.
struct Plan {
    enum What { None, Refuse, Open, Write, Finish, Abort } what = None;
    std::string why;          // Refuse, Abort: the reason the App is told
    const uint8_t *bytes = nullptr;
    size_t len = 0;           // Write
};
class Session {
public:
    enum class Phase { Idle, Manifest, Opening, Receiving, Finishing };
    struct Context {
        std::string running;        // the version running now
        size_t slot = 0;            // the size of the other slot
        bool allowed = false;       // no other update, no self-test pending, power steady
        std::string not_allowed;    // why not
        CheckFn check = nullptr;
        void *check_ctx = nullptr;
    };
    Plan frame(const Frame &f, int64_t now_ms, const Context &c);
    Plan tick(int64_t now_ms);      // Abort when idle too long during an update
    void opened(bool ok);           // the slot is open (Receiving), or could not be (Idle)
    void reset();                   // after an abort, a disconnect, a finish
    Phase phase() const { return phase_; }
    size_t received() const { return received_; }
    const Checked &manifest() const { return checked_; }
private:
    Plan abort_(const char *why);
    Phase phase_ = Phase::Idle;
    std::string buffer_;
    Checked checked_;
    size_t received_ = 0;
    int64_t last_ = 0;
};

}  // namespace dh::ble::update
