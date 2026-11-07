#!/usr/bin/env python3
"""Mutation check for the firmware update over Bluetooth (docs/desktop-app-design.md 13.11, O1 to O9): break the code on
purpose, ble_update_test must go red.

Usage: scripts/mutation/ble_update.py [name-part ...]
Works on a copy in a temporary directory. Exit 0 only if the baseline is green and every mutation is caught.
"""
from _engine import firmware_main

U = "components/ble_link/ble_update.cpp"

M = [
    # O1 frames
    ("Begin with extra bytes", U, "    case 'B': if (n == 1) f.kind = Kind::Begin; break;", "    case 'B': f.kind = Kind::Begin; break;"),
    ("a manifest frame without bytes", U, "        if (n > 3) { f.kind = Kind::Manifest;", "        if (n >= 3) { f.kind = Kind::Manifest;"),
    ("a data frame without bytes", U, "        if (n > 5) {\n            f.kind = Kind::Data;", "        if (n >= 5) {\n            f.kind = Kind::Data;"),
    ("the high byte of the data offset is lost", U, " | static_cast<uint32_t>(p[4]) << 24;", ";"),
    # O2 versions
    ("versions compare as text", U, "        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;", "        if (x[i] != y[i]) return std::to_string(x[i]) < std::to_string(y[i]) ? -1 : 1;"),
    ("a trailing dot is a version", U, "        if (s[i] != '.') return false;\n        ++i;\n", "        if (s[i] != '.') return false;\n        ++i;\n        if (i == s.size()) return true;\n"),
    ("missing parts are not zero", U, "    x.resize(n, 0);\n    y.resize(n, 0);\n", "    if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;\n"),
    # O3 prefix
    ("any chip", U, "p[0] != 0xE9 || (p[12] | p[13] << 8) != 9) return false;", "p[0] != 0xE9) return false;"),
    ("no descriptor magic", U, "    if (magic != 0xABCD5432) return false;\n", "    (void)magic;\n"),
    ("any project", U, " && std::strcmp(reinterpret_cast<const char *>(p + 80), \"deskhub\") == 0;", ";"),
    ("a version without its end", U, "    if (!vend || !pend) return false;\n", "    (void)vend;\n    if (!pend) return false;\n"),
    # O4 manifest
    ("a gap in the manifest", U, "        if (f.offset != buffer_.size()) return abort_(\"UPDATE_OUT_OF_ORDER\");\n", ""),
    ("a manifest of any size", U, "        if (buffer_.size() + f.len > kMaxManifest) return abort_(\"UPDATE_MANIFEST_TOO_LONG\");\n", ""),
    ("a refused manifest opens", U, "        if (!k.ok) return refuse(k.why.empty() ? \"UPDATE_SIGNATURE_INVALID\" : k.why);\n", ""),
    # O5 refusals
    ("the same version is taken", U, "if (compare_versions(k.version, c.running, &comparable) <= 0 || !comparable)", "if (compare_versions(k.version, c.running, &comparable) < 0 || !comparable)"),
    ("a version that cannot be compared is taken", U, "if (compare_versions(k.version, c.running, &comparable) <= 0 || !comparable)", "if (compare_versions(k.version, c.running, &comparable) < 0)"),
    ("busy is ignored", U, "        if (!c.allowed) return refuse(c.not_allowed.empty() ? \"UPDATE_BUSY\" : c.not_allowed);\n", ""),
    ("larger than the slot", U, "        if (k.size > c.slot || k.size > 6u * 1024 * 1024) return refuse(\"UPDATE_TOO_LARGE\");", "        if (k.size > 6u * 1024 * 1024) return refuse(\"UPDATE_TOO_LARGE\");"),
    ("smaller than the prefix", U, "        if (k.size < kPrefix) return refuse(\"UPDATE_TOO_SMALL\");\n", ""),
    # O6 data
    ("data before the slot is open", U, "        if (phase_ != Phase::Receiving) return abort_(\"UPDATE_NOT_READY\");", "        if (phase_ != Phase::Receiving && phase_ != Phase::Opening) return abort_(\"UPDATE_NOT_READY\");"),
    ("an offset that does not follow on", U, "        if (f.offset != received_) return abort_(\"UPDATE_OUT_OF_ORDER\");\n", ""),
    ("more bytes than the manifest", U, "        if (received_ + f.len > checked_.size) return abort_(\"UPDATE_TOO_MUCH\");\n", ""),
    ("a slot that could not open still takes data", U, "    if (ok) { phase_ = Phase::Receiving; received_ = 0; }\n    else reset();", "    phase_ = Phase::Receiving; received_ = 0; (void)ok;"),
    # O7 end
    ("an early end finishes", U, "if (phase_ != Phase::Receiving || received_ != checked_.size) return abort_(\"UPDATE_SHORT\");", "if (phase_ != Phase::Receiving) return abort_(\"UPDATE_SHORT\");"),
    # O8 abort and idle
    ("a bad frame does not drop the update", U, "    case Kind::Bad: return active ? abort_(\"UPDATE_BAD_FRAME\") : Plan{};", "    case Kind::Bad: return Plan{};"),
    ("cancel does nothing", U, "    case Kind::Abort: return active ? abort_(\"UPDATE_CANCELLED\") : Plan{};", "    case Kind::Abort: return Plan{};"),
    ("no idle limit", U, "    if (now_ms - last_ > kIdleMs) return abort_(\"UPDATE_IDLE\");\n", "    if (now_ms < 0) return abort_(\"UPDATE_IDLE\");\n"),
    ("activity is not noted", U, "    const bool active = phase_ != Phase::Idle;\n    last_ = now_ms;\n", "    const bool active = phase_ != Phase::Idle;\n"),
]

if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "ble_update_test"))
