#include "pairing_session.hpp"
#include <algorithm>

namespace dh::pairing {
namespace {
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // 32 characters: five bits each, no bias
constexpr int kMaxRejections = 64;
bool same_credentials(const Secrets &a, const Secrets &b) {
    return a.ssid == b.ssid && a.password == b.password && a.url == b.url && a.agent_id == b.agent_id && a.token == b.token && a.ca_pem == b.ca_pem;
}
void wipe(std::string &s) {  // best effort: the strings do not outlive the session
    std::fill(s.begin(), s.end(), '\0');
    s.clear();
}
}  // namespace

bool random_digits(Random &rng, size_t count, std::string &out) {
    std::string digits;
    for (size_t i = 0; i < count; ++i) {
        bool got = false;
        for (int attempt = 0; attempt < kMaxRejections && !got; ++attempt) {
            uint8_t b;
            if (!rng.fill(&b, 1)) return false;
            if (b < 250) {  // 250 is a multiple of ten: bytes 250..255 would favour the low digits, so they are skipped
                digits += static_cast<char>('0' + b % 10);
                got = true;
            }
        }
        if (!got) return false;
    }
    out = digits;
    return true;
}
bool random_password(Random &rng, size_t count, std::string &out) {
    std::string password;
    for (size_t i = 0; i < count; ++i) {
        uint8_t b;
        if (!rng.fill(&b, 1)) return false;
        password += kAlphabet[b & 31];
    }
    out = password;
    return true;
}

// ---------------------------------------------------------------- the button
void LongPress::edge(bool pressed, int64_t now_ms) {
    if (pressed) {
        if (down_) return;
        if (had_press_ && now_ms >= released_ && now_ms - released_ < glitch_) down_ = true;  // a glitch, the same press goes on
        else { start_ = now_ms; fired_ = false; down_ = true; had_press_ = true; }
    } else {
        if (!down_) return;
        down_ = false;
        released_ = now_ms;
    }
}
bool LongPress::poll(int64_t now_ms) {
    if (!down_ || fired_ || now_ms < start_ || now_ms - start_ < hold_) return false;
    fired_ = true;
    return true;
}

// ---------------------------------------------------------------- the session
Session::Session(Random &random, Sink &sink, std::string device_id) : random_(random), sink_(sink), device_id_(std::move(device_id)) {}
Session::~Session() { clear_form(); }

void Session::clear_form() {
    wipe(form_.ssid); wipe(form_.wifi_password); wipe(form_.package); wipe(form_.utc);
    wipe(package_.url); wipe(package_.grant_id); wipe(package_.code);
    package_.ca_der.assign(package_.ca_der.size(), 0);
    package_.ca_der.clear();
    wipe(ca_pem_);
    wipe(reply_.agent_id); wipe(reply_.token); wipe(reply_.ca_pem);
    have_reply_ = false;
}

bool Session::holds_secrets() const {
    return !form_.ssid.empty() || !form_.wifi_password.empty() || !form_.package.empty() || !package_.code.empty() || !ca_pem_.empty() || have_reply_ || !reply_.token.empty();
}

Why Session::open(int64_t now_ms, bool paired, bool long_press) {
    if (is_open()) return Why::WrongPhase;
    // An unpaired device opens by itself once after boot; a paired one only on a long press. Nothing else opens it.
    if (!(long_press || (!paired && !auto_open_used_))) return Why::NotAllowed;
    std::string code, password;
    if (!random_digits(random_, kSetupCodeLength, code) || !random_password(random_, kHotspotPasswordLength, password)) return Why::RandomFailed;
    auto_open_used_ = true;
    clear_form();
    offer_ = {hotspot_name(device_id_), password, code};
    deadline_ = now_ms + kWindowMs;
    wrong_codes_ = attempts_ = 0;
    code_ok_ = false;
    phase_ = Phase::Open;
    why_ = Why::None;
    return Why::None;
}

void Session::close(Why why) {
    if (!is_open()) return;
    clear_form();
    wipe(offer_.hotspot_password); wipe(offer_.setup_code); wipe(offer_.hotspot_ssid);
    code_ok_ = false;
    phase_ = Phase::Closed;
    why_ = why;
}

int64_t Session::remaining_ms(int64_t now_ms) const {
    if (!is_open()) return 0;
    return std::max<int64_t>(0, std::min<int64_t>(deadline_ - now_ms, kWindowMs));
}

Why Session::expire(int64_t now_ms) {
    if (is_open() && now_ms >= deadline_) {
        close(Why::Timeout);
        return Why::Timeout;
    }
    return Why::None;
}
Why Session::tick(int64_t now_ms) { return expire(now_ms); }

Why Session::fail_attempt(Why why) {
    clear_form();
    if (++attempts_ >= kMaxAttempts) {
        close(Why::TooManyAttempts);
        return Why::TooManyAttempts;
    }
    phase_ = Phase::Open;  // the person may try again within the window
    why_ = why;
    return why;
}

Why Session::enter_code(int64_t now_ms, const std::string &code) {
    if (Why w = expire(now_ms); w != Why::None) return w;
    if (phase_ != Phase::Open) return Why::WrongPhase;
    if (code_ok_) return Why::None;
    const bool right = code.size() == offer_.setup_code.size() && valid_setup_code(code) && equal_constant_time(reinterpret_cast<const uint8_t *>(code.data()), reinterpret_cast<const uint8_t *>(offer_.setup_code.data()), code.size());
    if (right) {
        code_ok_ = true;
        return Why::None;
    }
    if (++wrong_codes_ >= kMaxWrongCodes) {
        close(Why::TooManyWrongCodes);
        return Why::TooManyWrongCodes;
    }
    return Why::WrongCode;
}

Why Session::submit(int64_t now_ms, const SetupForm &form) {
    if (Why w = expire(now_ms); w != Why::None) return w;
    if (phase_ != Phase::Open) return Why::WrongPhase;
    if (!code_ok_) return Why::CodeRequired;  // before the setup code nothing is accepted
    Package package;
    int64_t epoch;
    if (!valid_ssid(form.ssid) || !valid_wifi_password(form.wifi_password) || !parse_utc(form.utc, epoch) || !decode_package(form.package, package)) return Why::InvalidInput;
    // The TLS client trusts exactly the CA of the package. The PEM it takes is made from those bytes and nothing else.
    const std::string pem = der_to_pem(package.ca_der);
    if (!valid_ca_pem(pem)) return Why::InvalidInput;
    if (!sink_.set_time(form.utc)) return Why::InvalidInput;  // the clock decides whether the certificate is valid
    form_ = form;
    package_ = package;
    ca_pem_ = pem;
    phase_ = Phase::Joining;
    return Why::None;
}

Why Session::joined(int64_t now_ms, bool ok) {
    if (Why w = expire(now_ms); w != Why::None) return w;
    if (phase_ != Phase::Joining) return Why::WrongPhase;
    if (!ok) return fail_attempt(Why::WifiFailed);
    phase_ = Phase::Redeeming;
    return Why::None;
}

Why Session::redeemed(int64_t now_ms, Tls tls, int http_status, const std::string &body) {
    if (Why w = expire(now_ms); w != Why::None) return w;
    if (phase_ != Phase::Redeeming) return Why::WrongPhase;
    switch (tls) {
    case Tls::Ok: break;
    case Tls::Unreachable: return fail_attempt(Why::AgentUnreachable);
    case Tls::CertificateExpired: return fail_attempt(Why::CertificateExpired);
    case Tls::HostnameMismatch: return fail_attempt(Why::HostnameMismatch);
    case Tls::ChainInvalid: return fail_attempt(Why::ChainInvalid);
    }
    if (http_status == 403) return fail_attempt(Why::RedeemRefused);
    if (http_status != 200) return fail_attempt(Why::AgentUnreachable);
    Reply reply;
    if (!parse_redeem_reply(body, package_.ca_der, reply)) return fail_attempt(Why::BadReply);
    reply_ = reply;
    have_reply_ = true;
    return save();
}

Why Session::retry_save(int64_t now_ms) {
    if (Why w = expire(now_ms); w != Why::None) return w;
    if (phase_ != Phase::SaveFailed) return Why::WrongPhase;
    return save();
}

// The one place the stored configuration is touched: once, complete, through the same path as USB pairing.
Why Session::save() {
    auto failed = [&](Why why) {
        phase_ = Phase::SaveFailed;  // stay open: the error is shown, the old configuration is unchanged, saving can be retried
        why_ = why;
        return why;
    };
    Secrets s;
    if (!sink_.current(s)) return failed(Why::SaveFailed);  // what pairing does not own (update key, NTP server) must come from the stored one
    s.ssid = form_.ssid;
    s.password = form_.wifi_password;
    s.url = package_.url;
    s.agent_id = reply_.agent_id;
    s.token = reply_.token;
    s.ca_pem = ca_pem_;
    s.update_key_explicit = false;
    if (!s.ready() || !valid_ssid(s.ssid) || !valid_wifi_password(s.password) || !valid_https_origin(s.url) || !valid_identifier(s.agent_id) || !valid_token(s.token) || !valid_ca_pem(s.ca_pem))
        return failed(Why::SaveFailed);
    if (!sink_.commit(s)) return failed(Why::SaveFailed);
    Secrets stored;
    if (!sink_.readback(stored) || !same_credentials(stored, s)) return failed(Why::ReadbackMismatch);
    clear_form();
    wipe(offer_.hotspot_password); wipe(offer_.setup_code); wipe(offer_.hotspot_ssid);
    code_ok_ = false;
    phase_ = Phase::Done;
    why_ = Why::None;
    return Why::None;
}
}  // namespace dh::pairing
