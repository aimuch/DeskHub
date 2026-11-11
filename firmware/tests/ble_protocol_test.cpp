// Failure modes of the Bluetooth link's protocol (docs/desktop-app-design.md 13.3, 13.4), written BEFORE the code.
//
// B1  Parsing is strict: a known op with exactly the fields it needs, of the right type. Unknown ops, missing or wrong
//     fields, junk after the object, more than 512 bytes, ids over 48 or names over 32 bytes, empty ids or names,
//     control characters, invalid UTF-8, an unknown port, a count over 12, a negative or fractional number, an unknown
//     outcome: all refused with a reason, never half-read.
// B2  The source list replaces the shown one only when complete: begin with a count, each index once, end. An index
//     twice, out of range, a source without begin, an end before all came, a second begin: the draft is dropped and
//     the old list stays. Ids must be unique in a list. A count of 0 is a valid empty list.
// B3  One request at a time; only for an id in the list the App sent (the device's white list, K2). A result counts only
//     for the waiting request; a late, unknown or repeated one is ignored. After the timeout the request is over (once),
//     and a new one may start. Request numbers are never reused.
// B4  K1: a new computer is accepted only inside the 2-minute window AND after the person said the digits are the same,
//     within 30 s of seeing them. Outside the window, too late, or "not the same": refused. The window closes on time.
// B5  What goes out is well formed: the switch request and the info are valid JSON with exactly the fields, ids
//     escaped. The result line names the input and the outcome, and the reason when there is one (K3: "confirmed" only
//     for confirmed).
// B7  The usage message (docs/desktop-app-design.md 13.12 G1): {"op":"usage","ok":false} exactly, or ok:true with exactly
//     its fields: provider and plan of 0 to 16 [a-z0-9._-]; 0 to 2 windows of exactly {m 1..525600, r 0..100, s 0..31536000};
//     calls, errors (not more than calls), tokens (up to 1e15), ttft (up to 600000 ms); the latest model of 0 to 32 bytes
//     without control characters, its status 0..999, its time up to 3600000 ms, its age up to 31536000 s. Whole numbers only.
// B6  The current input (the user asked 2026-11-07 that the active source stands out): {"op":"current","id":...} names
//     the input the App read back from the monitor, or "" when it does not know (K3: no mark without a read-back). Strict
//     like the rest; an id not in the list the App sent counts as unknown.
#include "ble_protocol.hpp"
#include "json_util.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace dh::ble;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static Message P(const std::string &s) { return parse(s); }
static bool bad(const std::string &s) {
    const Message m = parse(s);
    if (m.op != Op::Bad) std::fprintf(stderr, "accepted: %s\n", s.c_str());
    return m.op == Op::Bad && !m.error.empty();
}

