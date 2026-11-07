// Failure modes of the firmware update over Bluetooth (docs/desktop-app-design.md 13.11), written BEFORE the code.
//
// O1  Frames (U14): 'M' needs a 2-byte offset, 'D' a 4-byte offset (little endian) and at least one byte of data; 'B', 'E'
//     and 'X' are one byte exactly. An empty frame, an unknown type, a frame too short: Bad.
// O2  Versions: dotted numbers compare by number (0.3.21 > 0.3.9, 1.0 = 1.0.0); anything else cannot be compared (U3).
// O3  The image prefix (U8): 0xE9, chip 9 (ESP32-S3) at byte 12, the descriptor magic 0xABCD5432 at 32, project "deskhub"
//     at 80, the version at 48, all of it within 288 bytes. Anything else is refused.
// O4  The manifest (U2): pieces in order from offset 0, at most 4 KB in all; a gap, an overlap or more than 4 KB drops it.
//     'B' without a manifest is refused; a manifest the check refuses is refused with its reason; nothing is opened.
// O5  U3 to U6: not newer than the running version (the same, older, not comparable), larger than the slot, smaller than
//     288 bytes, or not allowed (busy, self-test, power): refused, nothing opened.
// O6  U7: data before the slot is open, an offset that does not follow on, more bytes than the manifest says: the update is
//     dropped (Abort) and the session is idle again.
// O7  'E' only when every byte came; early 'E' drops it. After Finish nothing more is taken until reset.
// O8  U10, U14: 'X', a bad frame during an update, 15 s without a frame: Abort. Outside an update a bad frame is ignored.
// O9  The good path: M.. B -> Open; opened(true) -> data -> Write, each piece in order; E -> Finish.
#include "ble_update.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dh::ble::update;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<uint8_t> bytes(std::initializer_list<int> l) { std::vector<uint8_t> v; for (int b : l) v.push_back(static_cast<uint8_t>(b)); return v; }
static Frame F(const std::vector<uint8_t> &v) { return parse_frame(v.data(), v.size()); }
static std::vector<uint8_t> manifest_frame(uint16_t off, const std::string &s) {
    std::vector<uint8_t> v = {'M', static_cast<uint8_t>(off & 0xff), static_cast<uint8_t>(off >> 8)};
    v.insert(v.end(), s.begin(), s.end());
    return v;
}
static std::vector<uint8_t> data_frame(uint32_t off, size_t n, uint8_t fill = 0xAB) {
    std::vector<uint8_t> v = {'D', static_cast<uint8_t>(off), static_cast<uint8_t>(off >> 8), static_cast<uint8_t>(off >> 16), static_cast<uint8_t>(off >> 24)};
    v.insert(v.end(), n, fill);
    return v;
}

static void o1_frames() {
    CHECK(parse_frame(nullptr, 0).kind == Kind::Bad);
    CHECK(F(bytes({'B'})).kind == Kind::Begin && F(bytes({'E'})).kind == Kind::End && F(bytes({'X'})).kind == Kind::Abort);
    CHECK(F(bytes({'B', 0})).kind == Kind::Bad && F(bytes({'E', 1})).kind == Kind::Bad && F(bytes({'X', 'X'})).kind == Kind::Bad);
    CHECK(F(bytes({'Q', 1, 2})).kind == Kind::Bad);
    const auto mv = manifest_frame(0x0102, "{}");   // the frame points into its bytes: they must live
    Frame m = F(mv);
    CHECK(m.kind == Kind::Manifest && m.offset == 0x0102 && m.len == 2 && m.bytes[0] == '{');
    CHECK(F(bytes({'M', 0})).kind == Kind::Bad && F(bytes({'M', 0, 0})).kind == Kind::Bad);   // no offset, no bytes
    const auto dv = data_frame(0x01020304, 3);
    Frame d = F(dv);
    CHECK(d.kind == Kind::Data && d.offset == 0x01020304 && d.len == 3 && d.bytes[2] == 0xAB);
    CHECK(F(bytes({'D', 0, 0, 0})).kind == Kind::Bad && F(bytes({'D', 0, 0, 0, 0})).kind == Kind::Bad);
}

static void o2_versions() {
    bool ok = false;
    CHECK(compare_versions("0.3.21", "0.3.9", &ok) == 1 && ok);
    CHECK(compare_versions("0.3.9", "0.3.21", &ok) == -1 && ok);
    CHECK(compare_versions("1.0", "1.0.0", &ok) == 0 && ok);
    CHECK(compare_versions("0.3.22", "0.3.22", &ok) == 0 && ok);
    for (const char *bad : {"", "0.3.21-dev", "0..1", ".1", "1.", "v1", "1.2.x", "12345678901"}) {
        ok = true;
        CHECK(compare_versions(bad, "0.3.21", &ok) == 0 && !ok);
        ok = true;
        CHECK(compare_versions("0.3.21", bad, &ok) == 0 && !ok);
    }
}

