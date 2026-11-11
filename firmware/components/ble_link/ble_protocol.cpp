#include "ble_protocol.hpp"
#include "json_util.hpp"
#include <cmath>
#include <cstring>
#include <set>

namespace dh::ble {
namespace {
bool good_text(const std::string &s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        uint32_t cp = 0;
        if (c < 0x80) { if (c < 0x20 || c == 0x7f) return false; ++i; continue; }
        if ((c >> 5) == 6) { n = 2; cp = c & 0x1f; }
        else if ((c >> 4) == 14) { n = 3; cp = c & 0x0f; }
        else if ((c >> 3) == 30) { n = 4; cp = c & 0x07; }
        else return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d >> 6) != 2) return false;
            cp = (cp << 6) | (d & 0x3f);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += n;
    }
    return true;
}
bool good_port(const std::string &p) {
    return p == "dp1" || p == "dp2" || p == "hdmi1" || p == "hdmi2" || p == "usbc" || p == "other";
}
// A whole number in [lo, hi]; nothing else.
bool integer(cJSON *item, double lo, double hi, double *out) {
    if (!cJSON_IsNumber(item)) return false;
    const double v = item->valuedouble;
    if (!(v >= lo && v <= hi) || std::floor(v) != v) return false;
    *out = v;
    return true;
}
const char *text(cJSON *item) { return cJSON_IsString(item) && item->valuestring ? item->valuestring : nullptr; }
Message bad(const char *why) { Message m; m.op = Op::Bad; m.error = why; return m; }
// exactly these keys
bool keys(cJSON *o, std::initializer_list<const char *> want) {
    if (cJSON_GetArraySize(o) != static_cast<int>(want.size())) return false;
    for (const char *k : want) if (!cJSON_GetObjectItemCaseSensitive(o, k)) return false;
    return true;
}
std::string escaped(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += c;
    }
    return out;
}
}  // namespace

