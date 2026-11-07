#!/usr/bin/env python3
"""Mutation check for the Bluetooth link's protocol (docs/desktop-app-design.md 13.3, 13.4): break the code on purpose,
ble_protocol_test must go red.

Usage: scripts/mutation/ble_protocol.py [name-part ...]
Works on a copy in a temporary directory. Exit 0 only if the baseline is green and every mutation is caught.
"""
from _engine import firmware_main

P = "components/ble_link/ble_protocol.cpp"

M = [
    ("a message of any size", P, "if (bytes.empty() || bytes.size() > kMaxMessage) return bad(\"size\");", "if (bytes.empty()) return bad(\"size\");"),
    ("extra keys are taken", P, "    if (cJSON_GetArraySize(o) != static_cast<int>(want.size())) return false;\n", ""),
    ("a count over 12", P, "integer(cJSON_GetObjectItemCaseSensitive(j, \"count\"), 0, kMaxSources, &v)", "integer(cJSON_GetObjectItemCaseSensitive(j, \"count\"), 0, 100, &v)"),
    ("a fraction is a number", P, "if (!(v >= lo && v <= hi) || std::floor(v) != v) return false;", "if (!(v >= lo && v <= hi)) return false;"),
    ("a long id", P, "!good_text(id, kMaxId)", "!good_text(id, 1000)"),
    ("a long name", P, "!good_text(name, kMaxName)", "!good_text(name, 1000)"),
    ("control characters", P, "if (c < 0x20 || c == 0x7f) return false; ++i; continue;", "++i; continue;"),
    ("broken UTF-8", P, "        if (i + n > s.size()) return false;\n", "        if (i + n > s.size()) return true;\n"),
    ("any port", P, "|| !good_port(port)) return bad(\"source values\");", "|| !(good_port(port) || true)) return bad(\"source values\");"),
    ("request 0", P, "integer(cJSON_GetObjectItemCaseSensitive(j, \"req\"), 1, 4294967295.0, &v)", "integer(cJSON_GetObjectItemCaseSensitive(j, \"req\"), 0, 4294967295.0, &v)"),
    ("any outcome", P, "        else return bad(\"result outcome\");\n", "        else m.result.outcome = Outcome::Confirmed;\n"),
    ("an index twice", P, "if (!d.open || m.index >= d.expected || d.items[m.index].has_value()) return reject();", "if (!d.open || m.index >= d.expected) return reject();"),
    # Not listed: "a source without begin" is equivalent (without begin the expected count is 0: the index check refuses it).
    ("an id twice in a list", P, "        for (const auto &s : d.items) if (s && s->id == m.source.id) return reject();\n", ""),
    ("an early end", P, "            if (!s) return reject();\n", "            if (!s) continue;\n"),
    ("a second begin", P, "        if (d.open) return reject();\n        d.open = true;", "        d.open = true;"),
    ("two requests at once", P, "    if (pending_) return false;\n    bool known", "    bool known"),
    ("an id outside the list", P, "    if (!known) return false;\n", ""),
    ("a result for another request", P, "if (!pending_ || pending_->req != r.req) return std::nullopt;", "if (!pending_ || r.req == 0) return std::nullopt;"),
    ("no timeout", P, "if (!pending_ || now_ms - pending_->sent_ms < kRequestTimeoutMs) return false;", "if (!pending_ || now_ms >= 0) return false;"),
    ("pairing without the person", P, "return person_said_same && w.is_open(now_ms) && now_ms - digits_shown_ms < kConfirmMs;", "return (person_said_same || true) && w.is_open(now_ms) && now_ms - digits_shown_ms < kConfirmMs;"),
    ("pairing outside the window", P, "return person_said_same && w.is_open(now_ms) && now_ms - digits_shown_ms < kConfirmMs;", "return person_said_same && (w.is_open(now_ms) || true) && now_ms - digits_shown_ms < kConfirmMs;"),
    ("no 30 s for the digits", P, "return person_said_same && w.is_open(now_ms) && now_ms - digits_shown_ms < kConfirmMs;", "return person_said_same && w.is_open(now_ms) && (now_ms - digits_shown_ms < kConfirmMs || true);"),
    ("ids not escaped", P, "        if (c == '\"' || c == '\\\\') { out += '\\\\'; out += c; }\n", "        if (false) {}\n"),
    ("unconfirmed said as confirmed", P, "case Outcome::SentUnconfirmed: return \"已发送到 \" + name + \"，未确认\"", "case Outcome::SentUnconfirmed: return \"已切到 \" + name + \"，已确认\""),
]

if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "ble_protocol_test"))
