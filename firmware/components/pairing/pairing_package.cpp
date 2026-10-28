#include "pairing_package.hpp"
#include "json_util.hpp"
#include "pairing_input.hpp"
#include <algorithm>
#include <cstring>

namespace dh::pairing {
// ---------------------------------------------------------------- SHA-256 (FIPS 180-4)
namespace {
constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
    0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
    0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
    0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16 | static_cast<uint32_t>(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace

Digest sha256(const uint8_t *data, size_t length) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t done = 0;
    for (; length - done >= 64; done += 64) block(h, data + done);
    uint8_t tail[128] = {};
    const size_t rest = length - done;
    if (rest) std::memcpy(tail, data + done, rest);
    tail[rest] = 0x80;
    const size_t total = rest < 56 ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(length) * 8;
    for (int i = 0; i < 8; ++i) tail[total - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    block(h, tail);
    if (total == 128) block(h, tail + 64);
    Digest out;
    for (int i = 0; i < 8; ++i) for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
    return out;
}

bool equal_constant_time(const uint8_t *a, const uint8_t *b, size_t length) {
    uint8_t diff = 0;
    for (size_t i = 0; i < length; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

// ---------------------------------------------------------------- the address
namespace {
bool alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
}  // namespace

bool split_origin(const std::string &url, Origin &out) {
    static const char prefix[] = "https://";
    if (url.size() > 120 || url.compare(0, 8, prefix) != 0) return false;
    const std::string rest = url.substr(8);
    if (rest.empty()) return false;
    std::string host, port;
    if (rest[0] == '[') {
        const size_t close = rest.find(']');
        if (close == std::string::npos) return false;
        host = rest.substr(1, close - 1);
        if (host.size() < 2 || host.size() > 45) return false;
        for (char c : host) if (!hex_digit(c) && c != ':' && c != '.') return false;
        const std::string after = rest.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') return false;
            port = after.substr(1);
            if (port.empty()) return false;
        }
    } else {
        const size_t colon = rest.find(':');
        host = rest.substr(0, colon);
        if (colon != std::string::npos) {
            port = rest.substr(colon + 1);
            if (port.empty()) return false;
        }
        if (host.empty() || host.size() > 100 || !alnum(host.front()) || !alnum(host.back())) return false;
        for (char c : host) if (!alnum(c) && c != '.' && c != '-') return false;
    }
    int number = 443;
    if (!port.empty()) {
        if (port.size() > 5) return false;
        number = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return false;
            number = number * 10 + (c - '0');
        }
        if (number < 1 || number > 65535) return false;
    }
    out.host = host;
    out.port = number;
    return true;
}
bool valid_https_origin(const std::string &url) {
    Origin ignored;
    return split_origin(url, ignored);
}

// ---------------------------------------------------------------- the package
namespace {
constexpr char kPrefix[] = "DHP2.";
constexpr uint8_t kVersion = 2;
constexpr size_t kFixed = 8 + 10 + 2;  // grant id, code, the two length bytes of the certificate
int url_safe_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
bool in_alphabet(const std::string &s, size_t length) {
    if (s.size() != length) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'H') || c == 'J' || c == 'K' || c == 'M' || c == 'N' || (c >= 'P' && c <= 'T') || (c >= 'V' && c <= 'Z'))) return false;
    return true;
}
}  // namespace

bool decode_package(const std::string &text, Package &out, PackageError *why) {
    auto fail = [&](PackageError e) { if (why) *why = e; return false; };
    size_t b = 0, e = text.size();
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (b < e && space(text[b])) ++b;
    while (e > b && space(text[e - 1])) --e;
    const std::string t = text.substr(b, e - b);
    if (t.size() <= 5 || t.compare(0, 5, kPrefix) != 0) return fail(PackageError::Prefix);
    const std::string payload = t.substr(5);
    for (char c : payload) if (url_safe_value(c) < 0) return fail(PackageError::Characters);
    if (payload.size() % 4 == 1) return fail(PackageError::Base64);
    std::vector<uint8_t> raw;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : payload) {
        acc = (acc << 6) | static_cast<uint32_t>(url_safe_value(c));
        bits += 6;
        if (bits >= 8) { bits -= 8; raw.push_back(static_cast<uint8_t>(acc >> bits)); acc &= (1u << bits) - 1; }
    }
    if (acc != 0) return fail(PackageError::NotCanonical);  // the unused bits of the last character must be zero
    if (raw.size() < 2 + kFixed + 4) return fail(PackageError::TooShort);
    const size_t body_size = raw.size() - 4;
    const Digest check = sha256(raw.data(), body_size);
    if (!equal_constant_time(check.data(), raw.data() + body_size, 4)) return fail(PackageError::Check);
    if (raw[0] != kVersion) return fail(PackageError::Version);
    const size_t url_size = raw[1];
    if (url_size == 0 || body_size < 2 + url_size + kFixed) return fail(PackageError::Length);
    const size_t length_at = 2 + url_size + 18;
    const size_t ca_size = (static_cast<size_t>(raw[length_at]) << 8) | raw[length_at + 1];
    if (body_size != length_at + 2 + ca_size) return fail(PackageError::Length);  // the stated length must be exactly what is there
    Package p;
    p.url.assign(reinterpret_cast<const char *>(raw.data() + 2), url_size);
    p.grant_id.assign(reinterpret_cast<const char *>(raw.data() + 2 + url_size), 8);
    p.code.assign(reinterpret_cast<const char *>(raw.data() + 2 + url_size + 8), 10);
    p.ca_der.assign(raw.begin() + length_at + 2, raw.begin() + length_at + 2 + ca_size);
    if (!valid_https_origin(p.url)) return fail(PackageError::Url);
    if (!in_alphabet(p.grant_id, 8) || !in_alphabet(p.code, 10)) return fail(PackageError::Alphabet);
    if (!der_sequence_ok(p.ca_der)) return fail(PackageError::Certificate);
    out = p;
    if (why) *why = PackageError::None;
    return true;
}

