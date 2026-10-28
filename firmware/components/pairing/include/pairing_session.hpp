#pragma once
// The hotspot pairing session as a state machine with no I/O (docs/pairing-design.md sections 4 and 7). The firmware
// glue (hotspot, web page, TLS client) reports what happened; the session decides what is allowed, keeps every secret
// in RAM only, and touches the stored configuration exactly once, with a complete one, after a good redemption.
#include "core.hpp"
#include "pairing_input.hpp"
#include "pairing_package.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace dh::pairing {
constexpr int64_t kWindowMs = 300000;   // the hotspot lives five minutes
constexpr int64_t kLongPressMs = 3000;  // an already paired device opens it only on a 3 s press
constexpr int kMaxWrongCodes = 5;       // five wrong setup codes close the hotspot
constexpr int kMaxAttempts = 5;         // five failed attempts to pair close it too
constexpr size_t kSetupCodeLength = 6, kHotspotPasswordLength = 12;

// The hardware random number generator. `false` means it failed: no secret is made from bad randomness.
struct Random {
    virtual ~Random() = default;
    virtual bool fill(uint8_t *out, size_t length) = 0;
};
bool random_digits(Random &, size_t count, std::string &out);    // no modulo bias
bool random_password(Random &, size_t count, std::string &out);  // 32 characters without I L O U

// Where the stored configuration lives. `commit` is the same save path as USB pairing (dh::save_secrets).
struct Sink {
    virtual ~Sink() = default;
    virtual bool current(Secrets &out) = 0;  // what is stored now
    virtual bool set_time(const std::string &utc) = 0;
    virtual bool commit(const Secrets &) = 0;   // all or nothing
    virtual bool readback(Secrets &out) = 0;    // what is stored after the commit
};

enum class Phase { Closed, Open, Joining, Redeeming, SaveFailed, Done };
enum class Why {
    None, NotAllowed, RandomFailed, WrongPhase, WrongCode, TooManyWrongCodes, CodeRequired, Timeout, Cancelled, InvalidInput, TooManyAttempts,
    WifiFailed, AgentUnreachable, CertificateExpired, HostnameMismatch, ChainInvalid, RedeemRefused, BadReply, SaveFailed, ReadbackMismatch
};
enum class Tls { Ok, Unreachable, CertificateExpired, HostnameMismatch, ChainInvalid };

struct Offer {
    std::string hotspot_ssid, hotspot_password, setup_code;
};
struct SetupForm {
    std::string ssid, wifi_password, package, utc;
};

// A press of the button: a long one opens pairing on a paired device. Short glitches of the signal do not restart it.
class LongPress {
public:
    explicit LongPress(int64_t hold_ms = kLongPressMs, int64_t glitch_ms = 50) : hold_(hold_ms), glitch_(glitch_ms) {}
    void edge(bool pressed, int64_t now_ms);
    bool poll(int64_t now_ms);  // true once per press, when it reaches the hold time
private:
    int64_t hold_, glitch_;
    bool down_ = false, fired_ = false, had_press_ = false;
    int64_t start_ = 0, released_ = 0;
};

class Session {
public:
    Session(Random &, Sink &, std::string device_id);
    ~Session();
    // Opens the hotspot: only on an unpaired device the first time after boot, or on a long press. Otherwise nothing opens.
    Why open(int64_t now_ms, bool paired, bool long_press);
    void close(Why why);
    Phase phase() const { return phase_; }
    Why last_why() const { return why_; }
    bool is_open() const { return phase_ != Phase::Closed && phase_ != Phase::Done; }
    const Offer &offer() const { return offer_; }  // empty unless open
    int64_t remaining_ms(int64_t now_ms) const;
    int attempts() const { return attempts_; }
    int wrong_codes() const { return wrong_codes_; }
    bool code_accepted() const { return code_ok_; }
    bool holds_secrets() const;  // a form, a token or a CA in RAM
    const Package &package() const { return package_; }
    const std::string &ca_pem() const { return ca_pem_; }  // the CA of the package as PEM: what the TLS client trusts

    Why tick(int64_t now_ms);                                                                           // closes the session when the window is over
    Why enter_code(int64_t now_ms, const std::string &code);                                            // the code on the screen
    Why submit(int64_t now_ms, const SetupForm &);                                                      // Wi-Fi and package; nothing is stored yet
    Why joined(int64_t now_ms, bool ok);                                                                // the device joined the Wi-Fi
    Why redeemed(int64_t now_ms, Tls tls, int http_status, const std::string &body);                    // POST /pairing/redeem
    Why retry_save(int64_t now_ms);                                                                     // after SaveFailed

private:
    Why expire(int64_t now_ms);
    Why fail_attempt(Why why);
    Why save();
    void clear_form();
    Random &random_;
    Sink &sink_;
    std::string device_id_;
    Phase phase_ = Phase::Closed;
    Why why_ = Why::None;
    Offer offer_;
    int64_t deadline_ = 0;
    int wrong_codes_ = 0, attempts_ = 0;
    bool code_ok_ = false, auto_open_used_ = false;
    SetupForm form_;
    Package package_;
    std::string ca_pem_;
    Reply reply_;
    bool have_reply_ = false;
};
}  // namespace dh::pairing