static void b1_parse() {
    Message m = P(R"({"op":"sources_begin","count":3})");
    CHECK(m.op == Op::SourcesBegin && m.count == 3);
    m = P(R"({"op":"source","i":1,"id":"60621D95","name":"MacBook M1","port":"hdmi1"})");
    CHECK(m.op == Op::Source && m.index == 1 && m.source.id == "60621D95" && m.source.name == "MacBook M1" && m.source.port == "hdmi1");
    m = P(R"({"op":"source","i":0,"id":"a","name":"电视","port":"other"})");
    CHECK(m.op == Op::Source && m.source.name == "电视");
    CHECK(P(R"({"op":"sources_end"})").op == Op::SourcesEnd);
    m = P(R"({"op":"result","req":7,"outcome":"confirmed","detail":""})");
    CHECK(m.op == Op::Result && m.result.req == 7 && m.result.outcome == Outcome::Confirmed);
    m = P(R"({"op":"result","req":8,"outcome":"sent_unconfirmed","detail":"读不回"})");
    CHECK(m.result.outcome == Outcome::SentUnconfirmed && m.result.detail == "读不回");
    CHECK(P(R"({"op":"result","req":9,"outcome":"not_sent","detail":"DDC 写失败"})").result.outcome == Outcome::NotSent);
    CHECK(P(R"({"op":"sources_begin","count":0})").op == Op::SourcesBegin);
    for (const std::string &s : {std::string(""), std::string("{}"), std::string("[]"), std::string(R"({"op":"reboot"})"), std::string(R"({"op":1})"),
                                 std::string(R"({"op":"sources_begin"})"), std::string(R"({"op":"sources_begin","count":13})"), std::string(R"({"op":"sources_begin","count":-1})"),
                                 std::string(R"({"op":"sources_begin","count":1.5})"), std::string(R"({"op":"sources_begin","count":"3"})"),
                                 std::string(R"({"op":"sources_begin","count":3}x)"), std::string(R"({"op":"sources_begin","count":3,"extra":1})"),
                                 std::string(R"({"op":"source","i":0,"id":"","name":"x","port":"dp1"})"), std::string(R"({"op":"source","i":0,"id":"a","name":"","port":"dp1"})"),
                                 std::string(R"({"op":"source","i":0,"id":"a","name":"x","port":"vga"})"), std::string(R"({"op":"source","i":0,"id":"a","name":"x"})"),
                                 std::string(R"({"op":"source","i":12,"id":"a","name":"x","port":"dp1"})"), std::string(R"({"op":"source","i":-1,"id":"a","name":"x","port":"dp1"})"),
                                 std::string(R"({"op":"source","i":0,"id":"a","name":"tab\there","port":"dp1"})"),
                                 std::string("{\"op\":\"source\",\"i\":0,\"id\":\"a\",\"name\":\"\xe4\xb8\",\"port\":\"dp1\"}"),
                                 std::string(R"({"op":"result","req":0,"outcome":"confirmed","detail":""})"), std::string(R"({"op":"result","req":7,"outcome":"maybe","detail":""})"),
                                 std::string(R"({"op":"result","req":7,"outcome":"confirmed"})"), std::string(R"({"op":"result","req":4294967296,"outcome":"confirmed","detail":""})"),
                                 std::string(R"({"op":"sources_end","x":1})")})
        CHECK(bad(s));
    CHECK(bad(R"({"op":"source","i":0,"id":")" + std::string(49, 'i') + R"(","name":"x","port":"dp1"})"));
    CHECK(!bad(R"({"op":"source","i":0,"id":")" + std::string(48, 'i') + R"(","name":"x","port":"dp1"})"));
    CHECK(bad(R"({"op":"source","i":0,"id":"a","name":")" + std::string(33, 'n') + R"(","port":"dp1"})"));
    CHECK(bad(R"({"op":"result","req":1,"outcome":"confirmed","detail":")" + std::string(200, 'd') + R"("})"));
    CHECK(bad(std::string(R"({"op":"sources_end"})") + std::string(600, ' ')));
}

static Message src(size_t i, const std::string &id, const std::string &name = "x", const std::string &port = "dp1") {
    Message m; m.op = Op::Source; m.index = i; m.source = {id, name, port}; return m;
}
static Message begin(size_t n) { Message m; m.op = Op::SourcesBegin; m.count = n; return m; }
static Message end() { Message m; m.op = Op::SourcesEnd; return m; }

static void b2_list() {
    SourcesDraft d;
    std::vector<Source> out{{"old", "Old", "dp1"}};
    CHECK(apply(d, begin(2), &out) == Apply::Pending);
    CHECK(apply(d, src(1, "b"), &out) == Apply::Pending);
    CHECK(apply(d, src(0, "a"), &out) == Apply::Pending);
    CHECK(out.size() == 1 && out[0].id == "old");   // not yet
    CHECK(apply(d, end(), &out) == Apply::Complete);
    CHECK(out.size() == 2 && out[0].id == "a" && out[1].id == "b");   // in index order
    // damaged sequences keep the old list
    auto rejected = [&](std::vector<Message> seq) {
        SourcesDraft x;
        std::vector<Source> keep{{"k", "K", "dp1"}};
        Apply last = Apply::Pending;
        for (const auto &m : seq) { last = apply(x, m, &keep); if (last == Apply::Rejected) break; }
        return last == Apply::Rejected && keep.size() == 1 && keep[0].id == "k";
    };
    CHECK(rejected({begin(2), src(0, "a"), src(0, "a2")}));                 // an index twice
    CHECK(rejected({begin(2), src(0, "a"), src(2, "c")}));                  // out of range
    CHECK(rejected({src(0, "a")}));                                         // no begin
    CHECK(rejected({begin(2), src(0, "a"), end()}));                        // end too early
    CHECK(rejected({begin(2), src(0, "a"), begin(1)}));                     // a second begin
    CHECK(rejected({end()}));                                               // end without begin
    CHECK(rejected({begin(2), src(0, "a"), src(1, "a")}));                  // the same id twice
    SourcesDraft e;
    std::vector<Source> none{{"k", "K", "dp1"}};
    CHECK(apply(e, begin(0), &none) == Apply::Pending && apply(e, end(), &none) == Apply::Complete && none.empty());
    // after a rejection a new list can come
    SourcesDraft r;
    std::vector<Source> l;
    apply(r, end(), &l);
    CHECK(apply(r, begin(1), &l) == Apply::Pending && apply(r, src(0, "z"), &l) == Apply::Pending && apply(r, end(), &l) == Apply::Complete && l.size() == 1);
}

