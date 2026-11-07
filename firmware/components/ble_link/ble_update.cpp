// Firmware update over Bluetooth: the pure part (ble_update.hpp; docs/desktop-app-design.md 13.11).
#include "ble_update.hpp"
#include <cstring>
#include <vector>

namespace dh::ble::update {

Frame parse_frame(const uint8_t *p, size_t n) {
    Frame f;
    if (!p || n == 0) return f;
    switch (p[0]) {
    case 'B': if (n == 1) f.kind = Kind::Begin; break;
    case 'E': if (n == 1) f.kind = Kind::End; break;
    case 'X': if (n == 1) f.kind = Kind::Abort; break;
    case 'M':
        if (n > 3) { f.kind = Kind::Manifest; f.offset = static_cast<uint32_t>(p[1]) | static_cast<uint32_t>(p[2]) << 8; f.bytes = p + 3; f.len = n - 3; }
        break;
    case 'D':
        if (n > 5) {
            f.kind = Kind::Data;
            f.offset = static_cast<uint32_t>(p[1]) | static_cast<uint32_t>(p[2]) << 8 | static_cast<uint32_t>(p[3]) << 16 | static_cast<uint32_t>(p[4]) << 24;
            f.bytes = p + 5; f.len = n - 5;
        }
        break;
    default: break;
    }
    return f;
}

static bool numbers(const std::string &s, std::vector<unsigned long> &out) {
    out.clear();
    if (s.empty()) return false;
    size_t i = 0;
    for (;;) {
        size_t start = i;
        unsigned long v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (i - start >= 9) return false;
            v = v * 10 + static_cast<unsigned long>(s[i] - '0');
            ++i;
        }
        if (i == start) return false;
        out.push_back(v);
        if (i == s.size()) return true;
        if (s[i] != '.') return false;
        ++i;
    }
}
int compare_versions(const std::string &a, const std::string &b, bool *ok) {
    std::vector<unsigned long> x, y;
    const bool good = numbers(a, x) && numbers(b, y);
    if (ok) *ok = good;
    if (!good) return 0;
    const size_t n = std::max(x.size(), y.size());
    x.resize(n, 0);
    y.resize(n, 0);
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

bool image_prefix_ok(const uint8_t *p, size_t n, const std::string &version) {
    if (!p || n < kPrefix || p[0] != 0xE9 || (p[12] | p[13] << 8) != 9) return false;
    const uint32_t magic = static_cast<uint32_t>(p[32]) | static_cast<uint32_t>(p[33]) << 8 | static_cast<uint32_t>(p[34]) << 16 | static_cast<uint32_t>(p[35]) << 24;
    if (magic != 0xABCD5432) return false;
    const void *vend = std::memchr(p + 48, 0, 32), *pend = std::memchr(p + 80, 0, 32);
    if (!vend || !pend) return false;
    return std::string(reinterpret_cast<const char *>(p + 48)) == version && std::strcmp(reinterpret_cast<const char *>(p + 80), "deskhub") == 0;
}

Plan Session::abort_(const char *why) {
    reset();
    Plan p;
    p.what = Plan::Abort;
    p.why = why;
    return p;
}
static Plan refuse(const std::string &why) {
    Plan p;
    p.what = Plan::Refuse;
    p.why = why;
    return p;
}

Plan Session::frame(const Frame &f, int64_t now_ms, const Context &c) {
    const bool active = phase_ != Phase::Idle;
    last_ = now_ms;
    switch (f.kind) {
    case Kind::Bad: return active ? abort_("UPDATE_BAD_FRAME") : Plan{};
    case Kind::Abort: return active ? abort_("UPDATE_CANCELLED") : Plan{};
    case Kind::Manifest:
        if (phase_ != Phase::Idle && phase_ != Phase::Manifest) return abort_("UPDATE_OUT_OF_ORDER");
        if (f.offset != buffer_.size()) return abort_("UPDATE_OUT_OF_ORDER");
        if (buffer_.size() + f.len > kMaxManifest) return abort_("UPDATE_MANIFEST_TOO_LONG");
        buffer_.append(reinterpret_cast<const char *>(f.bytes), f.len);
        phase_ = Phase::Manifest;
        return {};
    case Kind::Begin: {
        if (phase_ != Phase::Manifest) return active ? abort_("UPDATE_OUT_OF_ORDER") : refuse("UPDATE_NO_MANIFEST");
        const std::string envelope = buffer_;
        reset();
        if (!c.allowed) return refuse(c.not_allowed.empty() ? "UPDATE_BUSY" : c.not_allowed);
        if (!c.check) return refuse("UPDATE_SIGNATURE_INVALID");
        Checked k = c.check(envelope, c.check_ctx);
        if (!k.ok) return refuse(k.why.empty() ? "UPDATE_SIGNATURE_INVALID" : k.why);
        bool comparable = false;
        if (compare_versions(k.version, c.running, &comparable) <= 0 || !comparable) return refuse("UPDATE_NOT_NEWER");
        if (k.size < kPrefix) return refuse("UPDATE_TOO_SMALL");
        if (k.size > c.slot || k.size > 6u * 1024 * 1024) return refuse("UPDATE_TOO_LARGE");
        checked_ = k;
        phase_ = Phase::Opening;
        last_ = now_ms;
        Plan p;
        p.what = Plan::Open;
        return p;
    }
    case Kind::Data: {
        if (phase_ != Phase::Receiving) return abort_("UPDATE_NOT_READY");
        if (f.offset != received_) return abort_("UPDATE_OUT_OF_ORDER");
        if (received_ + f.len > checked_.size) return abort_("UPDATE_TOO_MUCH");
        received_ += f.len;
        Plan p;
        p.what = Plan::Write;
        p.bytes = f.bytes;
        p.len = f.len;
        return p;
    }
    case Kind::End:
        if (!active) return refuse("UPDATE_NO_MANIFEST");
        if (phase_ != Phase::Receiving || received_ != checked_.size) return abort_("UPDATE_SHORT");
        phase_ = Phase::Finishing;
        {
            Plan p;
            p.what = Plan::Finish;
            return p;
        }
    }
    return {};
}

Plan Session::tick(int64_t now_ms) {
    if (phase_ == Phase::Idle || phase_ == Phase::Finishing) return {};
    if (now_ms - last_ > kIdleMs) return abort_("UPDATE_IDLE");
    return {};
}

void Session::opened(bool ok) {
    if (phase_ != Phase::Opening) return;
    if (ok) { phase_ = Phase::Receiving; received_ = 0; }
    else reset();
}

void Session::reset() {
    phase_ = Phase::Idle;
    buffer_.clear();
    checked_ = Checked{};
    received_ = 0;
    last_ = 0;
}

}  // namespace dh::ble::update
