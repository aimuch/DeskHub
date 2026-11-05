// Failure modes of the glue between the hotspot pairing session and the hardware (docs/pairing-design.md section 13),
// the parts that are pure: the form of the setup page, the verdict of a failed TLS handshake, the address check of the
// page, the request of the redemption and the page itself. Written before the code. The hotspot, the HTTP server, the
// TLS client and the flash are ESP-IDF glue; they are reviewed against the list in section 13 and checked on a device.
//
// G1  The form of the setup page is read strictly: five members, each exactly once, nothing else; at most 4 KiB;
//     percent escapes are two hex digits; '+' is a space; a value has no control character (not even %00 or DEL)
//     and no more than its own limit; the package may carry a line feed at its ends (copy and paste) and nowhere else.
//     A secret is never part of an answer: the error is a name, not a message with the input in it.
// G2  A failed TLS handshake is read from the verify flags: not trusted beats everything, then expired, then the host
//     name, any other flag is a bad chain; no connection and no flags is "unreachable"; a clean handshake is Ok.
// G3  The page answers only to the hotspot: an address in the network of the hotspot passes, any other (the LAN, the
//     loopback, zero) does not, whatever the mask.
// G4  The redemption request is the three members of the Agent's contract and nothing more, built from checked values.
// G5  The page is small, posts to /setup, carries the five members, has no external address, no cookie, no script
//     source, and asks the phone for its clock.
// G6  The hotspot opens by itself only on a device with no Wi-Fi stored: a device that has one (paired, or set with the
//     keyboard and not paired) would lose its network for five minutes after every start, seen on a device on 2026-11-05.
//     It opens once, not again after it timed out, and not before the grace period.
#include "pairing_glue.hpp"
#include <cstdio>
#include <string>

using namespace dh::pairing;

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                             \
    } while (0)

static const std::string kGood = "code=123456&ssid=Home+Net&password=p%40ss+word1&package=DHP2.abc&utc=2026-10-29T10%3A00%3A00Z";