static void b3_requests() {
    const std::vector<Source> list{{"a", "A", "dp1"}, {"b", "B", "hdmi1"}};
    Requests q;
    uint32_t r1 = 0, r2 = 0, r3 = 0;
    CHECK(!q.begin("zzz", list, 0, &r1));                       // not in the list
    CHECK(q.begin("a", list, 0, &r1) && r1 != 0 && q.waiting());
    CHECK(!q.begin("b", list, 100, &r2));                       // one at a time
    Result late; late.req = r1 + 5; late.outcome = Outcome::Confirmed;
    CHECK(!q.finish(late).has_value() && q.waiting());          // not the waiting one
    Result ok; ok.req = r1; ok.outcome = Outcome::Confirmed;
    auto id = q.finish(ok);
    CHECK(id.has_value() && *id == "a" && !q.waiting());
    CHECK(!q.finish(ok).has_value());                           // repeated
    CHECK(q.begin("b", list, 1000, &r2) && r2 != r1);
    CHECK(!q.expire(1000 + kRequestTimeoutMs - 1));
    CHECK(q.expire(1000 + kRequestTimeoutMs) && !q.waiting());   // over, once
    CHECK(!q.expire(1000 + kRequestTimeoutMs + 10));
    Result stale; stale.req = r2; stale.outcome = Outcome::Confirmed;
    CHECK(!q.finish(stale).has_value());                        // after the timeout it no longer counts
    CHECK(q.begin("a", list, 20000, &r3) && r3 != r2 && r3 != r1);
    q.reset();
    CHECK(!q.waiting());
    CHECK(!q.begin("a", {}, 30000, &r3));                       // no list: nothing to switch
}

static void b4_pairing() {
    PairingWindow w;
    CHECK(!w.is_open(0) && !accept_pairing(w, 0, 0, true));
    w.open(1000);
    CHECK(w.is_open(1000) && w.left_ms(1000) == kPairingWindowMs);
    CHECK(accept_pairing(w, 5000, 2000, true));
    CHECK(!accept_pairing(w, 5000, 2000, false));                           // "not the same"
    CHECK(!accept_pairing(w, 2000 + kConfirmMs, 2000, true));               // too late to confirm
    CHECK(accept_pairing(w, 2000 + kConfirmMs - 1, 2000, true));
    CHECK(!w.is_open(1000 + kPairingWindowMs));                             // closes on time
    CHECK(!accept_pairing(w, 1000 + kPairingWindowMs, 1000 + kPairingWindowMs - 10, true));
    w.open(500000);
    w.close();
    CHECK(!w.is_open(500001) && w.left_ms(500001) == 0);
}

