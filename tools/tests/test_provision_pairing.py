"""Failure modes of the pairing object in `provision.py --status`, written before the code.

The device's USB status now carries a "pairing" object (docs/pairing-design.md section 13): whether the hotspot is open,
the phase of the session, and the name of the last reason an attempt ended. It is how a person finds out, without a screen
or a log, what hotspot pairing did. The tool prints only what it can vouch for.

P1  A valid object is printed as it is.
P2  Unknown keys are dropped. The setup code, the hotspot password, the Wi-Fi name, the grant code and the token are
    never part of the object; if a device (or something pretending to be one) sends them, they are not echoed.
P3  Words the tool does not know (phase, reason) are dropped, not passed on; a value of the wrong type is dropped.
P4  "open" must agree with the phase: a closed or finished session is not open, and the other phases are.
P5  A pairing value that is not an object is not printed; an object with nothing valid in it is not printed.
P6  Everything else in the status is cleaned exactly as before.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("provision", TOOLS / "provision.py")
provision = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(provision)

GOOD = {"open": True, "phase": "open", "why": "none"}
PHASES = ["closed", "open", "joining", "redeeming", "save_failed", "done"]
WHYS = ["none", "not_allowed", "random_failed", "wrong_phase", "wrong_code", "too_many_wrong_codes", "code_required", "timeout", "cancelled",
        "invalid_input", "too_many_attempts", "wifi_failed", "agent_unreachable", "certificate_expired", "hostname_mismatch", "chain_invalid",
        "redeem_refused", "bad_reply", "save_failed", "readback_mismatch"]


class PairingStatus(unittest.TestCase):
    def test_p1_a_valid_object_is_printed(self):
        self.assertEqual(provision.safe_pairing(GOOD), GOOD)
        for phase in PHASES:
            for why in WHYS:
                value = {"open": phase in {"open", "joining", "redeeming", "save_failed"}, "phase": phase, "why": why}
                self.assertEqual(provision.safe_pairing(value), value)

    def test_p2_secrets_and_unknown_keys_are_never_echoed(self):
        value = {**GOOD, "setup_code": "123456", "hotspot_password": "abcdefghjkmn", "ssid": "Home", "code": "KLMNPQRSTV", "token": "t" * 40,
                 "nested": {"a": 1}, "long": "x" * 500}
        self.assertEqual(provision.safe_pairing(value), GOOD)
        self.assertNotIn("123456", repr(provision.safe_pairing(value)))

    def test_p3_unknown_words_and_wrong_types_are_dropped(self):
        self.assertEqual(provision.safe_pairing({**GOOD, "phase": "pairing"}), {"open": True, "why": "none"})
        self.assertEqual(provision.safe_pairing({**GOOD, "why": "because"}), {"open": True, "phase": "open"})
        self.assertEqual(provision.safe_pairing({**GOOD, "why": 5}), {"open": True, "phase": "open"})
        self.assertEqual(provision.safe_pairing({**GOOD, "open": "yes"}), {"phase": "open", "why": "none"})
        self.assertEqual(provision.safe_pairing({**GOOD, "open": 1}), {"phase": "open", "why": "none"})
        self.assertEqual(provision.safe_pairing({**GOOD, "phase": ["open"]}), {"open": True, "why": "none"})

    def test_p4_open_agrees_with_the_phase(self):
        for phase in ("closed", "done"):
            self.assertNotIn("open", provision.safe_pairing({"open": True, "phase": phase, "why": "none"}))
        for phase in ("open", "joining", "redeeming", "save_failed"):
            self.assertNotIn("open", provision.safe_pairing({"open": False, "phase": phase, "why": "none"}))

    def test_p5_not_an_object_or_nothing_valid(self):
        for value in (None, "open", 5, [GOOD], True, {}, {"setup_code": "123456"}, {"phase": "nope", "why": "nope", "open": "x"}):
            self.assertIsNone(provision.safe_pairing(value))

    def test_p6_the_rest_of_the_status_is_unchanged(self):
        cleaned = provision.safe_status({"device_id": "deskhub-1", "firmware_version": "0.3.5", "pairing": {**GOOD, "token": "x" * 40}, "secret": "x"})
        self.assertEqual(cleaned["pairing"], GOOD)
        self.assertEqual(cleaned["device_id"], "deskhub-1")
        self.assertNotIn("secret", cleaned)
        self.assertNotIn("pairing", provision.safe_status({"device_id": "deskhub-1", "pairing": "open"}))


if __name__ == "__main__":
    unittest.main()
