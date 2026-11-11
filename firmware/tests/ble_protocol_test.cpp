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
// B8  The to-do list (docs/desktop-app-design.md 13.13 L1): todos_begin {n 0..30, total n..9999}; todo {i, id of exactly 12
//     lowercase hex digits, t 1..96 bytes, l 0..32 bytes (valid UTF-8, no control characters), k one of o t m l n, m 1..12,
//     d 1..31, hm -1..1439; k n exactly when m, d, hm are 0, 0, -1}; todos_end; todos_off {why access|setting};
//     todo_result {req >= 1, ok bool, detail 0..64 bytes}. Exactly these fields, whole numbers only.
// B9  A to-do list replaces the shown one only when complete (L2): the same rules as B2, plus unique ids and the total.
// B11 The to-do fields of 13.14 (M1): y 2000..2100 (0 exactly when undated), a real date (no 30 February), r one of "" wd x
//     or d/w/m/y with 1..99 (no leading zero), li 12 lowercase hex digits, a a bool that is false without a time, v 8 lowercase hex
//     digits. The list of lists: todo_lists_begin {n 0..16}, todo_list {i, id 12 hex, name 1..32 bytes}, todo_lists_end; complete
//     or nothing, ids unique.
// B12 todo_edit goes out with exactly op, req, id, v, y, m, d, hm, r, li, a; an edit counts as one action (one at a time, only for a
//     shown id), and its result ends it like the others.
// B10 One to-do action at a time, only for an id in the shown list (L10); the result counts only for the waiting action;
//     after kRequestTimeoutMs it is over, once (L11). The todo_act message has exactly op, req, id, a.
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