Message parse(const std::string &bytes) {
    if (bytes.empty() || bytes.size() > kMaxMessage) return bad("size");
    cJSON *j = parse_json_strict(bytes.c_str(), bytes.size());
    if (!j) return bad("not json");
    struct Guard { cJSON *j; ~Guard() { cJSON_Delete(j); } } guard{j};
    if (!cJSON_IsObject(j)) return bad("not an object");
    const char *op = text(cJSON_GetObjectItemCaseSensitive(j, "op"));
    if (!op) return bad("no op");
    const std::string o = op;
    Message m;
    double v = 0;
    if (o == "sources_begin") {
        if (!keys(j, {"op", "count"}) || !integer(cJSON_GetObjectItemCaseSensitive(j, "count"), 0, kMaxSources, &v)) return bad("sources_begin fields");
        m.op = Op::SourcesBegin;
        m.count = static_cast<size_t>(v);
        return m;
    }
    if (o == "source") {
        if (!keys(j, {"op", "i", "id", "name", "port"}) || !integer(cJSON_GetObjectItemCaseSensitive(j, "i"), 0, kMaxSources - 1, &v)) return bad("source fields");
        const char *id = text(cJSON_GetObjectItemCaseSensitive(j, "id"));
        const char *name = text(cJSON_GetObjectItemCaseSensitive(j, "name"));
        const char *port = text(cJSON_GetObjectItemCaseSensitive(j, "port"));
        if (!id || !name || !port || !good_text(id, kMaxId) || !good_text(name, kMaxName) || !good_port(port)) return bad("source values");
        m.op = Op::Source;
        m.index = static_cast<size_t>(v);
        m.source = {id, name, port};
        return m;
    }
    if (o == "sources_end") {
        if (!keys(j, {"op"})) return bad("sources_end fields");
        m.op = Op::SourcesEnd;
        return m;
    }
    if (o == "result") {
        if (!keys(j, {"op", "req", "outcome", "detail"}) || !integer(cJSON_GetObjectItemCaseSensitive(j, "req"), 1, 4294967295.0, &v)) return bad("result fields");
        const char *outcome = text(cJSON_GetObjectItemCaseSensitive(j, "outcome"));
        const char *detail = text(cJSON_GetObjectItemCaseSensitive(j, "detail"));
        if (!outcome || !detail) return bad("result values");
        const std::string d = detail;
        if (d.size() > 120 || (!d.empty() && !good_text(d, 120))) return bad("result detail");
        const std::string oc = outcome;
        if (oc == "confirmed") m.result.outcome = Outcome::Confirmed;
        else if (oc == "sent_unconfirmed") m.result.outcome = Outcome::SentUnconfirmed;
        else if (oc == "not_sent") m.result.outcome = Outcome::NotSent;
        else return bad("result outcome");
        m.op = Op::Result;
        m.result.req = static_cast<uint32_t>(v);
        m.result.detail = d;
        return m;
    }
    if (o == "current") {
        if (!keys(j, {"op", "id"})) return bad("current fields");
        const char *id = text(cJSON_GetObjectItemCaseSensitive(j, "id"));
        if (!id || (id[0] != '\0' && !good_text(id, kMaxId))) return bad("current id");
        m.op = Op::Current;
        m.current = id;
        return m;
    }
    if (o == "usage") {   // 13.12
        cJSON *okj = cJSON_GetObjectItemCaseSensitive(j, "ok");
        if (!cJSON_IsBool(okj)) return bad("usage ok");
        m.op = Op::Usage;
        if (cJSON_IsFalse(okj)) {
            if (!keys(j, {"op", "ok"})) return bad("usage fields");
            return m;
        }
        if (!keys(j, {"op", "ok", "p", "plan", "w", "c", "e", "t", "f", "lm", "ls", "lms", "la"})) return bad("usage fields");
        Usage &u = m.usage;
        u.ok = true;
        auto name = [](cJSON *item, std::string *out) {
            const char *s = text(item);
            if (!s || strlen(s) > 16) return false;
            for (const char *c = s; *c; ++c)
                if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-')) return false;
            *out = s;
            return true;
        };
        if (!name(cJSON_GetObjectItemCaseSensitive(j, "p"), &u.provider) || !name(cJSON_GetObjectItemCaseSensitive(j, "plan"), &u.plan)) return bad("usage names");
        cJSON *w = cJSON_GetObjectItemCaseSensitive(j, "w");
        if (!cJSON_IsArray(w) || cJSON_GetArraySize(w) > 2) return bad("usage windows");
        cJSON *item;
        cJSON_ArrayForEach(item, w) {
            double mins = 0, rem = 0, reset = 0;
            if (!cJSON_IsObject(item) || !keys(item, {"m", "r", "s"}) || !integer(cJSON_GetObjectItemCaseSensitive(item, "m"), 1, 525600, &mins) ||
                !integer(cJSON_GetObjectItemCaseSensitive(item, "r"), 0, 100, &rem) || !integer(cJSON_GetObjectItemCaseSensitive(item, "s"), 0, 31536000, &reset))
                return bad("usage window");
            u.windows.push_back({static_cast<uint32_t>(mins), static_cast<uint32_t>(rem), static_cast<uint32_t>(reset)});
        }
        double calls = 0, errors = 0, tokens = 0, ttft = 0, status = 0, ms = 0, age = 0;
        if (!integer(cJSON_GetObjectItemCaseSensitive(j, "c"), 0, 1e9, &calls) || !integer(cJSON_GetObjectItemCaseSensitive(j, "e"), 0, calls, &errors) ||
            !integer(cJSON_GetObjectItemCaseSensitive(j, "t"), 0, 1e15, &tokens) || !integer(cJSON_GetObjectItemCaseSensitive(j, "f"), 0, 600000, &ttft) ||
            !integer(cJSON_GetObjectItemCaseSensitive(j, "ls"), 0, 999, &status) || !integer(cJSON_GetObjectItemCaseSensitive(j, "lms"), 0, 3600000, &ms) ||
            !integer(cJSON_GetObjectItemCaseSensitive(j, "la"), 0, 31536000, &age))
            return bad("usage numbers");
        const char *model = text(cJSON_GetObjectItemCaseSensitive(j, "lm"));
        if (!model || (model[0] != '\0' && !good_text(model, 32))) return bad("usage model");
        u.calls = static_cast<uint32_t>(calls);
        u.errors = static_cast<uint32_t>(errors);
        u.tokens = static_cast<uint64_t>(tokens);
        u.ttft_ms = static_cast<uint32_t>(ttft);
        u.latest_model = model;
        u.latest_status = static_cast<uint32_t>(status);
        u.latest_ms = static_cast<uint32_t>(ms);
        u.latest_age_s = static_cast<uint32_t>(age);
        return m;
    }
    return bad("unknown op");
}