static void b5_out() {
    const std::string r = request_json(7, "id\"with\\quote");
    cJSON *j = dh::parse_json_strict(r.c_str(), r.size());
    CHECK(j != nullptr);
    if (j) {
        CHECK(cJSON_GetArraySize(j) == 3);
        CHECK(std::string(cJSON_GetObjectItemCaseSensitive(j, "op")->valuestring) == "switch");
        CHECK(cJSON_GetObjectItemCaseSensitive(j, "req")->valuedouble == 7);
        CHECK(std::string(cJSON_GetObjectItemCaseSensitive(j, "id")->valuestring) == "id\"with\\quote");
        cJSON_Delete(j);
    }
    const std::string fp(64, 'c');
    const std::string i = info_json("deskhub-44bd8d60c598", "0.3.18", fp, false);
    j = dh::parse_json_strict(i.c_str(), i.size());
    CHECK(j != nullptr);
    if (j) {
        CHECK(cJSON_GetObjectItemCaseSensitive(j, "protocol")->valuedouble == kProtocol);
        CHECK(std::string(cJSON_GetObjectItemCaseSensitive(j, "device")->valuestring) == "deskhub-44bd8d60c598");
        CHECK(std::string(cJSON_GetObjectItemCaseSensitive(j, "firmware")->valuestring) == "0.3.18");
        // 13.11: the fingerprint of the update key, so the App knows whether its own key is the one on the device
        CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "update_key")) && std::string(cJSON_GetObjectItemCaseSensitive(j, "update_key")->valuestring) == fp);
        // 13.11 U16: whether the running firmware has passed its self-test. The App waits for "confirmed" after an update.
        CHECK(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "boot")) && std::string(cJSON_GetObjectItemCaseSensitive(j, "boot")->valuestring) == "confirmed");
        CHECK(cJSON_GetArraySize(j) == 5);
        cJSON_Delete(j);
    }
    {   // no key on the device: an empty string, never a missing field
        const std::string n = info_json("deskhub-44bd8d60c598", "0.3.18", "", true);
        cJSON *k = dh::parse_json_strict(n.c_str(), n.size());
        CHECK(k && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(k, "update_key")) && std::string(cJSON_GetObjectItemCaseSensitive(k, "update_key")->valuestring).empty());
        CHECK(k && std::string(cJSON_GetObjectItemCaseSensitive(k, "boot")->valuestring) == "pending");   // still in its self-test
        cJSON_Delete(k);
    }
    {   // U16 on the device 2026-11-11: the App read the info 3 s after the restart, before main set the hook; it must not say "confirmed"
        CHECK(boot_pending_now(nullptr, true));                       // no hook yet: the bootloader's state at start decides
        CHECK(!boot_pending_now(nullptr, false));
        CHECK(!boot_pending_now([] { return false; }, true));          // the hook exists: the self-test passed, it is confirmed now
        CHECK(boot_pending_now([] { return true; }, false));
    }
    Result c; c.outcome = Outcome::Confirmed;
    CHECK(result_text(c, "MacBook M1") == "已切到 MacBook M1，已确认");
    Result u; u.outcome = Outcome::SentUnconfirmed; u.detail = "读不回";
    CHECK(result_text(u, "MacBook M4") == "已发送到 MacBook M4，未确认：读不回");
    Result n; n.outcome = Outcome::NotSent; n.detail = "DDC 写失败";
    CHECK(result_text(n, "电视") == "没有切到 电视：DDC 写失败");
    Result n2; n2.outcome = Outcome::NotSent;
    CHECK(result_text(n2, "X") == "没有切到 X");
}

static void b6_current() {
    Message m = P(R"({"op":"current","id":"id-m1"})");
    CHECK(m.op == Op::Current && m.current == "id-m1");
    m = P(R"({"op":"current","id":""})");
    CHECK(m.op == Op::Current && m.current.empty());
    CHECK(bad(R"({"op":"current"})"));
    CHECK(bad(R"({"op":"current","id":3})"));
    CHECK(bad(R"({"op":"current","id":"a","x":1})"));
    CHECK(bad(R"({"op":"current","id":"tab\there"})"));
    CHECK(bad(R"({"op":"current","id":")" + std::string(49, 'i') + R"("})"));
    const std::vector<Source> list{{"a", "A", "dp1"}, {"b", "B", "hdmi1"}};
    CHECK(current_in(list, "b") == "b");
    CHECK(current_in(list, "zzz").empty());
    CHECK(current_in(list, "").empty());
    CHECK(current_in({}, "a").empty());
}


static const char *kUsage =
    R"({"op":"usage","ok":true,"p":"codex","plan":"plus","w":[{"m":300,"r":100,"s":15120},{"m":10080,"r":37,"s":300000}],"c":128,"e":2,"t":1234567,"f":820,"lm":"gpt-6.1-sol","ls":200,"lms":3200,"la":120})";
