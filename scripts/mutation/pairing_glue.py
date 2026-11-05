#!/usr/bin/env python3
"""Mutation check for the pure glue of hotspot pairing: break the code on purpose, pairing_glue_test must go red.

Usage: scripts/mutation/pairing_glue.py [name-part ...]
Works on a copy in a temporary directory. The working tree is never touched.
Exit 0 only if the baseline is green and every mutation is caught.
"""
from _engine import firmware_main

G = "components/pairing/pairing_glue.cpp"
M = [
    # the form
    ("the body limit is gone", G, "if (body.size() > kMaxFormBytes) return fail(FormError::TooLarge);", ""),
    ("the body limit is one too big", G, "if (body.size() > kMaxFormBytes) return", "if (body.size() > kMaxFormBytes + 1) return"),
    ("an empty body is not missing", G, "if (body.empty()) return fail(FormError::Missing);", ""),
    ("a pair without a name is accepted", G, "|| eq >= end || eq == at) return fail(FormError::Syntax);", "|| eq >= end) return fail(FormError::Syntax);"),
    ("a pair without = is accepted", G, "if (eq == std::string::npos || eq >= end || eq == at) return", "if (eq == at) return"),
    ("a bad hex digit is accepted", G, "if (hi < 0 || lo < 0) return fail(FormError::Syntax);", "if (hi < 0) return fail(FormError::Syntax);"),
    ("plus is not a space", G, "value += ' ';", "value += '+';"),
    ("an unknown member is accepted", G, "if (index < 0) return fail(FormError::Unknown);", "if (index < 0) continue;"),
    ("a duplicate member is accepted", G, "if (seen[index]) return fail(FormError::Duplicate);", ""),
    ("a missing member is accepted", G, "for (bool s : seen) if (!s) return fail(FormError::Missing);", ""),
    ("control characters are accepted", G, "for (unsigned char c : value) if (c < 0x20 || c == 0x7f) return fail(FormError::Control);", ""),
    ("DEL is accepted", G, "if (c < 0x20 || c == 0x7f)", "if (c < 0x20)"),
    ("a tab is accepted", G, "if (c < 0x20 || c == 0x7f)", "if ((c < 0x20 && c != 9) || c == 0x7f)"),
    ("the package keeps its line feeds", G, "while (first < last && is_space(value[first])) ++first;", ""),
    ("the package end is not trimmed", G, "while (last > first && is_space(value[last - 1])) --last;", ""),
    ("every member may be long", G, "if (value.size() > kMembers[index].limit) return fail(FormError::TooLarge);", ""),
    ("the code may have seven digits", G, '{"code", 6}', '{"code", 7}'),
    ("the SSID may have 33 bytes", G, '{"ssid", 32}', '{"ssid", 33}'),
    ("the password may have 64 bytes", G, '{"password", 63}', '{"password", 64}'),
    ("the package limit is smaller", G, '{"package", 3300}', '{"package", 3000}'),
    ("the package limit is bigger", G, '{"package", 3300}', '{"package", 3301}'),
    ("the clock may be long", G, '{"utc", 20}', '{"utc", 21}'),
    ("a refused form leaves its fields", G, "out = FormFields{};  // nothing of a refused form stays", ""),
    # the handshake
    ("a handshake with no flags is trusted when not connected", G, "return connected ? Tls::Ok : Tls::Unreachable;", "(void)connected; return Tls::Ok;"),
    ("a connected handshake with flags is trusted", G, "if (flags == 0) return", "if (flags == 0 || connected) return"),
    ("not trusted is not first", G, "    if (flags & 0x08) return Tls::ChainInvalid;       // not trusted: the rest is noise\n    if (flags & 0x01) return Tls::CertificateExpired;",
     "    if (flags & 0x01) return Tls::CertificateExpired;\n    if (flags & 0x08) return Tls::ChainInvalid;"),
    ("expired is read as chain", G, "if (flags & 0x01) return Tls::CertificateExpired;", "if (flags & 0x01) return Tls::ChainInvalid;"),
    ("the name is read as chain", G, "if (flags & 0x04) return Tls::HostnameMismatch;", "if (flags & 0x04) return Tls::ChainInvalid;"),
    ("an unknown flag is read as the name", G, "return Tls::ChainInvalid;                          // revoked", "return Tls::HostnameMismatch;                          // revoked"),
    ("the name comes before expired", G, "    if (flags & 0x01) return Tls::CertificateExpired;\n    if (flags & 0x04) return Tls::HostnameMismatch;",
     "    if (flags & 0x04) return Tls::HostnameMismatch;\n    if (flags & 0x01) return Tls::CertificateExpired;"),
    # the address
    ("a zero mask matches everything", G, "if (network == 0 || mask == 0) return false;", "if (network == 0) return false;"),
    ("a zero network matches", G, "if (network == 0 || mask == 0) return false;", "if (mask == 0) return false;"),
    ("the mask is not applied to the address", G, "return (addr & mask) == (network & mask);", "return addr == (network & mask);"),
    ("the mask is not applied to the network", G, "return (addr & mask) == (network & mask);", "return (addr & mask) == network;"),
    # the request
    ("the grant id is not checked", G, "if (!all_alnum(p.grant_id) || !all_alnum(p.code) || device_id.empty()) return std::string();", "if (!all_alnum(p.code) || device_id.empty()) return std::string();"),
    ("the code is not checked", G, "if (!all_alnum(p.grant_id) || !all_alnum(p.code) || device_id.empty()) return std::string();", "if (!all_alnum(p.grant_id) || device_id.empty()) return std::string();"),
    ("an empty device id is sent", G, "|| device_id.empty()) return std::string();", ") return std::string();"),
    ("the device id is not checked", G, "for (char c : device_id) if (!name_char(c)) return std::string();", "for (char c : device_id) if (!name_char(c) && false) return std::string();"),
    ("the request has another path", G, 'return p.url + "/api/v1/pairing/redeem";', 'return p.url + "/api/v1/pairing/ca";'),
    # the page
    ("the page posts elsewhere", G, 'action=\\"/setup\\"', 'action=\\"/save\\"'),
    ("the page shows the Wi-Fi password", G, 'name=\\"password\\" type=\\"password\\"', 'name=\\"password\\" type=\\"text\\"'),
    ("the page loads a script", G, "<script>document.getElementById", '<script src=\\"x.js\\"></script><script>document.getElementById'),
    ("the page does not ask for the clock", G, "new Date().toISOString().slice(0,19)+'Z'", "'2026-01-01T00:00:00Z'"),
    ("the page lets the phone remember", G, 'autocomplete=\\"off\\" spellcheck=\\"false\\" required></textarea>', 'spellcheck=\\"false\\" required></textarea>'),
    ("the page has no code field", G, 'name=\\"code\\"', 'name=\\"c\\"'),
    ("the answers are the same", G, '(accepted ? "<h1>已收到</h1>', '(!accepted ? "<h1>已收到</h1>'),
    # the hotspot at boot
    ("a stored Wi-Fi still opens the hotspot", G, "return !has_wifi && !already_tried && now_ms >= grace_ms;", "(void)has_wifi; return !already_tried && now_ms >= grace_ms;"),
    ("the hotspot opens again after it timed out", G, "return !has_wifi && !already_tried && now_ms >= grace_ms;", "(void)already_tried; return !has_wifi && now_ms >= grace_ms;"),
    ("the hotspot opens before the grace period", G, "return !has_wifi && !already_tried && now_ms >= grace_ms;", "(void)now_ms; (void)grace_ms; return !has_wifi && !already_tried;"),
    ("the hotspot never opens by itself", G, "return !has_wifi && !already_tried && now_ms >= grace_ms;", "return false && !has_wifi && !already_tried && now_ms >= grace_ms;"),
    ("the end of the grace period is missed", G, "now_ms >= grace_ms;", "now_ms > grace_ms;"),
]

if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "pairing_glue_test"))