static std::vector<uint8_t> prefix(const std::string &version, uint16_t chip = 9, uint32_t magic = 0xABCD5432, const std::string &project = "deskhub") {
    std::vector<uint8_t> p(kPrefix, 0);
    p[0] = 0xE9;
    p[12] = static_cast<uint8_t>(chip); p[13] = static_cast<uint8_t>(chip >> 8);
    for (int i = 0; i < 4; ++i) p[32 + i] = static_cast<uint8_t>(magic >> (8 * i));
    std::memcpy(p.data() + 48, version.data(), std::min<size_t>(version.size(), 31));
    std::memcpy(p.data() + 80, project.data(), std::min<size_t>(project.size(), 31));
    return p;
}
static void o3_prefix() {
    auto good = prefix("0.3.22");
    CHECK(image_prefix_ok(good.data(), good.size(), "0.3.22"));
    CHECK(!image_prefix_ok(good.data(), good.size(), "0.3.23"));                // another version
    CHECK(!image_prefix_ok(good.data(), 287, "0.3.22"));                        // too short
    auto p = prefix("0.3.22", 0); CHECK(!image_prefix_ok(p.data(), p.size(), "0.3.22"));   // ESP32
    p = prefix("0.3.22", 9, 0x12345678); CHECK(!image_prefix_ok(p.data(), p.size(), "0.3.22"));
    p = prefix("0.3.22", 9, 0xABCD5432, "other"); CHECK(!image_prefix_ok(p.data(), p.size(), "0.3.22"));
    p = good; p[0] = 0xE8; CHECK(!image_prefix_ok(p.data(), p.size(), "0.3.22"));
    p = good; std::memset(p.data() + 48, 'x', 32); CHECK(!image_prefix_ok(p.data(), p.size(), "0.3.22"));   // no end to the version
    // a version field of 32 characters with no end runs into the project name: it must be refused even when that run matches
    p = good; std::memset(p.data() + 48, 'a', 32); CHECK(!image_prefix_ok(p.data(), p.size(), std::string(32, 'a') + "deskhub"));
    p = good; std::memset(p.data() + 80, 'x', 32); CHECK(!image_prefix_ok(p.data(), p.size(), "0.3.22"));   // no end to the project
}

// The check of the signed manifest is management's; here a stand-in: "good:<version>:<size>" passes, anything else not.
static Checked fake_check(const std::string &envelope, void *) {
    Checked c;
    unsigned size = 0;
    char version[32] = {0};
    if (std::sscanf(envelope.c_str(), "good:%31[^:]:%u", version, &size) == 2) { c.ok = true; c.version = version; c.size = size; c.sha256 = std::string(64, 'a'); }
    else c.why = "UPDATE_SIGNATURE_INVALID";
    return c;
}
static Session::Context ctx(bool allowed = true) {
    Session::Context c;
    c.running = "0.3.21"; c.slot = 0x600000; c.allowed = allowed; c.not_allowed = allowed ? "" : "UPDATE_BUSY";
    c.check = fake_check;
    return c;
}
static Plan feed(Session &s, const std::vector<uint8_t> &v, int64_t now = 1000, const Session::Context &c = ctx()) { return s.frame(F(v), now, c); }
static void begin_ok(Session &s, size_t size = 1000, const std::string &version = "0.3.22") {
    feed(s, manifest_frame(0, "good:" + version + ":" + std::to_string(size)));
    Plan p = feed(s, bytes({'B'}));
    CHECK(p.what == Plan::Open);
    s.opened(true);
}

static void o4_manifest() {
    Session s;
    CHECK(feed(s, bytes({'B'})).what == Plan::Refuse);                         // no manifest
    CHECK(s.phase() == Session::Phase::Idle);
    feed(s, manifest_frame(0, "good:0.3.22:"));
    feed(s, manifest_frame(12, "1000"));
    Plan p = feed(s, bytes({'B'}));
    CHECK(p.what == Plan::Open && s.manifest().version == "0.3.22" && s.manifest().size == 1000);
    s.reset();
    feed(s, manifest_frame(0, "good:0.3.22:"));
    CHECK(feed(s, manifest_frame(20, "1000")).what == Plan::Abort);           // a gap
    CHECK(s.phase() == Session::Phase::Idle);
    feed(s, manifest_frame(0, "good:0.3.22:"));
    CHECK(feed(s, manifest_frame(5, "1000")).what == Plan::Abort);            // an overlap
    s.reset();
    std::string big(kMaxManifest, 'x');
    CHECK(feed(s, manifest_frame(0, big)).what == Plan::None);
    CHECK(feed(s, manifest_frame(static_cast<uint16_t>(kMaxManifest), "y")).what == Plan::Abort);   // over 4 KB
    s.reset();
    feed(s, manifest_frame(0, "forged"));
    p = feed(s, bytes({'B'}));
    CHECK(p.what == Plan::Refuse && p.why == "UPDATE_SIGNATURE_INVALID" && s.phase() == Session::Phase::Idle);
}