static void g1_form() {
    FormFields f;
    CHECK(parse_form(kGood, f) == FormError::None);
    CHECK(f.code == "123456" && f.ssid == "Home Net" && f.wifi_password == "p@ss word1" && f.package == "DHP2.abc" && f.utc == "2026-10-29T10:00:00Z");
    // The order of the members does not matter; an empty password is an open network.
    CHECK(parse_form("utc=2026-10-29T10%3A00%3A00Z&package=DHP2.abc&password=&ssid=x&code=000000", f) == FormError::None && f.wifi_password.empty());
    // A line feed at the ends of the package (the paste) is dropped; one inside is not allowed.
    CHECK(parse_form("code=1&ssid=x&password=&package=%0ADHP2.abc%0D%0A&utc=u", f) == FormError::None && f.package == "DHP2.abc");
    CHECK(parse_form("code=1&ssid=x&password=&package=DHP2.%0Aabc&utc=u", f) == FormError::Control);
    // Missing, duplicated, unknown, malformed.
    CHECK(parse_form("code=1&ssid=x&password=&package=p", f) == FormError::Missing);
    CHECK(parse_form("", f) == FormError::Missing);
    CHECK(parse_form(kGood + "&code=654321", f) == FormError::Duplicate);
    CHECK(parse_form(kGood + "&admin=1", f) == FormError::Unknown);
    CHECK(parse_form(kGood + "&", f) == FormError::Syntax);
    CHECK(parse_form("&" + kGood, f) == FormError::Syntax);
    CHECK(parse_form("code&ssid=x&password=&package=p&utc=u", f) == FormError::Syntax);
    CHECK(parse_form("=1&ssid=x&password=&package=p&utc=u", f) == FormError::Syntax);
    // Percent escapes.
    CHECK(parse_form("code=%3&ssid=x&password=&package=p&utc=u", f) == FormError::Syntax);
    CHECK(parse_form("code=%G1&ssid=x&password=&package=p&utc=u", f) == FormError::Syntax);
    CHECK(parse_form("code=1&ssid=x&password=%&package=p&utc=u", f) == FormError::Syntax);
    CHECK(parse_form("code=1&ssid=a%2Bb%26c&password=&package=p&utc=u", f) == FormError::None && f.ssid == "a+b&c");
    CHECK(parse_form("code=1&ssid=%e4%b8%ad%E6%96%87&password=&package=p&utc=u", f) == FormError::None && f.ssid == "\xe4\xb8\xad\xe6\x96\x87");
    CHECK(parse_form("code=1&ssid=%4Z&password=&package=p&utc=u", f) == FormError::Syntax);    // a good first digit and a bad second one
    CHECK(parse_form("code=1&ssid=%Z4&password=&package=p&utc=u", f) == FormError::Syntax);
    CHECK(parse_form("code=1&ssid=%4&password=&package=p&utc=u", f) == FormError::Syntax);     // the escape may not run into the next member
    CHECK(parse_form("code=1&ssid=x&password=&package=p&utc=%4", f) == FormError::Syntax);     // or past the end of the body
    // Control characters, in every member.
    for (const char *bad : {"%00", "%01", "%1F", "%7f", "%09", "%0A"}) {
        CHECK(parse_form(std::string("code=1&ssid=a") + bad + "b&password=&package=p&utc=u", f) == FormError::Control);
        CHECK(parse_form(std::string("code=1&ssid=x&password=a") + bad + "b&package=p&utc=u", f) == FormError::Control);
        CHECK(parse_form(std::string("code=") + bad + "&ssid=x&password=&package=p&utc=u", f) == FormError::Control);
    }
    // Limits of the members (the whole body has its own).
    CHECK(parse_form("code=1234567&ssid=x&password=&package=p&utc=u", f) == FormError::TooLarge);
    CHECK(parse_form("code=1&ssid=" + std::string(33, 'a') + "&password=&package=p&utc=u", f) == FormError::TooLarge);
    CHECK(parse_form("code=1&ssid=" + std::string(32, 'a') + "&password=&package=p&utc=u", f) == FormError::None);
    CHECK(parse_form("code=1&ssid=x&password=" + std::string(64, 'a') + "&package=p&utc=u", f) == FormError::TooLarge);
    CHECK(parse_form("code=1&ssid=x&password=" + std::string(63, 'a') + "&package=p&utc=u", f) == FormError::None);
    CHECK(parse_form("code=1&ssid=x&password=&package=" + std::string(3301, 'a') + "&utc=u", f) == FormError::TooLarge);
    CHECK(parse_form("code=1&ssid=x&password=&package=" + std::string(3300, 'a') + "&utc=u", f) == FormError::None);
    CHECK(parse_form("code=1&ssid=x&password=&package=p&utc=" + std::string(21, '1'), f) == FormError::TooLarge);
    CHECK(parse_form("code=1&ssid=x&password=&package=p&utc=u&" + std::string(kMaxFormBytes, 'a') + "=1", f) == FormError::TooLarge);
    // The limit of the limit is the body, before anything is decoded: 4096 bytes pass, 4097 do not.
    std::string edge = "code=1&ssid=x&password=&package=" + std::string(3300, 'a') + "&utc=u&";
    CHECK(edge.size() < kMaxFormBytes);
    std::string padded = "code=1&ssid=x&password=&package=p&utc=u";
    CHECK(parse_form(padded + std::string(kMaxFormBytes - padded.size() + 1, '&'), f) == FormError::TooLarge);
    // A refused form leaves nothing behind, also when the refusal comes after some members were read.
    CHECK(parse_form(kGood + "&code=654321", f) == FormError::Duplicate && f.code.empty() && f.ssid.empty() && f.wifi_password.empty() && f.package.empty() && f.utc.empty());
    CHECK(parse_form("code=1&ssid=x&password=a%01b&package=p&utc=u", f) == FormError::Control && f.code.empty() && f.ssid.empty());
    CHECK(parse_form("code=1&ssid=x&password=&package=p&utc=u&zzz=1", f) == FormError::Unknown && f.code.empty() && f.package.empty());
    f.package = "left over";
    CHECK(parse_form("nonsense", f) != FormError::None && f.package.empty() && f.code.empty() && f.ssid.empty() && f.wifi_password.empty());
    CHECK(std::string(form_error_name(FormError::Control)) == "control" && std::string(form_error_name(FormError::None)) == "none");
}