// ---------------------------------------------------------------- the certificate and the reply
bool der_sequence_ok(const uint8_t *der, size_t length) {
    if (!der || length < 2 || length > kMaxCaBytes || der[0] != 0x30) return false;
    size_t header, content;
    if (der[1] < 0x80) {
        header = 2;
        content = der[1];
    } else if (der[1] == 0x81 && length >= 3) {
        header = 3;
        content = der[2];
        if (content < 0x80) return false;  // not minimal: the short form would have done
    } else if (der[1] == 0x82 && length >= 4) {
        header = 4;
        content = (static_cast<size_t>(der[2]) << 8) | der[3];
        if (content < 0x100) return false;  // not minimal
    } else {
        return false;  // indefinite length, three length bytes, or a header that is cut off
    }
    return header + content == length;
}

namespace {
int std_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

bool pem_to_der(const std::string &pem, std::vector<uint8_t> &der) {
    static const std::string begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    if (pem.size() > 8192 || pem.find("PRIVATE KEY") != std::string::npos) return false;  // a private key is never touched
    const size_t from = pem.find(begin);
    if (from == std::string::npos) return false;
    const size_t start = from + begin.size(), stop = pem.find(end, start);
    if (stop == std::string::npos) return false;
    std::string body;
    for (size_t i = start; i < stop; ++i) if (pem[i] != '\r' && pem[i] != '\n' && pem[i] != ' ' && pem[i] != '\t') body += pem[i];
    if (body.empty() || body.size() % 4 != 0) return false;
    size_t padding = 0;
    while (padding < 2 && body.size() > padding && body[body.size() - 1 - padding] == '=') ++padding;
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < body.size() - padding; ++i) {
        const int v = std_value(body[i]);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>(acc >> bits)); acc &= (1u << bits) - 1; }
    }
    if (out.empty()) return false;
    der = std::move(out);
    return true;
}

std::string der_to_pem(const std::vector<uint8_t> &der) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (der.empty()) return std::string();
    std::string body;
    for (size_t i = 0; i < der.size(); i += 3) {
        uint32_t v = static_cast<uint32_t>(der[i]) << 16;
        if (i + 1 < der.size()) v |= static_cast<uint32_t>(der[i + 1]) << 8;
        if (i + 2 < der.size()) v |= der[i + 2];
        body += table[v >> 18];
        body += table[(v >> 12) & 63];
        body += i + 1 < der.size() ? table[(v >> 6) & 63] : '=';
        body += i + 2 < der.size() ? table[v & 63] : '=';
    }
    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for (size_t i = 0; i < body.size(); i += 64) pem += body.substr(i, 64) + "\n";
    return pem + "-----END CERTIFICATE-----\n";
}

bool parse_redeem_reply(const std::string &body, const std::vector<uint8_t> &package_ca_der, Reply &out) {
    cJSON *root = dh::parse_json_strict(body.c_str(), body.size());
    if (!root) return false;
    struct Free { cJSON *p; ~Free() { cJSON_Delete(p); } } guard{root};
    if (!cJSON_IsObject(root)) return false;
    int members = 0;
    for (cJSON *c = root->child; c; c = c->next) ++members;
    if (members != 5) return false;
    auto text = [&](const char *key, std::string &value) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsString(v) || !v->valuestring) return false;
        value = v->valuestring;
        return true;
    };
    auto strings = [&](const char *key, int most) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
        if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > most) return false;
        cJSON *item;
        cJSON_ArrayForEach(item, v) if (!cJSON_IsString(item) || !item->valuestring || std::strlen(item->valuestring) > 96) return false;
        return true;
    };
    Reply r;
    if (!text("agent_id", r.agent_id) || !text("token", r.token) || !text("ca_pem", r.ca_pem) || !strings("scopes", 3) || !strings("targets", 4)) return false;
    if (!valid_identifier(r.agent_id) || !valid_token(r.token) || !valid_ca_pem(r.ca_pem)) return false;
    std::vector<uint8_t> der;
    if (package_ca_der.empty() || !pem_to_der(r.ca_pem, der) || der != package_ca_der) return false;  // the CA in the reply must be the one of the package
    out = r;
    return true;
}
}  // namespace dh::pairing
