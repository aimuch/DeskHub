// Failure modes of the device side of hotspot pairing, written before the code (docs/pairing-design.md section 7,
// items 10 to 15, plus the pieces they rest on). All pure logic: no hotspot, no TLS, no flash.
//
// V1  Every valid vector of protocol/examples/pairing-package.json decodes to its fields; every invalid one is refused.
// V2  Any single changed character, any truncation, any extension, white space inside, a wrong prefix, padding, the
//     non-canonical spelling of the last character, and a package whose check is right but whose address is not
//     an https origin, are refused. Random garbage never decodes.
// H1  SHA-256 matches the published answers (empty, "abc", 56 bytes, a million "a") and the vectors' fingerprint.
// N1  Fingerprint check (item 12): a CA is the trust root only if its DER hashes to the pinned fingerprint. One flipped
//     bit, an appended byte, an empty certificate, a different CA: refused. A private key is never a certificate.
// N2  The redeem reply must carry exactly the five members, valid values, and the pinned CA.
// I1  Input validation: the same limits as USB provisioning, and a strict clock and setup code.
// R1  Random secrets: digits without modulo bias, a password from the 32-character alphabet, a failing RNG makes
//     no secret and opens nothing.
// B1  Long press: 3 s, once per press, short glitches of the signal do not restart it.
// T1  Countdown: never negative, never above the window, rounded up, "04:32".
// E10 Wrong setup code: nothing else is accepted before the right one; five wrong codes close the hotspot, even for
//     the right code afterwards.
// E11 Window over: the hotspot closes at the deadline, at any step, what was entered is discarded, nothing is stored.
// E12 Wi-Fi, Agent, pin, certificate, host name, refusal, bad reply: each aborts without touching the stored
//     configuration; five failures close the hotspot.
// E13 Saving fails or reads back differently: the session stays open, shows the error, the old configuration is
//     unchanged, and saving can be retried. It ends when the window ends.
// E14 The configuration is committed once, complete, only after a good redemption, from the pinned CA; the update key
//     and NTP server of the old configuration are kept. A power cut during the commit leaves the old one.
// E15 A paired device opens nothing without a long press. An unpaired one opens once per boot, then only on a long press.
// F1  A random walk over all calls (a million steps): the state machine never commits twice, never commits outside a
//     good redemption, never keeps a secret when closed, never lets the clock go outside the window.
#include "cJSON.h"
#include "json_util.hpp"
#include "pairing_session.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace dh;
using namespace dh::pairing;

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

static std::string hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
static std::string hex(const Digest &d) { return hex(d.data(), d.size()); }
static std::string b64(const std::string &raw) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < raw.size(); i += 3) {
        uint32_t v = static_cast<uint8_t>(raw[i]) << 16;
        if (i + 1 < raw.size()) v |= static_cast<uint8_t>(raw[i + 1]) << 8;
        if (i + 2 < raw.size()) v |= static_cast<uint8_t>(raw[i + 2]);
        out += t[v >> 18]; out += t[(v >> 12) & 63];
        out += i + 1 < raw.size() ? t[(v >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? t[v & 63] : '=';
    }
    return out;
}

// ---- the shared vectors ----
struct Vector { std::string package, url, grant_id, code, fingerprint_hex, why; };
static std::vector<Vector> valid_vectors, invalid_vectors;
static void load_vectors() {
    std::ifstream in(PAIRING_VECTORS);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    cJSON *root = cJSON_Parse(text.c_str());
    CHECK(root != nullptr);
    if (!root) return;
    auto str = [](cJSON *o, const char *k) { auto *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) ? std::string(v->valuestring) : std::string(); };
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "valid"))
        valid_vectors.push_back({str(item, "package"), str(item, "url"), str(item, "grant_id"), str(item, "code"), str(item, "ca_fingerprint_hex"), ""});
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "invalid"))
        invalid_vectors.push_back({str(item, "package"), "", "", "", "", str(item, "why")});
    cJSON_Delete(root);
    CHECK(valid_vectors.size() >= 3 && invalid_vectors.size() >= 10);
}
static const std::string kCaDer = "deskhub pairing test vector CA";  // the vectors' "CA": its SHA-256 is the fingerprint

