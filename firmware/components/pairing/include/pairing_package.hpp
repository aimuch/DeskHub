#pragma once
// The pairing package the console shows once (docs/pairing-design.md section 4), and the small pure helpers around it:
// SHA-256, the CA pin, the redeem reply. No I/O, no logging: the host tests in firmware/tests/pairing_test.cpp check
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

struct Package {
    std::string url, grant_id, code;
    Digest fingerprint{};
};
enum class PackageError { None, Prefix, Characters, Base64, NotCanonical, TooShort, Check, Version, Length, Url, Alphabet };

// `https://host` or `https://host:port`, nothing else: no path, query, fragment, user info or white space.
bool valid_https_origin(const std::string &url);
struct Origin {
    std::string host;  // an IPv6 address without its brackets
    int port = 443;
};
bool split_origin(const std::string &url, Origin &out);
// `DHP1.` plus canonical base64url. White space around the text is ignored (copy and paste), inside it is an error.
bool decode_package(const std::string &text, Package &out, PackageError *why = nullptr);

// The device holds only the CA fingerprint from the package. A CA it fetched is the trust root only if its DER form
// hashes to that fingerprint.
bool pin_matches(const Digest &expected, const std::vector<uint8_t> &ca_der);
// The first CERTIFICATE block of a PEM text as DER. false for anything else, including a private key.
bool pem_to_der(const std::string &pem, std::vector<uint8_t> &der);

struct Reply {
    std::string agent_id, token, ca_pem;
};
// The body of a 200 from POST /api/v1/pairing/redeem. The returned CA must be the pinned one.
bool parse_redeem_reply(const std::string &body, const Digest &pinned, Reply &out);
}  // namespace dh::pairing
