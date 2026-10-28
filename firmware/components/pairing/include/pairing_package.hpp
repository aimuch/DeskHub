#pragma once
// The pairing package the console shows once (docs/pairing-design.md section 4), and the small pure helpers around it:
// SHA-256, the CA of the package, the redeem reply. No I/O, no logging: the host tests in firmware/tests/pairing_test.cpp check
// every rule against the vectors in protocol/examples/pairing-package.json, which the Agent's decoder passes too.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dh::pairing {
using Digest = std::array<uint8_t, 32>;

Digest sha256(const uint8_t *data, size_t length);
inline Digest sha256(const std::string &text) { return sha256(reinterpret_cast<const uint8_t *>(text.data()), text.size()); }
bool equal_constant_time(const uint8_t *a, const uint8_t *b, size_t length);

constexpr size_t kMaxCaBytes = 2048;  // one DER certificate; RSA-4096 fits
struct Package {
    std::string url, grant_id, code;
    std::vector<uint8_t> ca_der;  // the Agent's CA certificate: the only trust root of the redemption
};
enum class PackageError { None, Prefix, Characters, Base64, NotCanonical, TooShort, Check, Version, Length, Url, Alphabet, Certificate };

// `https://host` or `https://host:port`, nothing else: no path, query, fragment, user info or white space.
bool valid_https_origin(const std::string &url);
struct Origin {
    std::string host;  // an IPv6 address without its brackets
    int port = 443;
};
bool split_origin(const std::string &url, Origin &out);
// `DHP2.` plus canonical base64url. White space around the text is ignored (copy and paste), inside it is an error.
bool decode_package(const std::string &text, Package &out, PackageError *why = nullptr);

// Exactly one DER SEQUENCE and nothing else: tag 0x30, a minimal definite length (short form, 0x81 or 0x82), no bytes
// after it, at most kMaxCaBytes. It does not parse the certificate; the TLS stack does that when it uses it.
bool der_sequence_ok(const uint8_t *der, size_t length);
inline bool der_sequence_ok(const std::vector<uint8_t> &der) { return der_sequence_ok(der.data(), der.size()); }
// The first CERTIFICATE block of a PEM text as DER. false for anything else, including a private key.
bool pem_to_der(const std::string &pem, std::vector<uint8_t> &der);
// The PEM text the TLS stack takes (esp_http_client cert_pem): 64 characters per line, a final line feed. Empty for no input.
std::string der_to_pem(const std::vector<uint8_t> &der);

struct Reply {
    std::string agent_id, token, ca_pem;
};
// The body of a 200 from POST /api/v1/pairing/redeem. The returned CA must be the very CA of the package.
bool parse_redeem_reply(const std::string &body, const std::vector<uint8_t> &package_ca_der, Reply &out);
}  // namespace dh::pairing