static const char *kTodo = R"({"op":"todo","i":0,"id":"9f2c4e1a0b7d","t":"交季度报告","l":"工作","k":"o","y":2026,"m":11,"d":10,"hm":-1,"r":"","li":"a1b2c3d4e5f6","a":false,"v":"0a1b2c3d"})";
static std::string tswap(const std::string &from, const std::string &to) {
    std::string s = kTodo;
    const size_t at = s.find(from);
    if (at == std::string::npos) { std::fprintf(stderr, "no [%s] in the to-do sample\n", from.c_str()); return s; }
    return s.replace(at, from.size(), to);
}
static void b8_todo_parse() {
    Message m = P(R"({"op":"todos_begin","n":3,"total":9})");
    CHECK(m.op == Op::TodosBegin && m.count == 3 && m.total == 9);
    CHECK(P(R"({"op":"todos_begin","n":0,"total":0})").op == Op::TodosBegin);
    CHECK(P(R"({"op":"todos_begin","n":30,"total":9999})").op == Op::TodosBegin);
    m = P(kTodo);
    CHECK(m.op == Op::Todo && m.index == 0 && m.todo.id == "9f2c4e1a0b7d" && m.todo.title == "交季度报告" && m.todo.list == "工作");
    CHECK(m.todo.kind == TodoKind::Overdue && m.todo.month == 11 && m.todo.day == 10 && m.todo.minute == -1);
    m = P(tswap(R"("k":"o","y":2026,"m":11,"d":10,"hm":-1)", R"("k":"t","y":2026,"m":11,"d":11,"hm":870)"));
    CHECK(m.op == Op::Todo && m.todo.kind == TodoKind::Today && m.todo.minute == 870);
    CHECK(P(tswap(R"("k":"o")", R"("k":"m")")).todo.kind == TodoKind::Tomorrow);
    CHECK(P(tswap(R"("k":"o")", R"("k":"l")")).todo.kind == TodoKind::Later);
    m = P(tswap(R"("k":"o","y":2026,"m":11,"d":10,"hm":-1)", R"("k":"n","y":0,"m":0,"d":0,"hm":-1)"));
    CHECK(m.op == Op::Todo && m.todo.kind == TodoKind::None && m.todo.month == 0);
    CHECK(P(tswap(R"("l":"工作")", R"("l":"")")).todo.list.empty());
    CHECK(P(tswap(R"("hm":-1)", R"("hm":1439)")).op == Op::Todo);
    const std::string t96(96, 'a');
    CHECK(P(tswap("交季度报告", t96)).todo.title == t96);
    CHECK(P(R"({"op":"todos_end"})").op == Op::TodosEnd);
    m = P(R"({"op":"todos_off","why":"access"})");
    CHECK(m.op == Op::TodosOff && m.off_why == "access");
    CHECK(P(R"({"op":"todos_off","why":"setting"})").off_why == "setting");
    m = P(R"({"op":"todo_result","req":5,"ok":true,"detail":""})");
    CHECK(m.op == Op::TodoResult && m.todo_result.req == 5 && m.todo_result.ok && m.todo_result.detail.empty());
    m = P(R"({"op":"todo_result","req":6,"ok":false,"detail":"这条待办已经变了"})");
    CHECK(m.op == Op::TodoResult && !m.todo_result.ok && m.todo_result.detail == "这条待办已经变了");
    for (const std::string &s : {
             std::string(R"({"op":"todos_begin","n":31,"total":40})"), std::string(R"({"op":"todos_begin","n":3,"total":2})"),
             std::string(R"({"op":"todos_begin","n":3,"total":10000})"), std::string(R"({"op":"todos_begin","n":3})"),
             std::string(R"({"op":"todos_begin","n":-1,"total":0})"), std::string(R"({"op":"todos_begin","n":1.5,"total":2})"),
             std::string(R"({"op":"todos_begin","n":1,"total":1,"x":0})"), std::string(R"({"op":"todos_end","n":1})"),
             std::string(R"({"op":"todos_off","why":"other"})"), std::string(R"({"op":"todos_off"})"),
             std::string(R"({"op":"todo_result","req":0,"ok":true,"detail":""})"), std::string(R"({"op":"todo_result","req":5,"ok":1,"detail":""})"),
             std::string(R"({"op":"todo_result","req":5,"ok":true})"), std::string(R"({"op":"todo_result","req":5,"ok":true,"detail":"a\nb"})"),
             std::string(R"({"op":"todo_result","req":5,"ok":true,"detail":")") + std::string(65, 'x') + "\"}",
             tswap(R"("id":"9f2c4e1a0b7d")", R"("id":"9F2C4E1A0B7D")"), tswap(R"("id":"9f2c4e1a0b7d")", R"("id":"9f2c4e1a0b7")"),
             tswap(R"("id":"9f2c4e1a0b7d")", R"("id":"9f2c4e1a0b7dd")"), tswap(R"("id":"9f2c4e1a0b7d")", R"("id":"9f2c4e1a0b7g")"),
             tswap(R"("t":"交季度报告")", R"("t":"")"), tswap("交季度报告", std::string(97, 'a')), tswap("交季度报告", "a\tb"),
             tswap("交季度报告", "\xff\xfe"), tswap(R"("l":"工作")", R"("l":")" + std::string(33, 'a') + "\""),
             tswap(R"("k":"o")", R"("k":"x")"), tswap(R"("k":"o")", R"("k":"n")"),
             tswap(R"("k":"o","y":2026,"m":11,"d":10,"hm":-1)", R"("k":"n","y":0,"m":0,"d":0,"hm":60)"),
             tswap(R"("k":"o","y":2026,"m":11,"d":10,"hm":-1)", R"("k":"n","y":0,"m":11,"d":0,"hm":-1)"),
             tswap(R"("m":11)", R"("m":0)"), tswap(R"("m":11)", R"("m":13)"), tswap(R"("d":10)", R"("d":0)"), tswap(R"("d":10)", R"("d":32)"),
             tswap(R"("hm":-1)", R"("hm":-2)"), tswap(R"("hm":-1)", R"("hm":1440)"), tswap(R"("hm":-1)", R"("hm":1.5)"),
             tswap(R"("i":0)", R"("i":30)"), tswap(R"("i":0)", R"("i":-1)"), tswap(R"("i":0,)", ""),
             tswap(R"("v":"0a1b2c3d"})", R"("v":"0a1b2c3d","x":1})"), tswap(R"("t":"交季度报告")", R"("t":1)")}) {
        const bool refused = bad(s);
        if (!refused) std::fprintf(stderr, "  that was: %s\n", s.c_str());
        CHECK(refused);
    }
}
static Message tbegin(size_t n, size_t total) { Message m; m.op = Op::TodosBegin; m.count = n; m.total = total; return m; }
static Message todo(size_t i, const std::string &id) { Message m; m.op = Op::Todo; m.index = i; m.todo.id = id; m.todo.title = "t" + id; m.todo.kind = TodoKind::None; return m; }
static Message tend() { Message m; m.op = Op::TodosEnd; return m; }
static void b9_todo_list() {
    TodosDraft d;
    TodoList out;
    out.items.push_back(todo(0, "000000000000").todo);
    out.total = 1;
    CHECK(apply(d, tbegin(2, 7), &out) == Apply::Pending);
    CHECK(apply(d, todo(1, "bbbbbbbbbbbb"), &out) == Apply::Pending);
    CHECK(apply(d, todo(0, "aaaaaaaaaaaa"), &out) == Apply::Pending);
    CHECK(out.items.size() == 1 && out.items[0].id == "000000000000");   // not yet
    CHECK(apply(d, tend(), &out) == Apply::Complete);
    CHECK(out.items.size() == 2 && out.items[0].id == "aaaaaaaaaaaa" && out.items[1].id == "bbbbbbbbbbbb" && out.total == 7);
    auto rejected = [&](std::vector<Message> seq) {
        TodosDraft x;
        TodoList keep;
        keep.items.push_back(todo(0, "kkkkkkkkkkkk").todo);
        keep.total = 1;
        Apply last = Apply::Pending;
        for (const auto &m : seq) { last = apply(x, m, &keep); if (last == Apply::Rejected) break; }
        return last == Apply::Rejected && keep.items.size() == 1 && keep.items[0].id == "kkkkkkkkkkkk" && keep.total == 1;
    };
    CHECK(rejected({tbegin(2, 2), todo(0, "aaaaaaaaaaaa"), todo(0, "cccccccccccc")}));   // an index twice
    CHECK(rejected({tbegin(2, 2), todo(0, "aaaaaaaaaaaa"), todo(2, "cccccccccccc")}));   // out of range
    CHECK(rejected({todo(0, "aaaaaaaaaaaa")}));                                          // no begin
    CHECK(rejected({tbegin(2, 2), todo(0, "aaaaaaaaaaaa"), tend()}));                    // end too early
    CHECK(rejected({tbegin(2, 2), todo(0, "aaaaaaaaaaaa"), tbegin(1, 1)}));              // a second begin
    CHECK(rejected({tend()}));                                                           // end without begin
    CHECK(rejected({tbegin(2, 2), todo(0, "aaaaaaaaaaaa"), todo(1, "aaaaaaaaaaaa")}));   // the same id twice
    TodosDraft e;
    TodoList none;
    none.items.push_back(todo(0, "kkkkkkkkkkkk").todo);
    CHECK(apply(e, tbegin(0, 0), &none) == Apply::Pending && apply(e, tend(), &none) == Apply::Complete && none.items.empty() && none.total == 0);
    // a source message does not touch a to-do draft (as B2: other ops are ignored)
    TodosDraft f;
    TodoList l;
    CHECK(apply(f, tbegin(1, 1), &l) == Apply::Pending && apply(f, begin(1), &l) == Apply::Pending && f.open);
    CHECK(apply(f, todo(0, "aaaaaaaaaaaa"), &l) == Apply::Pending && apply(f, tend(), &l) == Apply::Complete && l.items.size() == 1);
}
static void b10_todo_acts() {
    std::vector<Todo> list{todo(0, "aaaaaaaaaaaa").todo, todo(1, "bbbbbbbbbbbb").todo};
    TodoActs q;
    uint32_t r1 = 0, r2 = 0, r3 = 0;
    CHECK(!q.begin("cccccccccccc", TodoAction::Done, list, 0, &r1));        // not in the list
    CHECK(q.begin("aaaaaaaaaaaa", TodoAction::Delete, list, 0, &r1) && r1 != 0 && q.waiting());
    CHECK(!q.begin("bbbbbbbbbbbb", TodoAction::Done, list, 100, &r2));      // one at a time
    TodoResult late; late.req = r1 + 5; late.ok = true;
    CHECK(!q.finish(late).has_value() && q.waiting());
    TodoResult ok; ok.req = r1; ok.ok = true;
    auto done = q.finish(ok);
    CHECK(done.has_value() && done->id == "aaaaaaaaaaaa" && done->action == TodoAction::Delete && !q.waiting());
    CHECK(!q.finish(ok).has_value());                                       // repeated
    CHECK(q.begin("bbbbbbbbbbbb", TodoAction::Tomorrow, list, 1000, &r2) && r2 != r1);
    CHECK(!q.expire(1000 + kRequestTimeoutMs - 1));
    CHECK(q.expire(1000 + kRequestTimeoutMs) && !q.waiting());
    CHECK(!q.expire(1000 + kRequestTimeoutMs + 10));
    TodoResult stale; stale.req = r2; stale.ok = true;
    CHECK(!q.finish(stale).has_value());
    CHECK(q.begin("aaaaaaaaaaaa", TodoAction::Done, list, 20000, &r3) && r3 != r2 && r3 != r1);
    q.reset();
    CHECK(!q.waiting());
    CHECK(!q.begin("aaaaaaaaaaaa", TodoAction::Done, {}, 30000, &r3));
    // what goes out
    for (auto [a, word] : {std::pair{TodoAction::Done, "done"}, std::pair{TodoAction::Tomorrow, "tomorrow"}, std::pair{TodoAction::Delete, "delete"}}) {
        const std::string j = todo_act_json(9, "aaaaaaaaaaaa", a);
        cJSON *o = dh::parse_json_strict(j.c_str(), j.size());
        CHECK(o && cJSON_GetArraySize(o) == 4);
        CHECK(o && std::string(cJSON_GetObjectItemCaseSensitive(o, "op")->valuestring) == "todo_act");
        CHECK(o && cJSON_GetObjectItemCaseSensitive(o, "req")->valuedouble == 9);
        CHECK(o && std::string(cJSON_GetObjectItemCaseSensitive(o, "id")->valuestring) == "aaaaaaaaaaaa");
        CHECK(o && std::string(cJSON_GetObjectItemCaseSensitive(o, "a")->valuestring) == word);
        cJSON_Delete(o);
    }
}