static std::string swap(const std::string &from, const std::string &to) {
    std::string s = kUsage;
    const size_t at = s.find(from);
    if (at == std::string::npos) { std::fprintf(stderr, "no [%s] in the sample\n", from.c_str()); return s; }
    return s.replace(at, from.size(), to);
}
static void b7_usage() {
    Message m = P(kUsage);
    CHECK(m.op == Op::Usage && m.usage.ok);
    CHECK(m.usage.provider == "codex" && m.usage.plan == "plus");
    CHECK(m.usage.windows.size() == 2 && m.usage.windows[0] == (UsageWindow{300, 100, 15120}) && m.usage.windows[1] == (UsageWindow{10080, 37, 300000}));
    CHECK(m.usage.calls == 128 && m.usage.errors == 2 && m.usage.tokens == 1234567 && m.usage.ttft_ms == 820);
    CHECK(m.usage.latest_model == "gpt-6.1-sol" && m.usage.latest_status == 200 && m.usage.latest_ms == 3200 && m.usage.latest_age_s == 120);
    m = P(R"({"op":"usage","ok":false})");
    CHECK(m.op == Op::Usage && !m.usage.ok);
    // the edges that are fine
    CHECK(P(swap(R"("w":[{"m":300,"r":100,"s":15120},{"m":10080,"r":37,"s":300000}])", R"("w":[])")).op == Op::Usage);
    CHECK(P(swap(R"("lm":"gpt-6.1-sol")", R"("lm":"")")).op == Op::Usage);
    CHECK(P(swap(R"("p":"codex","plan":"plus")", R"("p":"","plan":"")")).op == Op::Usage);
    CHECK(P(swap(R"("t":1234567)", R"("t":1000000000000000)")).op == Op::Usage);
    CHECK(P(swap(R"("e":2)", R"("e":128)")).op == Op::Usage);
    CHECK(P(swap(R"("lm":"gpt-6.1-sol")", R"("lm":"模型名")")).usage.latest_model == "模型名");
    // refused
    for (const std::string &s : {
             std::string(R"({"op":"usage","ok":false,"c":1})"), std::string(R"({"op":"usage"})"), std::string(R"({"op":"usage","ok":1})"),
             swap(R"("ok":true)", R"("ok":"yes")"), swap(R"(,"la":120)", ""), swap(R"("la":120)", R"("la":120,"x":1)"),
             swap(R"("p":"codex")", R"("p":"Codex")"), swap(R"("p":"codex")", R"("p":"co dex")"), swap(R"("plan":"plus")", R"("plan":"abcdefghijklmnopq")"),
             swap(R"("p":"codex")", R"("p":7)"),
             swap(R"("w":[{"m":300,"r":100,"s":15120},)", R"("w":[{"m":300,"r":100,"s":15120},{"m":300,"r":100,"s":15120},)"),
             swap(R"({"m":300,"r":100,"s":15120})", R"({"m":300,"r":100})"), swap(R"({"m":300,"r":100,"s":15120})", R"({"m":300,"r":100,"s":15120,"x":0})"),
             swap(R"("m":300)", R"("m":0)"), swap(R"("m":300)", R"("m":525601)"), swap(R"("r":100)", R"("r":101)"), swap(R"("r":100)", R"("r":-1)"),
             swap(R"("r":100)", R"("r":99.5)"), swap(R"("s":15120)", R"("s":31536001)"), swap(R"("w":[)", R"("w":{"a":)"),
             swap(R"("e":2)", R"("e":129)"), swap(R"("c":128)", R"("c":-1)"), swap(R"("c":128)", R"("c":1.5)"),
             swap(R"("t":1234567)", R"("t":1000000000000001)"), swap(R"("f":820)", R"("f":600001)"),
             swap(R"("lm":"gpt-6.1-sol")", R"("lm":"abcdefghijabcdefghijabcdefghijabc")"), swap(R"("lm":"gpt-6.1-sol")", R"("lm":"a\nb")"),
             swap(R"("ls":200)", R"("ls":1000)"), swap(R"("lms":3200)", R"("lms":3600001)"), swap(R"("la":120)", R"("la":31536001)"),
             swap(R"("lm":"gpt-6.1-sol")", R"("lm":null)")}) {
        const bool refused = bad(s);
        if (!refused) std::fprintf(stderr, "  that was: %s\n", s.c_str());
        CHECK(refused);
    }
}
int main() {
    b6_current();
    b7_usage();
    b1_parse();
    b2_list();
    b3_requests();
    b4_pairing();
    b5_out();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::puts("ble_protocol_test: all checks passed");
    return 0;
}