std::string current_in(const std::vector<Source> &sources, const std::string &id) {
    if (id.empty()) return "";
    for (const auto &s : sources) if (s.id == id) return id;
    return "";
}

Apply apply(SourcesDraft &d, const Message &m, std::vector<Source> *complete) {
    auto reject = [&] { d = SourcesDraft{}; return Apply::Rejected; };
    switch (m.op) {
    case Op::SourcesBegin:
        if (d.open) return reject();
        d.open = true;
        d.expected = m.count;
        d.items.assign(m.count, std::nullopt);
        return Apply::Pending;
    case Op::Source:
        if (!d.open || m.index >= d.expected || d.items[m.index].has_value()) return reject();
        for (const auto &s : d.items) if (s && s->id == m.source.id) return reject();
        d.items[m.index] = m.source;
        return Apply::Pending;
    case Op::SourcesEnd: {
        if (!d.open) return reject();
        std::vector<Source> out;
        for (const auto &s : d.items) {
            if (!s) return reject();
            out.push_back(*s);
        }
        *complete = out;
        d = SourcesDraft{};
        return Apply::Complete;
    }
    default:
        return Apply::Pending;
    }
}

bool Requests::begin(const std::string &id, const std::vector<Source> &sources, int64_t now_ms, uint32_t *req) {
    if (pending_) return false;
    bool known = false;
    for (const auto &s : sources) known = known || s.id == id;
    if (!known) return false;
    pending_ = Pending{next_++, id, now_ms};
    if (next_ == 0) next_ = 1;
    *req = pending_->req;
    return true;
}
std::optional<std::string> Requests::finish(const Result &r) {
    if (!pending_ || pending_->req != r.req) return std::nullopt;
    std::string id = pending_->id;
    pending_.reset();
    return id;
}
bool Requests::expire(int64_t now_ms) {
    if (!pending_ || now_ms - pending_->sent_ms < kRequestTimeoutMs) return false;
    pending_.reset();
    return true;
}

bool accept_pairing(const PairingWindow &w, int64_t now_ms, int64_t digits_shown_ms, bool person_said_same) {
    return person_said_same && w.is_open(now_ms) && now_ms - digits_shown_ms < kConfirmMs;
}

std::string request_json(uint32_t req, const std::string &id) {
    return "{\"op\":\"switch\",\"req\":" + std::to_string(req) + ",\"id\":\"" + escaped(id) + "\"}";
}
std::string info_json(const std::string &device_id, const std::string &firmware, const std::string &update_key, bool boot_pending) {
    return "{\"protocol\":" + std::to_string(kProtocol) + ",\"device\":\"" + escaped(device_id) + "\",\"firmware\":\"" + escaped(firmware) +
           "\",\"update_key\":\"" + escaped(update_key) + "\",\"boot\":\"" + (boot_pending ? "pending" : "confirmed") + "\"}";
}
std::string result_text(const Result &r, const std::string &name) {
    switch (r.outcome) {
    case Outcome::Confirmed: return "已切到 " + name + "，已确认";
    case Outcome::SentUnconfirmed: return "已发送到 " + name + "，未确认" + (r.detail.empty() ? "" : "：" + r.detail);
    case Outcome::NotSent: break;
    }
    return "没有切到 " + name + (r.detail.empty() ? "" : "：" + r.detail);
}
}  // namespace dh::ble