// ---- V1, V2 ----
static void v1_vectors() {
    for (const auto &v : valid_vectors) {
        Package p;
        PackageError why = PackageError::Prefix;
        CHECK(decode_package(v.package, p, &why) && why == PackageError::None);
        CHECK(p.url == v.url && p.grant_id == v.grant_id && p.code == v.code && hex(p.fingerprint) == v.fingerprint_hex);
        CHECK(valid_https_origin(p.url));
        CHECK(decode_package("  \t" + v.package + "\r\n ", p));  // copy and paste
    }
    for (const auto &v : invalid_vectors) {
        Package p;
        PackageError why = PackageError::None;
        if (decode_package(v.package, p, &why)) std::fprintf(stderr, "accepted: %s\n", v.why.c_str());
        CHECK(!decode_package(v.package, p, &why) && why != PackageError::None);
    }
}
static void v2_tampering() {
    const std::string good = valid_vectors[0].package;
    Package p;
    for (size_t i = 5; i < good.size(); ++i) {
        std::string changed = good;
        changed[i] = good[i] != 'A' ? 'A' : 'B';
        CHECK(!decode_package(changed, p));
    }
    for (size_t n = 0; n < good.size(); ++n) CHECK(!decode_package(good.substr(0, n), p));   // every truncation
    for (const char *extra : {"A", "AA", "AAAA", "=", "\n\n", "-"}) CHECK(decode_package(good + extra, p) == (std::string(extra) == "\n\n"));  // only white space is allowed after it
    for (size_t i = 5; i < good.size(); ++i) { std::string s = good; s.insert(i, " "); CHECK(!decode_package(s, p)); }
    for (size_t i = 5; i < good.size(); ++i) { std::string s = good; s.insert(i, "\n"); CHECK(!decode_package(s, p)); }
    for (const char *prefix : {"DHP2.", "dhp1.", "DHP1", "DHP1:", "XHP1.", ""}) CHECK(!decode_package(prefix + good.substr(5), p));
    CHECK(!decode_package("DHP1.", p) && !decode_package("", p) && !decode_package("   ", p));
    // the spare bits of the last character
    static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const size_t last = table.find(good.back());
    CHECK(last != std::string::npos && last % 16 == 0);
    for (size_t extra = 1; extra < 16; ++extra) CHECK(!decode_package(good.substr(0, good.size() - 1) + table[last + extra], p));
    // the standard alphabet and padding are not accepted
    for (const char *bad : {"+", "/", "=", "==", "*", "~", "\x01", "\xff"}) { std::string s = good; s.replace(10, 1, bad); CHECK(!decode_package(s, p)); }
    CHECK(!decode_package(good + std::string(1, '\0'), p));
    // a valid check does not make a bad address good
    auto craft = [&](const std::string &url, const std::string &id, const std::string &code, uint8_t version = 1, const std::string &tail = "") {
        std::string body;
        body += static_cast<char>(version);
        body += static_cast<char>(url.size());
        body += url + id + code;
        const Digest fp = sha256(kCaDer);
        body.append(reinterpret_cast<const char *>(fp.data()), 32);
        body += tail;                                                  // bytes after the fingerprint, under a valid check
        const Digest check = sha256(body);
        body.append(reinterpret_cast<const char *>(check.data()), 4);
        std::string out = b64(body);
        for (auto &c : out) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
        while (!out.empty() && out.back() == '=') out.pop_back();
        return "DHP1." + out;
    };
    CHECK(decode_package(craft("https://10.10.10.5:8765", "ABCD2345", "KM7QWXYZ09"), p));
    for (const char *url : {"http://10.10.10.5:8765", "https://x/api", "https://x/", "https://u:p@x", "https://x?q=1", "https://x#f", "", "https://", "https://x:0",
                            "https://x:99999", "https://x:", "https://x y", "https://-x", "https://x-", "https://.x", "ftp://x", "https://[::1", "https://[]"})
        CHECK(!decode_package(craft(url, "ABCD2345", "KM7QWXYZ09"), p));
    for (const char *id : {"ABCD234", "ABCD23456", "abcd2345", "ABCI2345", "ABCL2345", "ABCO2345", "ABCU2345", "ABCD 345"}) CHECK(!decode_package(craft("https://x", id, "KM7QWXYZ09"), p));
    for (const char *code : {"KM7QWXYZ0", "KM7QWXYZ090", "km7qwxyz09", "KM7QWXYZ0I", "KM7QWXYZ0U"}) CHECK(!decode_package(craft("https://x", "ABCD2345", code), p));
    CHECK(!decode_package(craft("https://x", "ABCD2345", "KM7QWXYZ09", 0), p) && !decode_package(craft("https://x", "ABCD2345", "KM7QWXYZ09", 2), p));
    CHECK(!decode_package(craft("https://x", "ABCD2345", "KM7QWXYZ09", 1, "X"), p) && !decode_package(craft("https://x", "ABCD2345", "KM7QWXYZ09", 1, std::string(40, 'X')), p));
    // random garbage and random damage never decode
    uint64_t s = 88172645463325252ULL;
    auto next = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    for (int round = 0; round < 20000; ++round) {
        std::string junk = "DHP1.";
        const size_t n = next() % 140;
        for (size_t i = 0; i < n; ++i) junk += static_cast<char>(next() % 256);
        CHECK(!decode_package(junk, p));
        std::string damaged = good;
        damaged[5 + next() % (good.size() - 5)] = static_cast<char>(next() % 256);
        if (damaged != good) CHECK(!decode_package(damaged, p));
    }
}
static void v3_origin() {
    Origin o;
    CHECK(split_origin("https://10.10.10.5:8765", o) && o.host == "10.10.10.5" && o.port == 8765);
    CHECK(split_origin("https://desk-mac.local", o) && o.host == "desk-mac.local" && o.port == 443);
    CHECK(split_origin("https://[fd00::5]:8765", o) && o.host == "fd00::5" && o.port == 8765);
    CHECK(split_origin("https://x:1", o) && o.port == 1 && split_origin("https://x:65535", o) && o.port == 65535);
    CHECK(split_origin("https://x:08765", o) && o.port == 8765);                       // five digits are fine, with a leading zero too
    for (const char *bad : {"https://x:0", "https://x:65536", "https://x:099999", "https://x:008765", "https://x:0000080", "http://x", "https://x/", "https://", ""})
        CHECK(!split_origin(bad, o) && !valid_https_origin(bad));
    CHECK(!split_origin("https://" + std::string(130, 'a'), o) && !valid_https_origin("https://" + std::string(130, 'a')));

}