static void g2_tls() {
    CHECK(tls_from_flags(true, 0) == dh::pairing::Tls::Ok);
    CHECK(tls_from_flags(false, 0) == dh::pairing::Tls::Unreachable);
    CHECK(tls_from_flags(false, 0x01) == dh::pairing::Tls::CertificateExpired);
    CHECK(tls_from_flags(false, 0x04) == dh::pairing::Tls::HostnameMismatch);
    CHECK(tls_from_flags(false, 0x08) == dh::pairing::Tls::ChainInvalid);
    CHECK(tls_from_flags(false, 0x02) == dh::pairing::Tls::ChainInvalid);   // revoked
    CHECK(tls_from_flags(false, 0x80) == dh::pairing::Tls::ChainInvalid);   // any other flag
    CHECK(tls_from_flags(false, 0x01 | 0x08) == dh::pairing::Tls::ChainInvalid);       // not trusted beats expired
    CHECK(tls_from_flags(false, 0x04 | 0x08) == dh::pairing::Tls::ChainInvalid);
    CHECK(tls_from_flags(false, 0x01 | 0x04) == dh::pairing::Tls::CertificateExpired);  // expired beats the name
    CHECK(tls_from_flags(false, 0x04 | 0x80) == dh::pairing::Tls::HostnameMismatch);    // a known reason beats an unknown one
    // A "connected" handshake with flags is not a good one.
    CHECK(tls_from_flags(true, 0x01) == dh::pairing::Tls::CertificateExpired);
    CHECK(tls_from_flags(true, 0x08) == dh::pairing::Tls::ChainInvalid);
}

static void g3_peer() {
    const uint32_t net = 0xC0A80400, mask = 0xFFFFFF00;   // 192.168.4.0/24
    CHECK(ipv4_in_subnet(0xC0A80402, net, mask));
    CHECK(ipv4_in_subnet(0xC0A804FE, net, mask));
    CHECK(!ipv4_in_subnet(0xC0A80502, net, mask));       // 192.168.5.2
    CHECK(!ipv4_in_subnet(0xC0A80302, net, mask));       // 192.168.3.2
    CHECK(!ipv4_in_subnet(0x0A0A0A15, net, mask));       // 10.10.10.21: the LAN
    CHECK(!ipv4_in_subnet(0x7F000001, net, mask));       // loopback
    CHECK(!ipv4_in_subnet(0, net, mask));
    CHECK(!ipv4_in_subnet(0xC0A80402, 0, 0xFFFFFF00));   // a network of zero is nothing
    CHECK(!ipv4_in_subnet(5, 0, 0xFFFFFF00));            // not even for the addresses that look like it
    CHECK(ipv4_in_subnet(0xC0A80402, 0xC0A80401, mask)); // the address of the interface itself, with its host bits, names the network
    CHECK(!ipv4_in_subnet(0xC0A80502, 0xC0A80401, mask));
    CHECK(!ipv4_in_subnet(0xC0A80402, net, 0));          // no mask: nothing matches, not everything
    CHECK(ipv4_in_subnet(0xC0A80402, net, 0xFFFFFFFF) == false);   // a /32 is a single host, and it is not this one
    CHECK(ipv4_in_subnet(0xC0A80400, net, 0xFFFFFFFF));
}

