#!/usr/bin/env python3
"""Mutation check for the device pairing logic: break the code on purpose, pairing_test must go red.

Usage: scripts/mutation/pairing_firmware.py [name-part ...]
Works on a copy in a temporary directory. The working tree is never touched.
Exit 0 only if the baseline is green and every mutation is caught.
"""
from _engine import firmware_main

PK = "components/pairing/pairing_package.cpp"
IN = "components/pairing/pairing_input.cpp"
SE = "components/pairing/pairing_session.cpp"
M = [
    ("non-canonical last character accepted", PK, "if (acc != 0) return fail(PackageError::NotCanonical);", ""),
    ("check bytes ignored", PK, "if (!equal_constant_time(check.data(), raw.data() + body_size, 4)) return fail(PackageError::Check);", "if (!equal_constant_time(check.data(), raw.data() + body_size, 4) && false) return fail(PackageError::Check);"),
    ("version ignored", PK, "if (raw[0] != kVersion) return fail(PackageError::Version);", "if (raw[0] != kVersion && false) return fail(PackageError::Version);"),
    ("bytes after the certificate allowed", PK, "if (body_size != length_at + 2 + ca_size) return fail(PackageError::Length);", "if (body_size < length_at + 2 + ca_size) return fail(PackageError::Length);"),
    ("the stated certificate length is not used", PK, "const size_t ca_size = (static_cast<size_t>(raw[length_at]) << 8) | raw[length_at + 1];", "const size_t ca_size = body_size - length_at - 2;"),
    ("the certificate of the package is not checked", PK, "if (!der_sequence_ok(p.ca_der)) return fail(PackageError::Certificate);", ""),
    ("a certificate that is not a sequence", PK, "length > kMaxCaBytes || der[0] != 0x30) return false;", "length > kMaxCaBytes) return false;"),
    ("a certificate of 4096 bytes", PK, "length > kMaxCaBytes ||", "length > 4096 ||"),
    ("a long length form that is not minimal (0x81)", PK, "if (content < 0x80) return false;  // not minimal: the short form would have done", ""),
    ("a long length form that is not minimal (0x82)", PK, "if (content < 0x100) return false;  // not minimal", ""),
    ("indefinite length and longer forms allowed", PK, "return false;  // indefinite length, three length bytes, or a header that is cut off", "return true;"),
    ("bytes after the DER sequence allowed", PK, "return header + content == length;", "return header + content <= length;"),
    ("a DER sequence longer than its data", PK, "return header + content == length;", "return header + content >= length;"),
    ("the PEM has long lines", PK, 'for (size_t i = 0; i < body.size(); i += 64) pem += body.substr(i, 64) + "\\n";', 'for (size_t i = 0; i < body.size(); i += 76) pem += body.substr(i, 76) + "\\n";'),
    ("the PEM has no final line feed", PK, 'return pem + "-----END CERTIFICATE-----\\n";', 'return pem + "-----END CERTIFICATE-----";'),
    ("the PEM has no padding", PK, "body += i + 1 < der.size() ? table[(v >> 6) & 63] : '=';", "if (i + 1 < der.size()) body += table[(v >> 6) & 63];"),
    ("an empty certificate makes a PEM", PK, "if (der.empty()) return std::string();", ""),
    ("grant id alphabet not checked", PK, "if (!in_alphabet(p.grant_id, 8) ||", "if ((false && !in_alphabet(p.grant_id, 8)) ||"),
    ("code alphabet not checked", PK, "|| !in_alphabet(p.code, 10)) return fail(PackageError::Alphabet);", "|| (!in_alphabet(p.code, 10) && false)) return fail(PackageError::Alphabet);"),
    ("address not checked", PK, "if (!valid_https_origin(p.url)) return fail(PackageError::Url);", ""),
    ("I allowed in the alphabet", PK, "(c >= 'A' && c <= 'H') || c == 'J'", "(c >= 'A' && c <= 'J')"),
    ("prefix ignored", PK, "|| t.compare(0, 5, kPrefix) != 0)", "|| (t.compare(0, 5, kPrefix) != 0 && false))"),
    ("surrounding white space not trimmed", PK, "while (b < e && space(text[b])) ++b;", ""),
    ("host may start with a dash", PK, "!alnum(host.front()) || !alnum(host.back())", "false"),
    ("port 0 allowed", PK, "number < 1 || number > 65535", "number < 0 || number > 65535"),
    ("port of six digits allowed", PK, "if (port.size() > 5) return false;", "if (port.size() > 7) return false;"),
    ("equality of one bit missed", PK, "return diff == 0;", "return diff < 2;"),
    ("a private key next to the certificate allowed", PK, 'pem.find("PRIVATE KEY") != std::string::npos) return false;  // a private key is never touched', "false) return false;"),
    ("extra members in the reply allowed", PK, "if (members != 5) return false;", "if (members < 5) return false;"),
    ("another CA in the reply allowed", PK, "|| der != package_ca_der) return false;", "|| (der != package_ca_der && false)) return false;"),
    ("a longer CA in the reply allowed", PK, "|| der != package_ca_der) return false;", "|| der.size() < package_ca_der.size() || !std::equal(package_ca_der.begin(), package_ca_der.end(), der.begin())) return false;"),
    ("short token in the reply allowed", PK, "!valid_token(r.token) ||", "false ||"),
    ("ssid of 33 bytes", IN, "s.size() <= 32 && !has_nul(s)", "s.size() <= 33 && !has_nul(s)"),
    ("password of 7 characters", IN, "(s.size() >= 8 && s.size() <= 63)", "(s.size() >= 7 && s.size() <= 63)"),
    ("token of 31 characters", IN, "s.size() < 32 || s.size() > 256", "s.size() < 31 || s.size() > 256"),
    ("year 2000 accepted", IN, "y < 2024 ||", "y < 2000 ||"),
    ("29 February in every year", IN, "return m == 2 && leap(y) ? 29 : d[m - 1];", "return m == 2 && (leap(y) || true) ? 29 : d[m - 1];"),
    ("setup code of seven digits", IN, "if (s.size() != 6) return false;", "if (s.size() != 7) return false;"),
    ("countdown rounds down", IN, "(left + 999) / 1000", "left / 1000"),
    ("countdown above the window", IN, "std::min<int64_t>(left, window_ms)", "std::min<int64_t>(left, window_ms + 1000000)"),
    ("countdown without a cap", IN, "std::min(seconds, 99 * 60 + 59)", "std::min(seconds, 100 * 60)"),
    ("hotspot name in lower case", IN, "c = static_cast<char>(c - 'a' + 'A');", "c = static_cast<char>(c - 'a' + 'a');"),
    ("digits biased", SE, "if (b < 250) {", "if (b < 255) {"),
    ("password alphabet of 31", SE, "password += kAlphabet[b & 31];", "password += kAlphabet[b % 31];"),
    ("the fifth wrong code is allowed", SE, "if (++wrong_codes_ >= kMaxWrongCodes) {", "if (++wrong_codes_ > kMaxWrongCodes) {"),
    ("the form is taken before the code", SE, "if (!code_ok_) return Why::CodeRequired;  // before the setup code nothing is accepted", ""),
    ("the window ends one tick late", SE, "if (is_open() && now_ms >= deadline_) {", "if (is_open() && now_ms > deadline_) {"),
    ("the fifth failure does not close", SE, "if (++attempts_ >= kMaxAttempts) {", "if (++attempts_ > kMaxAttempts) {"),
    ("the PEM is not the CA of the package", SE, "    ca_pem_ = pem;\n    phase_ = Phase::Joining;", '    ca_pem_ = "-----BEGIN CERTIFICATE-----\\nAAAA\\n-----END CERTIFICATE-----\\n";\n    phase_ = Phase::Joining;'),
    ("a failed Wi-Fi is not noticed", SE, "if (!ok) return fail_attempt(Why::WifiFailed);", "if (!ok && false) return fail_attempt(Why::WifiFailed);"),
    ("the CA stays after a close", SE, "    wipe(ca_pem_);\n", ""),
    ("the DER of the package stays after a close", SE, "    package_.ca_der.clear();\n", ""),
    ("a refusal is not noticed", SE, "if (http_status == 403) return fail_attempt(Why::RedeemRefused);", ""),
    ("an odd status is accepted", SE, "if (http_status != 200) return fail_attempt(Why::AgentUnreachable);", ""),
    ("expired certificate accepted", SE, "case Tls::CertificateExpired: return fail_attempt(Why::CertificateExpired);", "case Tls::CertificateExpired: break;"),
    ("host name mismatch accepted", SE, "case Tls::HostnameMismatch: return fail_attempt(Why::HostnameMismatch);", "case Tls::HostnameMismatch: break;"),
    ("update key marked explicit", SE, "s.update_key_explicit = false;", "s.update_key_explicit = true;"),
    ("readback not compared", SE, "if (!sink_.readback(stored) || !same_credentials(stored, s)) return failed(Why::ReadbackMismatch);", "if (!sink_.readback(stored) || (!same_credentials(stored, s) && false)) return failed(Why::ReadbackMismatch);"),
    ("a failed commit is ignored", SE, "if (!sink_.commit(s)) return failed(Why::SaveFailed);", ""),
    ("secrets kept after success", SE, "    clear_form();\n    wipe(offer_.hotspot_password); wipe(offer_.setup_code); wipe(offer_.hotspot_ssid);\n    code_ok_ = false;\n    phase_ = Phase::Done;", "    phase_ = Phase::Done;"),
    ("retry without a failure", SE, "if (phase_ != Phase::SaveFailed) return Why::WrongPhase;", ""),
    ("an unpaired device opens again and again", SE, "(!paired && !auto_open_used_)", "(!paired)"),
    ("the automatic opening is never used up", SE, "    auto_open_used_ = true;\n    clear_form();", "    clear_form();"),
    ("a paired device opens without a press", SE, "if (!(long_press || (!paired && !auto_open_used_))) return Why::NotAllowed;", "if (false && !(long_press || (!paired && !auto_open_used_))) return Why::NotAllowed;"),
    ("a glitch restarts the press", SE, "if (had_press_ && now_ms >= released_ && now_ms - released_ < glitch_) down_ = true;", "if (false && had_press_ && now_ms >= released_ && now_ms - released_ < glitch_) down_ = true;"),
    ("a press fires many times", SE, "    fired_ = true;\n    return true;", "    return true;"),
    ("an accepted code counts again", SE, "if (code_ok_) return Why::None;", ""),
    ("the remaining time is not clamped", SE, "return std::max<int64_t>(0, std::min<int64_t>(deadline_ - now_ms, kWindowMs));", "return deadline_ - now_ms;"),
    ("a closed session keeps its offer", SE, "    wipe(offer_.hotspot_password); wipe(offer_.setup_code); wipe(offer_.hotspot_ssid);\n    code_ok_ = false;\n    phase_ = Phase::Closed;", "    code_ok_ = false;\n    phase_ = Phase::Closed;"),
    ("a failed attempt keeps the form", SE, "Why Session::fail_attempt(Why why) {\n    clear_form();", "Why Session::fail_attempt(Why why) {"),
]


if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "pairing_test"))