static void b11_todo_fields() {
    Message m = P(kTodo);
    CHECK(m.op == Op::Todo && m.todo.year == 2026 && m.todo.repeat.empty() && m.todo.list_id == "a1b2c3d4e5f6" && !m.todo.alarm && m.todo.version == "0a1b2c3d");
    m = P(tswap(R"("k":"o","y":2026,"m":11,"d":10,"hm":-1,"r":"","li":"a1b2c3d4e5f6","a":false)", R"("k":"t","y":2026,"m":11,"d":12,"hm":870,"r":"w2","li":"a1b2c3d4e5f6","a":true)"));
    CHECK(m.op == Op::Todo && m.todo.alarm && m.todo.repeat == "w2" && m.todo.minute == 870);
    for (const char *r : {"wd", "x", "d1", "w99", "m12", "y1"}) CHECK(P(tswap(R"("r":"")", std::string("\"r\":\"") + r + "\"")).todo.repeat == r);
    CHECK(P(tswap(R"("y":2026,"m":11,"d":10)", R"("y":2028,"m":2,"d":29)")).op == Op::Todo);   // a leap day
    for (const std::string &s : {
             tswap(R"("y":2026,"m":11,"d":10)", R"("y":2026,"m":2,"d":29)"), tswap(R"("y":2026,"m":11,"d":10)", R"("y":2026,"m":4,"d":31)"),
             tswap(R"("y":2026)", R"("y":1999)"), tswap(R"("y":2026)", R"("y":2101)"), tswap(R"("y":2026)", R"("y":0)"),
             tswap(R"("r":"")", R"("r":"w0")"), tswap(R"("r":"")", R"("r":"w100")"), tswap(R"("r":"")", R"("r":"w05")"), tswap(R"("r":"")", R"("r":"q1")"),
             tswap(R"("r":"")", R"("r":"wdx")"), tswap(R"("r":"")", R"("r":1)"), tswap(R"("li":"a1b2c3d4e5f6")", R"("li":"A1B2C3D4E5F6")"),
             tswap(R"("li":"a1b2c3d4e5f6")", R"("li":"a1b2")"), tswap(R"("a":false)", R"("a":true)"),   // all day: no ring
             tswap(R"("a":false)", R"("a":0)"), tswap(R"("v":"0a1b2c3d")", R"("v":"0a1b2c3")"), tswap(R"("v":"0a1b2c3d")", R"("v":"0A1B2C3D")"),
             tswap(R"("k":"o","y":2026,"m":11,"d":10,"hm":-1,"r":"","li":"a1b2c3d4e5f6","a":false)", R"("k":"n","y":0,"m":0,"d":0,"hm":-1,"r":"","li":"a1b2c3d4e5f6","a":true)"),
             tswap(R"(,"v":"0a1b2c3d")", "")}) {
        const bool refused = bad(s);
        if (!refused) std::fprintf(stderr, "  that was: %s\n", s.c_str());
        CHECK(refused);
    }
    // the list of lists
    m = P(R"({"op":"todo_lists_begin","n":2})");
    CHECK(m.op == Op::TodoListsBegin && m.count == 2);
    m = P(R"({"op":"todo_list","i":1,"id":"a1b2c3d4e5f6","name":"工作"})");
    CHECK(m.op == Op::TodoListItem && m.index == 1 && m.list_entry.id == "a1b2c3d4e5f6" && m.list_entry.name == "工作");
    CHECK(P(R"({"op":"todo_lists_end"})").op == Op::TodoListsEnd);
    for (const std::string &s : {std::string(R"({"op":"todo_lists_begin","n":17})"), std::string(R"({"op":"todo_list","i":16,"id":"a1b2c3d4e5f6","name":"x"})"),
                                 std::string(R"({"op":"todo_list","i":0,"id":"a1b2c3d4e5f6","name":""})"), std::string(R"({"op":"todo_list","i":0,"id":"zz","name":"x"})"),
                                 std::string(R"({"op":"todo_list","i":0,"id":"a1b2c3d4e5f6","name":")") + std::string(33, 'a') + "\"}",
                                 std::string(R"({"op":"todo_lists_end","n":0})")}) CHECK(bad(s));
    ListsDraft d;
    std::vector<TodoListName> out{{"000000000000", "旧"}};
    auto lbegin = [](size_t n) { Message x; x.op = Op::TodoListsBegin; x.count = n; return x; };
    auto litem = [](size_t i, const char *id) { Message x; x.op = Op::TodoListItem; x.index = i; x.list_entry = {id, "n"}; return x; };
    Message lend; lend.op = Op::TodoListsEnd;
    CHECK(apply(d, lbegin(2), &out) == Apply::Pending && apply(d, litem(1, "bbbbbbbbbbbb"), &out) == Apply::Pending && apply(d, litem(0, "aaaaaaaaaaaa"), &out) == Apply::Pending);
    CHECK(out.size() == 1 && apply(d, lend, &out) == Apply::Complete && out.size() == 2 && out[0].id == "aaaaaaaaaaaa");
    ListsDraft e;
    std::vector<TodoListName> keep{{"kkkkkkkkkkkk", "k"}};
    CHECK(apply(e, lbegin(2), &keep) == Apply::Pending && apply(e, litem(0, "aaaaaaaaaaaa"), &keep) == Apply::Pending && apply(e, litem(1, "aaaaaaaaaaaa"), &keep) == Apply::Rejected);
    CHECK(keep.size() == 1 && keep[0].id == "kkkkkkkkkkkk");
    ListsDraft f;
    CHECK(apply(f, lbegin(1), &keep) == Apply::Pending && apply(f, lend, &keep) == Apply::Rejected && keep.size() == 1);   // end too early
}
static void b12_todo_edit() {
    TodoEdit e;
    e.id = "aaaaaaaaaaaa"; e.version = "0a1b2c3d"; e.year = 2026; e.month = 11; e.day = 12; e.minute = 870; e.repeat = "w1"; e.list_id = "a1b2c3d4e5f6"; e.alarm = true;
    const std::string j = todo_edit_json(7, e);
    cJSON *o = dh::parse_json_strict(j.c_str(), j.size());
    CHECK(o && cJSON_GetArraySize(o) == 11);
    auto str = [&](const char *k) { cJSON *x = o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr; return x && cJSON_IsString(x) ? std::string(x->valuestring) : std::string("?"); };
    auto num = [&](const char *k) { cJSON *x = o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr; return x && cJSON_IsNumber(x) ? x->valuedouble : -99.0; };
    CHECK(str("op") == "todo_edit" && num("req") == 7 && str("id") == "aaaaaaaaaaaa" && str("v") == "0a1b2c3d");
    CHECK(num("y") == 2026 && num("m") == 11 && num("d") == 12 && num("hm") == 870 && str("r") == "w1" && str("li") == "a1b2c3d4e5f6");
    CHECK(o && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "a")));
    cJSON_Delete(o);
    TodoEdit none = e; none.year = 0; none.month = 0; none.day = 0; none.minute = -1; none.alarm = false; none.repeat = "";
    const std::string k = todo_edit_json(8, none);
    cJSON *p = dh::parse_json_strict(k.c_str(), k.size());
    CHECK(p && cJSON_GetObjectItemCaseSensitive(p, "y")->valuedouble == 0 && cJSON_GetObjectItemCaseSensitive(p, "hm")->valuedouble == -1 && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "a")));
    cJSON_Delete(p);
    CHECK(valid_edit(e) && valid_edit(none));
    TodoEdit bad1 = e; bad1.minute = -1;                           // all day with a ring
    TodoEdit bad2 = e; bad2.day = 31; bad2.month = 11;             // 31 November
    TodoEdit bad3 = e; bad3.repeat = "w0";
    TodoEdit bad4 = none; bad4.alarm = true;
    TodoEdit bad5 = e; bad5.list_id = "x";
    CHECK(!valid_edit(bad1) && !valid_edit(bad2) && !valid_edit(bad3) && !valid_edit(bad4) && !valid_edit(bad5));
    // an edit is an action: one at a time, only a shown id
    Todo shown; shown.id = "aaaaaaaaaaaa";
    TodoActs q;
    uint32_t r1 = 0, r2 = 0;
    CHECK(q.begin("aaaaaaaaaaaa", TodoAction::Edit, {shown}, 0, &r1) && !q.begin("aaaaaaaaaaaa", TodoAction::Done, {shown}, 1, &r2));
    TodoResult ok; ok.req = r1; ok.ok = true;
    auto done = q.finish(ok);
    CHECK(done && done->action == TodoAction::Edit);
}
int main() {
    b11_todo_fields();
    b12_todo_edit();
    b8_todo_parse();
    b9_todo_list();
    b10_todo_acts();
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
