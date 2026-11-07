#pragma once
// The Bluetooth link between the touch screen and the DeskHub App (docs/desktop-app-design.md 13.3 and 13.4), the part
// without NimBLE: message parsing with limits, the list of input sources, one request at a time, the pairing window and
// the words of a result. Tested on the host (firmware/tests/ble_protocol_test.cpp).
//
// Messages are short JSON objects, one per GATT write or notification (at most 512 bytes):
//   App -> device (control):  {"op":"sources_begin","count":3}
//                             {"op":"source","i":0,"id":"<App id>","name":"MacBook M1","port":"hdmi1"}
//                             {"op":"sources_end"}
//                             {"op":"result","req":7,"outcome":"confirmed|sent_unconfirmed|not_sent","detail":"..."}
//                             {"op":"current","id":"<App id>"}   the input the monitor is on; "" when not known
//   device -> App (events):   {"op":"switch","req":7,"id":"<App id>"}
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dh::ble {
constexpr int kProtocol = 1;
constexpr size_t kMaxMessage = 512, kMaxSources = 12, kMaxId = 48, kMaxName = 32;
constexpr int64_t kPairingWindowMs = 120000;   // "pair a computer" accepts a new computer for 2 minutes (K1)
constexpr int64_t kConfirmMs = 30000;          // the six digits must be confirmed within 30 s (the Mac gives up then too)
constexpr int64_t kRequestTimeoutMs = 12000;   // the App needs about 4.5 s for a switch and its read-backs

struct Source {
    std::string id, name, port;   // port: dp1 dp2 hdmi1 hdmi2 usbc other
    bool operator==(const Source &o) const { return id == o.id && name == o.name && port == o.port; }
};
enum class Outcome { Confirmed, SentUnconfirmed, NotSent };
struct Result {
    uint32_t req = 0;
    Outcome outcome = Outcome::NotSent;
    std::string detail;
};

enum class Op { SourcesBegin, Source, SourcesEnd, Result, Current, Bad };
struct Message {
    Op op = Op::Bad;
    size_t count = 0;        // SourcesBegin
    size_t index = 0;        // Source
    Source source;           // Source
    Result result;           // Result
    std::string current;     // Current: an id, or "" for "not known"
    std::string error;       // Bad: why
};
// Strict: a known op, the fields it needs with the right types, nothing too long, valid UTF-8 without control characters.
Message parse(const std::string &bytes);

// The list arrives in pieces. It replaces the shown list only when it is complete: every index once, the count as
// announced. Anything out of order throws the draft away (the shown list stays as it was).
struct SourcesDraft {
    bool open = false;
    size_t expected = 0;
    std::vector<std::optional<Source>> items;
};
enum class Apply { Pending, Complete, Rejected };
Apply apply(SourcesDraft &, const Message &, std::vector<Source> *complete);

// One switch request at a time. A result counts only for the request that is waiting; a late or unknown one is ignored.
// A request that got no result in kRequestTimeoutMs is over, and says so.
class Requests {
public:
    // false: one is still waiting, or the id is not in `sources` (the device's own white list, K2).
    bool begin(const std::string &id, const std::vector<Source> &sources, int64_t now_ms, uint32_t *req);
    std::optional<std::string> finish(const Result &);   // the id the result is for; nullopt when it is not the waiting one
    bool expire(int64_t now_ms);                          // true once, when the waiting request timed out
    bool waiting() const { return pending_.has_value(); }
    void reset() { pending_.reset(); }   // the link went down
private:
    struct Pending { uint32_t req; std::string id; int64_t sent_ms; };
    uint32_t next_ = 1;
    std::optional<Pending> pending_;
};

// K1: a new computer is accepted only while the window is open, and only after the person confirmed the digits.
class PairingWindow {
public:
    void open(int64_t now_ms) { until_ = now_ms + kPairingWindowMs; }
    void close() { until_ = 0; }
    bool is_open(int64_t now_ms) const { return until_ != 0 && now_ms < until_; }
    int64_t left_ms(int64_t now_ms) const { return is_open(now_ms) ? until_ - now_ms : 0; }
private:
    int64_t until_ = 0;
};
bool accept_pairing(const PairingWindow &, int64_t now_ms, int64_t digits_shown_ms, bool person_said_same);

// The current input as the device shows it: `id` when it is in the list, else "" (not known).
std::string current_in(const std::vector<Source> &, const std::string &id);
std::string request_json(uint32_t req, const std::string &id);
std::string info_json(const std::string &device_id, const std::string &firmware);
// The line the device shows for a result: "已切到 MacBook M1，已确认" / "已发送到 …，未确认：…" / "没有切到 …：…".
std::string result_text(const Result &, const std::string &name);
}  // namespace dh::ble