static void g4_request() {
    Package p;
    p.url = "https://10.10.10.5:8765";
    p.grant_id = "ABCD2345";
    p.code = "KLMNPQRSTV";
    CHECK(redeem_request_body(p, "deskhub-44bd8d60c598") == "{\"grant_id\":\"ABCD2345\",\"code\":\"KLMNPQRSTV\",\"device_id\":\"deskhub-44bd8d60c598\"}");
    CHECK(redeem_url(p) == "https://10.10.10.5:8765/api/v1/pairing/redeem");
    p.url = "https://agent.local";
    CHECK(redeem_url(p) == "https://agent.local/api/v1/pairing/redeem");
    // Values that would need escaping are never put in the body: the caller must have checked them, and this refuses.
    CHECK(redeem_request_body(p, "bad\"id").empty());
    CHECK(redeem_request_body(p, "").empty());
    p.code = "AB\"CD";
    CHECK(redeem_request_body(p, "deskhub-1").empty());
    p.code = "KLMNPQRSTV";
    p.grant_id = "AB\\CD";
    CHECK(redeem_request_body(p, "deskhub-1").empty());
}

static void g5_page() {
    const std::string page = setup_page_html();
    CHECK(page.size() > 200 && page.size() < 4096);
    CHECK(page.find("action=\"/setup\"") != std::string::npos && page.find("method=\"post\"") != std::string::npos);
    for (const char *name : {"name=\"code\"", "name=\"ssid\"", "name=\"password\"", "name=\"package\"", "name=\"utc\""}) CHECK(page.find(name) != std::string::npos);
    CHECK(page.find("http://") == std::string::npos && page.find("https://") == std::string::npos && page.find("//cdn") == std::string::npos);
    CHECK(page.find("<script src") == std::string::npos && page.find("<link") == std::string::npos && page.find("@import") == std::string::npos);
    CHECK(page.find("document.cookie") == std::string::npos && page.find("localStorage") == std::string::npos);
    CHECK(page.find("toISOString") != std::string::npos);     // the phone's clock is the clock of the device
    size_t off = 0;
    for (size_t at = page.find("autocomplete=\"off\""); at != std::string::npos; at = page.find("autocomplete=\"off\"", at + 1)) ++off;
    CHECK(off >= 5);                                      // the form and each of the four fields the phone must not remember
    CHECK(page.find("type=\"password\"") != std::string::npos);    // the Wi-Fi password is not shown on the phone
    CHECK(page.find("DHP2.") != std::string::npos);        // the field says what to paste
    const std::string answer = setup_result_html(true), refused = setup_result_html(false);
    CHECK(!answer.empty() && !refused.empty() && answer != refused && answer.size() < 1024 && refused.size() < 1024);
    CHECK(answer.find("<form") == std::string::npos && refused.find("<form") == std::string::npos);
    CHECK(answer.find("已收到") != std::string::npos && refused.find("没有接受") != std::string::npos && answer.find("没有接受") == std::string::npos && refused.find("已收到") == std::string::npos);
}

static void g6_auto_open() {
    const int64_t grace = 8000;
    CHECK(auto_open_hotspot(false, false, 8000, grace));           // no Wi-Fi: opens at the end of the grace period
    CHECK(auto_open_hotspot(false, false, 60000, grace));
    CHECK(!auto_open_hotspot(false, false, 7999, grace));          // not before
    CHECK(!auto_open_hotspot(false, false, 0, grace));
    CHECK(!auto_open_hotspot(false, true, 60000, grace));          // once: not again after it timed out
    CHECK(!auto_open_hotspot(true, false, 8000, grace));           // a stored Wi-Fi keeps the network
    CHECK(!auto_open_hotspot(true, false, 600000, grace));
    CHECK(!auto_open_hotspot(true, true, 600000, grace));
}

int main() {
    g1_form();
    g2_tls();
    g3_peer();
    g4_request();
    g5_page();
    g6_auto_open();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("pairing_glue_test: all checks passed");
    return 0;
}
