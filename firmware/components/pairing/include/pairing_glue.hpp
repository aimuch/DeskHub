#pragma once
// The pure parts of the glue between the hotspot pairing session and the hardware (docs/pairing-design.md section 13):
// what the setup page may send, what a failed TLS handshake means, who may talk to the page, what the redemption asks,
// and the page itself. No I/O, no logging, and no secret in any answer: an error is a name, not the input.
// Host tests: firmware/tests/pairing_glue_test.cpp.
#include "pairing_package.hpp"
#include "pairing_session.hpp"
#include <cstdint>
#include <string>

namespace dh::pairing {
constexpr size_t kMaxFormBytes = 4096;  // the whole body of the form; a package is at most about 3.1 KB of it

struct FormFields {
    std::string code, ssid, wifi_password, package, utc;
};
enum class FormError { None, TooLarge, Syntax, Duplicate, Unknown, Missing, Control };
const char *form_error_name(FormError);
// application/x-www-form-urlencoded, strictly: code, ssid, password, package, utc, each exactly once and nothing else.
// The output is cleared first and stays empty on any error.
FormError parse_form(const std::string &body, FormFields &out);

// The verdict of a TLS handshake from the verify flags (mbedTLS: 0x01 expired, 0x02 revoked, 0x04 name, 0x08 not trusted).
Tls tls_from_flags(bool connected, int flags);

// Host order. A zero network or a zero mask matches nothing.
bool ipv4_in_subnet(uint32_t addr, uint32_t network, uint32_t mask);

std::string redeem_url(const Package &);
// `{"grant_id":..,"code":..,"device_id":..}`; empty when any value is not made of the characters it may have.
std::string redeem_request_body(const Package &, const std::string &device_id);

const char *setup_page_html();
std::string setup_result_html(bool accepted);
}  // namespace dh::pairing