// ---- H1 ----
static void h1_sha256() {
    CHECK(hex(sha256(std::string())) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(sha256(std::string("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex(sha256(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hex(sha256(std::string(1000000, 'a'))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    for (const auto &v : valid_vectors) CHECK(hex(sha256(kCaDer)) == v.fingerprint_hex);  // an independent oracle made these
    CHECK(equal_constant_time(reinterpret_cast<const uint8_t *>("abc"), reinterpret_cast<const uint8_t *>("abc"), 3));
    CHECK(!equal_constant_time(reinterpret_cast<const uint8_t *>("abc"), reinterpret_cast<const uint8_t *>("abd"), 3));
    CHECK(equal_constant_time(nullptr, nullptr, 0));
    for (int bit = 0; bit < 8; ++bit) { uint8_t a = 0, b = static_cast<uint8_t>(1 << bit); CHECK(!equal_constant_time(&a, &b, 1)); }
}

// ---- N1, N2 ----
static std::string pem_of(const std::string &der) {
    std::string body = b64(der), out = "-----BEGIN CERTIFICATE-----\n";
    for (size_t i = 0; i < body.size(); i += 64) out += body.substr(i, 64) + "\n";
    return out + "-----END CERTIFICATE-----\n";
}
static void n1_pin() {
    Digest pin = sha256(kCaDer);
    std::vector<uint8_t> der(kCaDer.begin(), kCaDer.end());
    CHECK(pin_matches(pin, der));
    for (size_t byte = 0; byte < der.size(); ++byte)
        for (int bit = 0; bit < 8; ++bit) { auto d = der; d[byte] ^= static_cast<uint8_t>(1 << bit); CHECK(!pin_matches(pin, d)); }
    auto longer = der; longer.push_back(0);
    CHECK(!pin_matches(pin, longer) && !pin_matches(pin, std::vector<uint8_t>(der.begin(), der.end() - 1)));
    CHECK(!pin_matches(pin, {}));
    Digest zero{};
    CHECK(!pin_matches(sha256(std::string()), {}));                                // the hash of nothing is not a pin for nothing
    CHECK(!pin_matches(zero, der) && !pin_matches(zero, {}));    // an all-zero pin never matches, not even the empty certificate
    const std::string other = "another CA";
    CHECK(!pin_matches(pin, std::vector<uint8_t>(other.begin(), other.end())));
    // PEM to DER
    std::vector<uint8_t> out;
    CHECK(pem_to_der(pem_of(kCaDer), out) && out == der);
    CHECK(pem_to_der("junk\n" + pem_of(kCaDer) + "junk", out) && out == der);
    for (const char *bad : {"", "garbage", "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n", "-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n",
                            "-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n", "-----BEGIN CERTIFICATE-----\nAAAA\n"})
        CHECK(!pem_to_der(bad, out));
    CHECK(!pem_to_der(pem_of(kCaDer) + "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n", out));   // a private key next to it: refused
    CHECK(!pem_to_der(std::string("-----BEGIN CERTIFICATE-----\n") + std::string(9000, 'A') + "\n-----END CERTIFICATE-----\n", out));   // too large
}
static std::string good_body(const std::string &token = std::string(43, 'T'), const std::string &agent = "agent-test", const std::string &pem = std::string()) {
    std::string p = pem.empty() ? pem_of(kCaDer) : pem, escaped;
    for (char c : p) escaped += c == '\n' ? std::string("\\n") : std::string(1, c);
    return "{\"agent_id\":\"" + agent + "\",\"token\":\"" + token + "\",\"ca_pem\":\"" + escaped + "\",\"scopes\":[\"read:status\",\"device:sync\"],\"targets\":[\"monitor-desk-01\"]}";
}
static void n2_reply() {
    const Digest pin = sha256(kCaDer);
    Reply r;
    CHECK(parse_redeem_reply(good_body(), pin, r) && r.agent_id == "agent-test" && r.token == std::string(43, 'T') && r.ca_pem == pem_of(kCaDer));
    CHECK(!parse_redeem_reply(good_body(), Digest{}, r));                                                         // not the pinned CA
    CHECK(!parse_redeem_reply(good_body(std::string(43, 'T'), "agent-test", pem_of("a different CA")), pin, r));
    for (const std::string &token : {std::string(31, 'T'), std::string(257, 'T'), std::string("short"), std::string(40, ' '), std::string(40, 'T') + "\\n", std::string()})
        CHECK(!parse_redeem_reply(good_body(token), pin, r));
    for (const char *agent : {"", "bad id", "-x", "a/b"}) CHECK(!parse_redeem_reply(good_body(std::string(43, 'T'), agent), pin, r));
    const std::string base = good_body();
    for (const char *key : {"agent_id", "token", "ca_pem", "scopes", "targets"}) {
        std::string s = base;
        s.replace(s.find(std::string("\"") + key + "\""), std::strlen(key) + 2, "\"x_x\"");
        CHECK(!parse_redeem_reply(s, pin, r));                                 // a missing member
    }
    CHECK(!parse_redeem_reply(base.substr(0, base.size() - 1) + ",\"extra\":1}", pin, r));
    CHECK(!parse_redeem_reply(base.substr(0, base.size() - 1) + ",\"token\":\"" + std::string(43, 'U') + "\"}", pin, r));   // a repeated member
    for (const char *bad : {"", "{}", "[]", "null", "\"x\"", "{\"agent_id\":1}", "not json", "{\"agent_id\":\"a\"", "{}{}"}) CHECK(!parse_redeem_reply(bad, pin, r));
    std::string typed = base;
    const std::string needle = "[\"read:status\",\"device:sync\"]";
    typed.replace(typed.find(needle), needle.size(), "\"read:status\"");
    CHECK(!parse_redeem_reply(typed, pin, r));
    CHECK(!parse_redeem_reply(base + std::string(1, '\0'), pin, r));
    CHECK(!parse_redeem_reply(std::string(20000, ' ') + base, pin, r));
}

// ---- I1 ----
static void i1_input() {
    CHECK(valid_ssid("Desk") && valid_ssid(std::string(32, 'a')) && valid_ssid("Desk\xe7\xbd\x91\xe7\xbb\x9c"));
    CHECK(!valid_ssid("") && !valid_ssid(std::string(33, 'a')) && !valid_ssid(std::string("a\0b", 3)));
    CHECK(valid_wifi_password("") && valid_wifi_password("12345678") && valid_wifi_password(std::string(63, 'p')));
    CHECK(!valid_wifi_password("1234567") && !valid_wifi_password(std::string(64, 'p')) && !valid_wifi_password(std::string("12345678\0", 9)));
    for (const char *ok : {"agent-test", "a", "A1", "host-main", "a.b:c_d-e", "0abc"}) CHECK(valid_identifier(ok));
    for (const char *bad : {"", "-a", "_a", ".a", ":a", "a b", "a/b", "a\n", "é"}) CHECK(!valid_identifier(bad));
    CHECK(valid_identifier(std::string(96, 'a')) && !valid_identifier(std::string(97, 'a')));
    CHECK(valid_token(std::string(32, 'x')) && valid_token(std::string(256, 'x')) && valid_token("abc_DEF-123~!@#$%^&*()+=[]{}|;:,.<>?/abc"));
    CHECK(!valid_token(std::string(31, 'x')) && !valid_token(std::string(257, 'x')) && !valid_token(std::string(31, 'x') + " ") && !valid_token(std::string(31, 'x') + "\n") &&
          !valid_token(std::string(31, 'x') + "\x7f") && !valid_token(std::string(31, 'x') + "\xc3\xa9"));
    CHECK(valid_ca_pem(pem_of(kCaDer)) && !valid_ca_pem("") && !valid_ca_pem("garbage") && !valid_ca_pem(std::string(8193, 'A') + "-----BEGIN CERTIFICATE-----"));
    CHECK(!valid_ca_pem("-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n"));
    CHECK(valid_setup_code("000000") && valid_setup_code("624819"));
    for (const char *bad : {"", "12345", "1234567", "12345a", "12 456", "٣٤٥٦٧٨", "12345\n", "+12345", "-12345", "1.2345"}) CHECK(!valid_setup_code(bad));
    int64_t t = 0;
    CHECK(parse_utc("2026-10-28T12:34:56Z", t) && t == 1793190896);
    CHECK(parse_utc("2024-02-29T00:00:00Z", t) && parse_utc("2099-12-31T23:59:59Z", t));
    for (const char *bad : {"", "2026-10-28", "2026-10-28 12:34:56Z", "2026-10-28T12:34:56", "2026-10-28T12:34:56+00:00", "2026-10-28t12:34:56z", "2023-12-31T23:59:59Z",
                            "2100-01-01T00:00:00Z", "2026-13-01T00:00:00Z", "2026-00-10T00:00:00Z", "2026-02-29T00:00:00Z", "2026-04-31T00:00:00Z", "2026-10-00T00:00:00Z",
                            "2026-10-32T00:00:00Z", "2026-10-28T24:00:00Z", "2026-10-28T12:60:00Z", "2026-10-28T12:34:60Z", "2026-10-28T12:34:5Z", "26-10-28T12:34:56Z",
                            "2026-10-28T12:34:56Z ", " 2026-10-28T12:34:56Z", "2026-10-28T12:34:56.123Z", "20261028T123456Z"})
        CHECK(!parse_utc(bad, t));
    CHECK(hotspot_name("deskhub-44bd8d60c598") == "DeskHub-C598" && hotspot_name("deskhub-aabbccddee0f") == "DeskHub-EE0F");
    for (const char *odd : {"", "x", "zz", "deskhub-", "not hex at all!", "é"}) { const std::string n = hotspot_name(odd); CHECK(valid_ssid(n) && n.rfind("DeskHub-", 0) == 0 && n.size() == 12); }
}

// ---- R1 ----
struct FixedRandom : Random {
    std::vector<uint8_t> bytes;
    size_t pos = 0;
    bool fail = false, repeat = false;
    bool fill(uint8_t *out, size_t n) override {
        if (fail) return false;
        for (size_t i = 0; i < n; ++i) {
            if (pos >= bytes.size() && !repeat) return false;
            out[i] = bytes[repeat ? pos % bytes.size() : pos];
            ++pos;
        }
        return true;
    }
};
struct XorRandom : Random {
    uint64_t s = 88172645463325252ULL;
    bool fill(uint8_t *out, size_t n) override {
        for (size_t i = 0; i < n; ++i) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; out[i] = static_cast<uint8_t>(s >> 24); }
        return true;
    }
};
static void r1_random() {
    FixedRandom r;
    r.bytes = {255, 254, 250, 7, 3, 249, 0, 9, 1, 2};
    std::string digits;
    FixedRandom exact;
    exact.bytes = {255, 254, 250, 7, 3, 249, 0, 9, 1, 2};
    digits.clear();
    CHECK(random_digits(exact, 6, digits) && digits == "739091");
    FixedRandom runs_out;
    runs_out.bytes = {1, 2};
    CHECK(!random_digits(runs_out, 6, digits));
    FixedRandom stuck;
    stuck.bytes = {255};
    stuck.repeat = true;
    CHECK(!random_digits(stuck, 6, digits));                                    // a stuck generator ends, it does not loop
    FixedRandom failed;
    failed.fail = true;
    CHECK(!random_digits(failed, 6, digits) && !random_password(failed, 12, digits));
    // uniform: every digit appears about equally often
    XorRandom x;
    int count[10] = {};
    for (int i = 0; i < 20000; ++i) { std::string d; CHECK(random_digits(x, 6, d) && d.size() == 6); for (char c : d) { CHECK(c >= '0' && c <= '9'); ++count[c - '0']; } }
    for (int c : count) CHECK(c > 11400 && c < 12600);
    // password alphabet and length
    std::string pw;
    const std::string alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    int seen[256] = {};
    for (int i = 0; i < 4000; ++i) {
        CHECK(random_password(x, 12, pw) && pw.size() == 12);
        for (char c : pw) { CHECK(alphabet.find(c) != std::string::npos); ++seen[static_cast<uint8_t>(c)]; }
    }
    for (char c : alphabet) CHECK(seen[static_cast<uint8_t>(c)] > 1300 && seen[static_cast<uint8_t>(c)] < 1700);
    for (char c : std::string("ILOUilou")) CHECK(seen[static_cast<uint8_t>(c)] == 0);
    CHECK(random_password(x, 12, pw) && valid_wifi_password(pw));              // good for WPA2
}

// ---- B1 ----
static void b1_long_press() {
    LongPress p;
    CHECK(!p.poll(10000));                       // nothing pressed
    p.edge(true, 0);
    CHECK(!p.poll(0) && !p.poll(2999) && p.poll(3000) && !p.poll(3001) && !p.poll(9000));   // once per press
    p.edge(false, 3500);
    p.edge(true, 4000);
    CHECK(!p.poll(6999) && p.poll(7000));                                       // a new press fires again
    p.edge(false, 7100);
    LongPress q;                                                                 // a glitch of 30 ms does not restart the press
    q.edge(true, 0); q.edge(false, 1000); q.edge(true, 1030);
    CHECK(!q.poll(2999) && q.poll(3000));
    LongPress r;                                                                 // a release of 100 ms does
    r.edge(true, 0); r.edge(false, 1000); r.edge(true, 1100);
    CHECK(!r.poll(3000) && !r.poll(4099) && r.poll(4100));
    LongPress s;                                                                 // released early: nothing
    s.edge(true, 0); s.edge(false, 2999);
    CHECK(!s.poll(3000) && !s.poll(100000));
    LongPress t;
    t.edge(true, 5000);
    CHECK(!t.poll(1000) && !t.poll(4999));                                       // time never runs backwards into a press
    LongPress u;
    u.edge(false, 100); u.edge(false, 200);                                      // a release with no press is nothing
    CHECK(!u.poll(100000));
    LongPress v;
    v.edge(true, 0); v.edge(true, 500); v.edge(true, 1000);                      // repeated "pressed" edges do not restart it
    CHECK(v.poll(3000));
    LongPress w(1000, 50);
    w.edge(true, 0);
    CHECK(!w.poll(999) && w.poll(1000));
}

// ---- T1 ----
static void t1_countdown() {
    CHECK(remaining_seconds(0, kWindowMs, kWindowMs) == 300 && remaining_seconds(1, kWindowMs, kWindowMs) == 300);
    CHECK(remaining_seconds(28000, kWindowMs, kWindowMs) == 272 && remaining_seconds(28001, kWindowMs, kWindowMs) == 272 && remaining_seconds(28999, kWindowMs, kWindowMs) == 272 && remaining_seconds(29000, kWindowMs, kWindowMs) == 271);
    CHECK(remaining_seconds(kWindowMs, kWindowMs, kWindowMs) == 0 && remaining_seconds(kWindowMs + 1, kWindowMs, kWindowMs) == 0 && remaining_seconds(INT64_MAX / 2, kWindowMs, kWindowMs) == 0);
    CHECK(remaining_seconds(-1000000, kWindowMs, kWindowMs) == 300);              // a clock before the start never shows more than the window
    CHECK(remaining_seconds(299001, kWindowMs, kWindowMs) == 1 && remaining_seconds(299999, kWindowMs, kWindowMs) == 1);
    CHECK(format_countdown(272) == "04:32" && format_countdown(0) == "00:00" && format_countdown(300) == "05:00" && format_countdown(59) == "00:59" && format_countdown(60) == "01:00");
    CHECK(format_countdown(-5) == "00:00" && format_countdown(1000000) == "99:59");
}

// ---- the session rig ----
struct FakeSink : Sink {
    Secrets stored;
    bool time_ok = true, commit_ok = true, readback_ok = true, corrupt_readback = false, power_cut = false, current_ok = true;
    int commits = 0, time_sets = 0, readbacks = 0;
    std::string time_value;
    std::vector<Secrets> committed;
    FakeSink() {
        stored.ssid = "OldNet"; stored.password = "oldpassword"; stored.url = "https://old.example:8765"; stored.agent_id = "old-agent";
        stored.token = std::string(40, 'o'); stored.ca_pem = "OLD CA"; stored.update_public_key_pem = "UPDATE KEY"; stored.ntp_server = "ntp.old.example";
        stored.update_key_explicit = false; stored.revision = 7;
    }
    bool current(Secrets &out) override { if (!current_ok) return false; out = stored; return true; }
    bool set_time(const std::string &utc) override { ++time_sets; time_value = utc; return time_ok; }
    bool commit(const Secrets &s) override {
        ++commits;
        committed.push_back(s);
        if (power_cut) return false;           // the power went while writing: the old configuration is what is left
        if (!commit_ok) return false;
        stored = s;
        return true;
    }
    bool readback(Secrets &out) override {
        ++readbacks;
        if (!readback_ok) return false;
        out = stored;
        if (corrupt_readback) out.token[0] ^= 1;
        return true;
    }
};
static bool same_secrets(const Secrets &a, const Secrets &b) {
    return a.ssid == b.ssid && a.password == b.password && a.url == b.url && a.agent_id == b.agent_id && a.token == b.token && a.ca_pem == b.ca_pem &&
           a.update_public_key_pem == b.update_public_key_pem && a.ntp_server == b.ntp_server && a.update_key_explicit == b.update_key_explicit && a.revision == b.revision;
}
static const int64_t T0 = 100000;
struct Rig {
    XorRandom random;
    FakeSink sink;
    Session session{random, sink, "deskhub-44bd8d60c598"};
    Secrets original = sink.stored;
    int64_t now = T0;
    SetupForm form() const {
        return {"HomeWifi", "wifi-password", valid_vectors[0].package, "2026-10-28T12:34:56Z"};
    }
    Why open(bool paired = false, bool press = false) { return session.open(now, paired, press); }
    void unlock() { CHECK(session.enter_code(now, session.offer().setup_code) == Why::None); }
    void to_joining() { CHECK(open() == Why::None); unlock(); CHECK(session.submit(now, form()) == Why::None); }
    void to_fetching() { to_joining(); CHECK(session.joined(now, true) == Why::None); }
    std::vector<uint8_t> der() const { return std::vector<uint8_t>(kCaDer.begin(), kCaDer.end()); }
    void to_redeeming() { to_fetching(); CHECK(session.ca_fetched(now, true, der(), pem_of(kCaDer)) == Why::None); }
    Why finish(const std::string &body = std::string()) { return session.redeemed(now, Tls::Ok, 200, body.empty() ? good_body() : body); }
    bool untouched() const { return sink.commits == 0 && same_secrets(sink.stored, original); }
};

static void e_offer() {
    Rig r;
    CHECK(r.session.phase() == Phase::Closed && r.session.offer().setup_code.empty() && r.session.offer().hotspot_password.empty() && !r.session.holds_secrets());
    CHECK(r.open() == Why::None && r.session.phase() == Phase::Open && r.session.is_open());
    const Offer &o = r.session.offer();
    CHECK(o.hotspot_ssid == "DeskHub-C598" && valid_setup_code(o.setup_code) && o.hotspot_password.size() == kHotspotPasswordLength && valid_wifi_password(o.hotspot_password));
    CHECK(r.session.remaining_ms(r.now) == kWindowMs && !r.session.code_accepted());
    Rig other;
    other.random.s ^= 0x1234567;
    other.open();
    CHECK(other.session.offer().setup_code != o.setup_code || other.session.offer().hotspot_password != o.hotspot_password);
    CHECK(r.open() == Why::WrongPhase && r.session.offer().setup_code == o.setup_code);       // not twice
    // a failing generator opens nothing
    FixedRandom none;
    none.fail = true;
    FakeSink sink;
    Session s(none, sink, "deskhub-44bd8d60c598");
    CHECK(s.open(T0, false, false) == Why::RandomFailed && s.phase() == Phase::Closed && s.offer().setup_code.empty() && s.offer().hotspot_password.empty());
}
static void e10_wrong_code() {
    {
        Rig r;                                                                   // before the code nothing else is accepted
        r.open();
        CHECK(r.session.submit(r.now, r.form()) == Why::CodeRequired && r.session.phase() == Phase::Open && !r.session.holds_secrets());
        CHECK(r.session.joined(r.now, true) == Why::WrongPhase && r.session.ca_fetched(r.now, true, r.der(), pem_of(kCaDer)) == Why::WrongPhase &&
              r.session.redeemed(r.now, Tls::Ok, 200, good_body()) == Why::WrongPhase && r.session.retry_save(r.now) == Why::WrongPhase);
        CHECK(r.sink.time_sets == 0 && r.untouched());
    }
    {
        Rig r;
        r.open();
        const std::string right = r.session.offer().setup_code;
        std::string wrong = right;
        wrong[0] = wrong[0] == '0' ? '1' : '0';
        for (int i = 1; i <= 4; ++i) CHECK(r.session.enter_code(r.now, wrong) == Why::WrongCode && r.session.wrong_codes() == i && r.session.is_open());
        CHECK(r.session.enter_code(r.now, right) == Why::None && r.session.code_accepted());   // the fifth try is the right one
        CHECK(r.session.enter_code(r.now, "000000") == Why::None);                                 // already accepted
    }
    {
        Rig r;
        r.open();
        const std::string right = r.session.offer().setup_code;
        std::string wrong = right;
        wrong[5] = wrong[5] == '0' ? '1' : '0';
        for (int i = 1; i <= 4; ++i) CHECK(r.session.enter_code(r.now, wrong) == Why::WrongCode);
        CHECK(r.session.enter_code(r.now, wrong) == Why::TooManyWrongCodes && r.session.phase() == Phase::Closed && r.session.offer().setup_code.empty());
        CHECK(r.session.enter_code(r.now, right) == Why::WrongPhase && !r.session.code_accepted());   // the right code is too late
        CHECK(r.session.submit(r.now, r.form()) == Why::WrongPhase && r.untouched() && r.sink.time_sets == 0);
        CHECK(r.session.last_why() == Why::TooManyWrongCodes);
    }
    {
        Rig r;                                                                   // a malformed code counts as wrong
        r.open();
        for (const char *junk : {"", "12345", "1234567", "abcdef"}) CHECK(r.session.enter_code(r.now, junk) == Why::WrongCode);
        CHECK(r.session.enter_code(r.now, "12 345") == Why::TooManyWrongCodes);
        CHECK(r.session.phase() == Phase::Closed && r.session.last_why() == Why::TooManyWrongCodes);
    }
}
static void e11_window() {
    {
        Rig r;
        r.open();
        CHECK(r.session.tick(r.now + kWindowMs - 1) == Why::None && r.session.is_open() && r.session.remaining_ms(r.now + kWindowMs - 1) == 1);
        CHECK(r.session.tick(r.now + kWindowMs) == Why::Timeout && r.session.phase() == Phase::Closed && r.session.last_why() == Why::Timeout);
        CHECK(r.session.remaining_ms(r.now + kWindowMs) == 0 && r.session.offer().hotspot_password.empty());
        CHECK(r.untouched());
    }
    for (int stage = 0; stage < 5; ++stage) {                                    // the window ends at every step; the step then fails
        Rig r;
        r.open();
        r.unlock();
        if (stage >= 1) CHECK(r.session.submit(r.now, r.form()) == Why::None);
        if (stage >= 2) CHECK(r.session.joined(r.now, true) == Why::None);
        if (stage >= 3) CHECK(r.session.ca_fetched(r.now, true, r.der(), pem_of(kCaDer)) == Why::None);
        const int64_t late = r.now + kWindowMs;
        Why why = Why::None;
        switch (stage) {
        case 0: why = r.session.submit(late, r.form()); break;
        case 1: why = r.session.joined(late, true); break;
        case 2: why = r.session.ca_fetched(late, true, r.der(), pem_of(kCaDer)); break;
        case 3: why = r.session.redeemed(late, Tls::Ok, 200, good_body()); break;
        default: why = r.session.enter_code(late, "000000"); break;
        }
        CHECK(why == Why::Timeout && r.session.phase() == Phase::Closed);
        CHECK(!r.session.holds_secrets() && r.untouched());                      // what was entered is gone, nothing was stored
    }
    {
        Rig r;                                                                   // the clock jumping backwards does not extend the window
        r.open();
        CHECK(r.session.remaining_ms(r.now - 10000) == kWindowMs);
        CHECK(r.session.tick(r.now + kWindowMs) == Why::Timeout);
    }
}
static void e12_aborts() {
    struct Step { const char *name; std::function<Why(Rig &)> fail; Why expect; int stage; };
    const Step steps[] = {
        {"wifi", [](Rig &r) { return r.session.joined(r.now, false); }, Why::WifiFailed, 1},
        {"agent unreachable", [](Rig &r) { return r.session.ca_fetched(r.now, false, {}, ""); }, Why::AgentUnreachable, 2},
        {"wrong ca", [](Rig &r) { std::string o = "another CA"; return r.session.ca_fetched(r.now, true, std::vector<uint8_t>(o.begin(), o.end()), pem_of(o)); }, Why::PinMismatch, 2},
        {"empty ca", [](Rig &r) { return r.session.ca_fetched(r.now, true, {}, ""); }, Why::PinMismatch, 2},
        {"pem and der differ", [](Rig &r) { return r.session.ca_fetched(r.now, true, r.der(), pem_of("another CA")); }, Why::PinMismatch, 2},
        {"tls unreachable", [](Rig &r) { return r.session.redeemed(r.now, Tls::Unreachable, 0, ""); }, Why::AgentUnreachable, 3},
        {"certificate expired", [](Rig &r) { return r.session.redeemed(r.now, Tls::CertificateExpired, 0, ""); }, Why::CertificateExpired, 3},
        {"host name", [](Rig &r) { return r.session.redeemed(r.now, Tls::HostnameMismatch, 0, ""); }, Why::HostnameMismatch, 3},
        {"chain", [](Rig &r) { return r.session.redeemed(r.now, Tls::ChainInvalid, 0, ""); }, Why::ChainInvalid, 3},
        {"refused", [](Rig &r) { return r.session.redeemed(r.now, Tls::Ok, 403, "{\"error\":{}}"); }, Why::RedeemRefused, 3},
        {"busy", [](Rig &r) { return r.session.redeemed(r.now, Tls::Ok, 429, ""); }, Why::AgentUnreachable, 3},
        {"server error", [](Rig &r) { return r.session.redeemed(r.now, Tls::Ok, 503, ""); }, Why::AgentUnreachable, 3},
        {"not json", [](Rig &r) { return r.session.redeemed(r.now, Tls::Ok, 200, "<html>"); }, Why::BadReply, 3},
        {"short token", [](Rig &r) { return r.session.redeemed(r.now, Tls::Ok, 200, good_body("short")); }, Why::BadReply, 3},
        {"another ca in the reply", [](Rig &r) { return r.session.redeemed(r.now, Tls::Ok, 200, good_body(std::string(43, 'T'), "agent-test", pem_of("another CA"))); }, Why::BadReply, 3},
    };
    for (const Step &s : steps) {
        Rig r;
        r.open();
        r.unlock();
        CHECK(r.session.submit(r.now, r.form()) == Why::None);
        if (s.stage >= 2) CHECK(r.session.joined(r.now, true) == Why::None);
        if (s.stage >= 3) CHECK(r.session.ca_fetched(r.now, true, r.der(), pem_of(kCaDer)) == Why::None);
        const Why got = s.fail(r);
        if (got != s.expect) std::fprintf(stderr, "step %s answered %d\n", s.name, static_cast<int>(got));
        CHECK(got == s.expect);
        CHECK(r.session.phase() == Phase::Open && r.session.last_why() == s.expect && r.session.is_open());   // the person can try again
        CHECK(!r.session.holds_secrets() && r.session.attempts() == 1);
        CHECK(r.untouched());                                                                                  // the old configuration is as it was
        CHECK(r.session.code_accepted());
    }
    {
        Rig r;                                                                                                  // five failures close the hotspot
        r.open();
        r.unlock();
        for (int i = 1; i <= 5; ++i) {
            CHECK(r.session.submit(r.now, r.form()) == Why::None);
            const Why w = r.session.joined(r.now, false);
            CHECK(w == (i < 5 ? Why::WifiFailed : Why::TooManyAttempts));
        }
        CHECK(r.session.phase() == Phase::Closed && r.untouched() && r.session.last_why() == Why::TooManyAttempts);
    }
    {
        Rig r;                                                                                                  // a late step of another phase is refused
        r.to_joining();
        CHECK(r.session.ca_fetched(r.now, true, r.der(), pem_of(kCaDer)) == Why::WrongPhase && r.session.redeemed(r.now, Tls::Ok, 200, good_body()) == Why::WrongPhase);
        CHECK(r.session.phase() == Phase::Joining && r.untouched());
    }
    {
        Rig r;                                                                                                  // bad input never starts anything
        r.open();
        r.unlock();
        SetupForm f = r.form();
        const std::vector<std::function<void(SetupForm &)>> breaks = {
            [](SetupForm &x) { x.ssid = ""; }, [](SetupForm &x) { x.ssid = std::string(33, 'a'); }, [](SetupForm &x) { x.wifi_password = "short"; },
            [](SetupForm &x) { x.wifi_password = std::string(64, 'p'); }, [](SetupForm &x) { x.utc = "yesterday"; }, [](SetupForm &x) { x.utc = "2026-02-29T00:00:00Z"; },
            [](SetupForm &x) { x.package = ""; }, [](SetupForm &x) { x.package = x.package.substr(0, x.package.size() - 1); },
            [](SetupForm &x) { x.package[20] = x.package[20] == 'A' ? 'B' : 'A'; }};
        for (const auto &b : breaks) {
            SetupForm g = f;
            b(g);
            CHECK(r.session.submit(r.now, g) == Why::InvalidInput && r.session.phase() == Phase::Open && !r.session.holds_secrets());
        }
        CHECK(r.sink.time_sets == 0 && r.untouched() && r.session.attempts() == 0);
        r.sink.time_ok = false;                                                                                  // the clock could not be set
        CHECK(r.session.submit(r.now, f) == Why::InvalidInput && r.session.phase() == Phase::Open && !r.session.holds_secrets() && r.untouched());
    }
}
static void e13_save_fails() {
    Rig r;
    r.to_redeeming();
    r.sink.commit_ok = false;
    CHECK(r.finish() == Why::SaveFailed);
    CHECK(r.session.phase() == Phase::SaveFailed && r.session.is_open() && r.session.last_why() == Why::SaveFailed);      // still open, error shown
    CHECK(r.session.offer().setup_code.size() == kSetupCodeLength && r.session.remaining_ms(r.now) > 0);
    CHECK(same_secrets(r.sink.stored, r.original));                                                                          // the old configuration is unchanged
    CHECK(r.sink.commits == 1);
    r.sink.commit_ok = true;
    CHECK(r.session.retry_save(r.now + 1000) == Why::None && r.session.phase() == Phase::Done && r.sink.commits == 2);
    CHECK(!r.session.holds_secrets() && r.sink.stored.token == std::string(43, 'T'));
    {
        Rig q;                                                                                                             // read back differently
        q.to_redeeming();
        q.sink.corrupt_readback = true;
        CHECK(q.finish() == Why::ReadbackMismatch && q.session.phase() == Phase::SaveFailed && q.session.is_open());
        q.sink.corrupt_readback = false;
        CHECK(q.session.retry_save(q.now) == Why::None && q.session.phase() == Phase::Done);
    }
    {
        Rig q;                                                                                                             // read back nothing
        q.to_redeeming();
        q.sink.readback_ok = false;
        CHECK(q.finish() == Why::ReadbackMismatch && q.session.phase() == Phase::SaveFailed);
    }
    {
        Rig q;                                                                                                             // the stored configuration cannot even be read
        q.to_redeeming();
        q.sink.current_ok = false;
        CHECK(q.finish() == Why::SaveFailed && q.sink.commits == 0 && q.session.phase() == Phase::SaveFailed);
    }
    {
        Rig q;                                                                                                             // the window ends while the error is shown: it closes and forgets
        q.to_redeeming();
        q.sink.commit_ok = false;
        q.finish();
        CHECK(q.session.tick(q.now + kWindowMs) == Why::Timeout && q.session.phase() == Phase::Closed && !q.session.holds_secrets() && same_secrets(q.sink.stored, q.original));
        CHECK(q.session.retry_save(q.now + kWindowMs) == Why::WrongPhase && q.sink.commits == 1);
    }
    {
        Rig q;                                                                                                             // retry only after a failure
        q.to_redeeming();
        CHECK(q.session.retry_save(q.now) == Why::WrongPhase && q.sink.commits == 0);
    }
}
static void e14_commit() {
    Rig r;
    r.to_joining();
    CHECK(r.sink.commits == 0 && r.sink.time_sets == 1 && r.sink.time_value == "2026-10-28T12:34:56Z");     // the clock is set early, nothing else
    CHECK(r.session.joined(r.now, true) == Why::None && r.sink.commits == 0);
    CHECK(r.session.ca_fetched(r.now, true, r.der(), pem_of(kCaDer)) == Why::None && r.sink.commits == 0 && r.session.phase() == Phase::Redeeming);
    CHECK(r.session.redeemed(r.now, Tls::Unreachable, 0, "") == Why::AgentUnreachable && r.sink.commits == 0);
    r.session.close(Why::Cancelled);
    Rig q;
    q.to_redeeming();
    CHECK(q.finish() == Why::None && q.session.phase() == Phase::Done && !q.session.is_open());
    CHECK(q.sink.commits == 1 && q.sink.committed.size() == 1);
    const Secrets &s = q.sink.committed[0];
    CHECK(s.ready());
    CHECK(s.ssid == "HomeWifi" && s.password == "wifi-password" && s.url == valid_vectors[0].url && s.agent_id == "agent-test" && s.token == std::string(43, 'T'));
    CHECK(s.ca_pem == pem_of(kCaDer));                                                                        // the pinned CA, not another one
    CHECK(s.update_public_key_pem == "UPDATE KEY" && !s.update_key_explicit && s.ntp_server == "ntp.old.example");   // what pairing does not own stays
    CHECK(!q.session.holds_secrets() && q.session.offer().hotspot_password.empty() && q.session.package().code.empty() && q.session.ca_pem().empty());
    CHECK(q.session.open(q.now, true, false) == Why::NotAllowed && q.session.open(q.now, true, true) == Why::None);   // a new pairing needs a long press
    // power cut: the commit does not finish, the old configuration remains and nothing half-written is read back
    Rig c;
    c.to_redeeming();
    c.sink.power_cut = true;
    CHECK(c.finish() == Why::SaveFailed && same_secrets(c.sink.stored, c.original) && c.session.phase() == Phase::SaveFailed);
    // an incomplete configuration is never committed
    Rig d;
    d.to_redeeming();
    CHECK(d.session.redeemed(d.now, Tls::Ok, 200, good_body(std::string(43, 'T'), "")) == Why::BadReply && d.sink.commits == 0);
}
static void e15_open() {
    {
        Rig r;                                                                                                    // paired: nothing without a long press
        CHECK(r.open(true, false) == Why::NotAllowed && r.session.phase() == Phase::Closed && r.session.offer().setup_code.empty() && r.session.offer().hotspot_password.empty());
        CHECK(r.open(true, false) == Why::NotAllowed);
        CHECK(r.open(true, true) == Why::None && r.session.is_open());
    }
    {
        Rig r;                                                                                                    // unpaired: opens by itself, once per boot
        CHECK(r.open(false, false) == Why::None);
        r.session.close(Why::Cancelled);
        CHECK(r.session.phase() == Phase::Closed && r.session.last_why() == Why::Cancelled && !r.session.holds_secrets());
        CHECK(r.open(false, false) == Why::NotAllowed);                                                           // not again, unless the person presses
        CHECK(r.open(false, true) == Why::None);
    }
    {
        Rig r;                                                                                                    // after a timeout the same
        CHECK(r.open(false, false) == Why::None);
        CHECK(r.session.tick(r.now + kWindowMs) == Why::Timeout);
        CHECK(r.open(false, false) == Why::NotAllowed && r.open(false, true) == Why::None);
        r.session.close(Why::Cancelled);
        CHECK(r.session.last_why() == Why::Cancelled);
    }
    {
        Rig r;                                                                                                    // a long press on an unpaired device uses the one automatic opening too
        CHECK(r.open(false, true) == Why::None);
        r.session.close(Why::Cancelled);
        CHECK(r.open(false, false) == Why::NotAllowed);
    }
    {
        Rig r;                                                                                                    // closing a closed session changes nothing
        r.session.close(Why::Cancelled);
        CHECK(r.session.phase() == Phase::Closed);
        r.open();
        r.unlock();
        r.session.close(Why::Cancelled);
        CHECK(r.session.phase() == Phase::Closed && !r.session.holds_secrets() && !r.session.code_accepted() && r.session.offer().hotspot_password.empty());
    }
}

// ---- F1 ----
static void f1_random_walk() {
    XorRandom dice;
    auto roll = [&](uint32_t n) { uint8_t b[4]; dice.fill(b, 4); return (static_cast<uint32_t>(b[0]) | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24) % n; };
    for (int game = 0; game < 400; ++game) {
        Rig r;
        long steps = 2500;
        int commits_seen = 0;
        while (steps--) {
            r.now += roll(4) == 0 ? roll(20000) : roll(300);
            const bool was_open = r.session.is_open();
            const Phase before = r.session.phase();
            const int commits_before = r.sink.commits;
            const std::string code = r.session.offer().setup_code;
            switch (roll(16)) {
            case 0: r.session.tick(r.now); break;
            case 1: r.session.enter_code(r.now, code.empty() || roll(3) == 0 ? "000000" : code); break;
            case 2: r.session.enter_code(r.now, roll(2) ? code : "12345"); break;
            case 3: r.session.submit(r.now, r.form()); break;
            case 4: { SetupForm f = r.form(); f.ssid = roll(2) ? "" : f.ssid; r.session.submit(r.now, f); break; }
            case 5: r.session.joined(r.now, roll(4) != 0); break;
            case 6: r.session.ca_fetched(r.now, roll(5) != 0, r.der(), pem_of(roll(6) == 0 ? "another CA" : kCaDer)); break;
            case 7: r.session.redeemed(r.now, static_cast<Tls>(roll(5)), roll(3) == 0 ? 403 : 200, roll(5) == 0 ? "junk" : good_body()); break;
            case 8: r.sink.commit_ok = roll(3) != 0; r.sink.corrupt_readback = roll(4) == 0; r.sink.power_cut = roll(5) == 0; break;
            case 9: r.session.retry_save(r.now); break;
            case 10: r.session.open(r.now, roll(2), roll(2)); break;
            case 11: r.session.close(Why::Cancelled); break;
            default: if (roll(2)) r.session.enter_code(r.now, code); else r.session.redeemed(r.now, Tls::Ok, 200, good_body()); break;
            }
            const bool redeem_call_possible = before == Phase::Redeeming || before == Phase::SaveFailed;
            if (r.sink.commits != commits_before) {
                CHECK(was_open && redeem_call_possible);                                   // only from a redemption or a retry, only while open
                const Secrets &s = r.sink.committed.back();
                CHECK(s.ready() && s.ca_pem == pem_of(kCaDer) && s.update_public_key_pem == "UPDATE KEY" && s.ssid == "HomeWifi" && s.url == valid_vectors[0].url);
                ++commits_seen;
            }
            const Phase now_phase = r.session.phase();
            if (now_phase == Phase::Closed || now_phase == Phase::Done) CHECK(!r.session.holds_secrets() && r.session.offer().setup_code.empty() && r.session.offer().hotspot_password.empty());
            if (now_phase == Phase::Open) CHECK(!r.session.holds_secrets());
            const int64_t left = r.session.remaining_ms(r.now);
            CHECK(left >= 0 && left <= kWindowMs);
            CHECK(r.session.wrong_codes() <= kMaxWrongCodes && r.session.attempts() <= kMaxAttempts);
            if (r.session.phase() != Phase::Done && commits_seen == 0) CHECK(same_secrets(r.sink.stored, r.original));   // nothing stored before a commit
            CHECK(r.sink.commits >= commits_seen);
            {   // what is stored is the old configuration or a complete one that was committed, never anything in between
                bool known = same_secrets(r.sink.stored, r.original);
                for (const Secrets &c : r.sink.committed) known = known || same_secrets(r.sink.stored, c);
                CHECK(known);
            }
            for (const Secrets &c : r.sink.committed) CHECK(c.ready());
        }
    }
}

int main() {
    load_vectors();
    h1_sha256();
    v1_vectors();
    v2_tampering();
    v3_origin();
    n1_pin();
    n2_reply();
    i1_input();
    r1_random();
    b1_long_press();
    t1_countdown();
    e_offer();
    e10_wrong_code();
    e11_window();
    e12_aborts();
    e13_save_fails();
    e14_commit();
    e15_open();
    f1_random_walk();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("pairing_test: all checks passed");
    return 0;
}