static void o5_refusals() {
    struct Case { std::string manifest; bool allowed; const char *why; };
    for (const Case &c : {Case{"good:0.3.21:1000", true, "UPDATE_NOT_NEWER"}, Case{"good:0.3.20:1000", true, "UPDATE_NOT_NEWER"},
                          Case{"good:0.3.22-dev:1000", true, "UPDATE_NOT_NEWER"}, Case{"good:0.3.22:6291457", true, "UPDATE_TOO_LARGE"},
                          Case{"good:0.3.22:287", true, "UPDATE_TOO_SMALL"}, Case{"good:0.3.22:1000", false, "UPDATE_BUSY"}}) {
        Session s;
        feed(s, manifest_frame(0, c.manifest), 1000, ctx(c.allowed));
        Plan p = feed(s, bytes({'B'}), 1000, ctx(c.allowed));
        if (p.what != Plan::Refuse || p.why != c.why) std::fprintf(stderr, "%s: what %d why %s\n", c.manifest.c_str(), p.what, p.why.c_str());
        CHECK(p.what == Plan::Refuse && p.why == c.why && s.phase() == Session::Phase::Idle);
    }
    Session s;   // the slot is smaller than the image
    Session::Context small = ctx(); small.slot = 4096;
    feed(s, manifest_frame(0, "good:0.3.22:5000"), 1000, small);
    CHECK(s.frame(F(bytes({'B'})), 1000, small).why == "UPDATE_TOO_LARGE");
}

static void o6_data() {
    Session s;
    feed(s, manifest_frame(0, "good:0.3.22:1000"));
    feed(s, bytes({'B'}));
    CHECK(feed(s, data_frame(0, 10)).what == Plan::Abort);                     // before the slot is open
    CHECK(s.phase() == Session::Phase::Idle);
    begin_ok(s);
    CHECK(feed(s, data_frame(0, 100)).what == Plan::Write);
    CHECK(feed(s, data_frame(50, 10)).what == Plan::Abort);                    // does not follow on
    begin_ok(s);
    CHECK(feed(s, data_frame(0, 900)).what == Plan::Write);
    CHECK(feed(s, data_frame(900, 101)).what == Plan::Abort);                  // more than the manifest says
    Session t;
    feed(t, manifest_frame(0, "good:0.3.22:1000"));
    feed(t, bytes({'B'}));
    t.opened(false);                                                            // the slot could not be opened
    CHECK(t.phase() == Session::Phase::Idle && feed(t, data_frame(0, 10)).what == Plan::Abort);
}

static void o7_end() {
    Session s;
    begin_ok(s);
    feed(s, data_frame(0, 999));
    CHECK(feed(s, bytes({'E'})).what == Plan::Abort);                           // one byte missing
    begin_ok(s);
    feed(s, data_frame(0, 1000));
    CHECK(feed(s, bytes({'E'})).what == Plan::Finish && s.phase() == Session::Phase::Finishing);
    CHECK(feed(s, data_frame(1000, 1)).what == Plan::Abort);                   // nothing after the end
    s.reset();
    CHECK(s.phase() == Session::Phase::Idle && feed(s, bytes({'E'})).what == Plan::Refuse);
}

static void o8_abort_and_idle() {
    Session s;
    begin_ok(s);
    CHECK(feed(s, bytes({'X'})).what == Plan::Abort && s.phase() == Session::Phase::Idle);
    begin_ok(s);
    CHECK(feed(s, bytes({'Q'})).what == Plan::Abort);                            // a bad frame during an update
    CHECK(feed(s, bytes({'Q'})).what == Plan::None);                             // outside: ignored
    begin_ok(s);
    feed(s, data_frame(0, 100), 2000);
    CHECK(s.tick(2000 + kIdleMs - 1).what == Plan::None);
    CHECK(s.tick(2000 + kIdleMs + 1).what == Plan::Abort && s.phase() == Session::Phase::Idle);
    CHECK(s.tick(999999).what == Plan::None);                                   // idle: nothing to drop
    Session m;
    feed(m, manifest_frame(0, "good:0.3.22:1000"), 1000);
    CHECK(m.tick(1000 + kIdleMs + 1).what == Plan::Abort);                       // a manifest left half-way
}

static void o9_good_path() {
    Session s;
    feed(s, manifest_frame(0, "good:0.3.22:"), 10);
    feed(s, manifest_frame(12, "2500"), 20);
    Plan p = feed(s, bytes({'B'}), 30);
    CHECK(p.what == Plan::Open && s.phase() == Session::Phase::Opening);
    s.opened(true);
    CHECK(s.phase() == Session::Phase::Receiving);
    size_t off = 0, writes = 0;
    while (off < 2500) {
        const size_t n = std::min<size_t>(244, 2500 - off);
        auto v = data_frame(static_cast<uint32_t>(off), n, static_cast<uint8_t>(off));
        Plan w = feed(s, v, 40);
        CHECK(w.what == Plan::Write && w.len == n && w.bytes && w.bytes[0] == static_cast<uint8_t>(off));
        off += n; ++writes;
    }
    CHECK(s.received() == 2500 && writes == 11);
    CHECK(feed(s, bytes({'E'}), 50).what == Plan::Finish);
}

int main() {
    o1_frames(); o2_versions(); o3_prefix(); o4_manifest(); o5_refusals(); o6_data(); o7_end(); o8_abort_and_idle(); o9_good_path();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::puts("ble_update_test: all checks passed");
    return 0;
}
