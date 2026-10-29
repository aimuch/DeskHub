"""Failure modes of the charge object in `provision.py --status`, written before the code.

The device's USB status now carries a "charge" object (docs/charge-protection-design.md): the settings, the state of the
controller, the switch and the last writes to it. The tool prints only what it can vouch for, like the power object.

C1  A valid charge object is printed as it is, with its events.
C2  Unknown keys are dropped, including nested objects and long strings. Nothing the tool did not check is echoed.
C3  Numbers that are not allowed, of the wrong type, NaN or infinite are dropped, not corrected.
C4  Words the tool does not know (state, switch, request, result) are dropped. A reason must be a short upper-case code.
C5  A charge value that is not an object is not printed.
C6  Events: at most the last 16; an event with a bad member is dropped whole, not repaired; a list that is not a list is dropped.
C7  Cross-field rules: protection off cannot be in an "on" state, and on cannot be "off"; an unconfirmed state cannot
    claim to know the switch.
C8  Everything else in the status is cleaned exactly as before.
C9  End to end: `--status` over a stubbed port prints the checked object and nothing from the unchecked one.
"""
from __future__ import annotations

from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("provision", TOOLS / "provision.py")
provision = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(provision)

EVENT = {"at_ms": 123456, "request": "stop", "result": "ok", "reason": "LEVEL_AT_LIMIT"}
GOOD = {"enabled": True, "limit": 80, "resume": 75, "state": "holding", "reason": "LEVEL_AT_LIMIT", "faulted": False,
        "stop_failures": 0, "switch": "stopped", "events": [EVENT]}


def charge(**changes):
    value = dict(GOOD)
    value.update(changes)
    return value


def cleaned(value):
    return provision.safe_status({"device_id": "deskhub-aabbccddeeff", "charge": value}).get("charge")


class SerialStub:
    def __init__(self, response: bytes):
        self.response = io.BytesIO(response)
        self.written = bytearray()

    def write(self, raw):
        self.written.extend(raw)
        return len(raw)

    def flush(self):
        pass

    def read(self, count):
        return self.response.read(count)

    def close(self):
        pass


def reply(result):
    return json.dumps({"ok": True, "result": result}).encode() + b"\n"


class ChargeStatusTests(unittest.TestCase):
    def test_valid_object_is_printed_as_it_is(self):
        self.assertEqual(cleaned(charge()), GOOD)
        off = charge(enabled=False, state="off", reason="", switch="charging", events=[])
        self.assertEqual(cleaned(off), off)
        failsafe = charge(state="failsafe", reason="SAMPLE_STALE", switch="charging", stop_failures=2, faulted=True)
        self.assertEqual(cleaned(failsafe), failsafe)
        unsure = charge(state="unconfirmed", reason="SWITCH_UNCONFIRMED", switch="unknown")
        self.assertEqual(cleaned(unsure), unsure)

    def test_range_edges_are_kept(self):
        for key, value in (("limit", 70), ("limit", 100), ("resume", 1), ("resume", 95), ("stop_failures", 0), ("stop_failures", 99)):
            with self.subTest(key=key, value=value):
                self.assertEqual(cleaned(charge(**{key: value}))[key], value)

    def test_unknown_keys_and_nested_values_are_dropped(self):
        value = charge(token="secret-token", note="x" * 5000, nested={"a": 1}, extra=[1, 2])
        self.assertEqual(cleaned(value), GOOD)
        self.assertNotIn("secret", json.dumps(cleaned(value)))
        event = dict(EVENT, token="secret-token")
        self.assertEqual(cleaned(charge(events=[event]))["events"], [])   # an event with a member we do not know is not repaired

    def test_numbers_that_are_not_allowed_are_dropped_not_corrected(self):
        bad = {"limit": [69, 101, 0, -80, 8000, True, "80", 80.5, [80], float("nan"), float("inf")],
               "resume": [0, -1, 96, 1000, True, "75", 75.5, [75], float("nan")],
               "stop_failures": [-1, 100, True, "0", 1.5, float("nan")]}
        for key, values in bad.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    result = cleaned(charge(**{key: value}))
                    self.assertIsNotNone(result)
                    self.assertNotIn(key, result)

    def test_unknown_words_and_long_reasons_are_dropped(self):
        for key, word in (("state", "HOLDING"), ("state", "weird"), ("state", ""), ("state", None), ("switch", "off"), ("switch", "Stopped"),
                          ("reason", "lower_case"), ("reason", "X" * 41), ("reason", "WITH SPACE"), ("reason", 5), ("reason", "A;B"),
                          ("enabled", "true"), ("enabled", 1), ("faulted", "no"), ("faulted", 0)):
            with self.subTest(key=key, word=word):
                self.assertNotIn(key, cleaned(charge(**{key: word})))
        self.assertEqual(cleaned(charge(reason=""))["reason"], "")
        self.assertEqual(cleaned(charge(reason="X" * 40))["reason"], "X" * 40)

    def test_non_object_charge_is_not_printed(self):
        for value in ([], [GOOD], "holding", 5, None, True):
            with self.subTest(value=value):
                self.assertNotIn("charge", provision.safe_status({"device_id": "deskhub-aabbccddeeff", "charge": value}))

    def test_events_are_checked_one_by_one(self):
        many = [dict(EVENT, at_ms=i) for i in range(40)]
        kept = cleaned(charge(events=many))["events"]
        self.assertEqual([e["at_ms"] for e in kept], list(range(24, 40)))      # the last 16
        bad_events = [dict(EVENT, at_ms=-1), dict(EVENT, at_ms=1.5), dict(EVENT, at_ms=True), dict(EVENT, at_ms="1"),
                      dict(EVENT, request="halt"), dict(EVENT, result="great"), dict(EVENT, reason="lower"), dict(EVENT, reason="X" * 41),
                      {"at_ms": 1, "request": "stop", "result": "ok"}, "stop", 5, None, [EVENT]]
        for event in bad_events:
            with self.subTest(event=event):
                self.assertEqual(cleaned(charge(events=[EVENT, event, EVENT]))["events"], [EVENT, EVENT])
        for events in ("stop", 5, {"a": 1}, None, True):
            with self.subTest(events=events):
                self.assertNotIn("events", cleaned(charge(events=events)))

    def test_cross_field_rules(self):
        for state in ("charging", "holding", "safety_stop", "failsafe", "unconfirmed"):
            self.assertNotIn("state", cleaned(charge(enabled=False, state=state)), state)   # off cannot be in an "on" state
        self.assertNotIn("state", cleaned(charge(enabled=True, state="off")))
        for switch in ("charging", "stopped"):
            result = cleaned(charge(state="unconfirmed", switch=switch))
            self.assertNotIn("switch", result)                                               # an unsure controller does not know the switch
            self.assertEqual(result["state"], "unconfirmed")
        self.assertEqual(cleaned(charge(state="unconfirmed", switch="unknown"))["switch"], "unknown")

    def test_rest_of_the_status_is_cleaned_as_before(self):
        status = {"device_id": "deskhub-aabbccddeeff", "firmware_version": "0.3.1", "theme": "ocean", "rotation": 90, "ui_alive": True,
                  "configured": False, "token": "secret-token", "password": "secret-password", "wifi": "未配置"}
        before = provision.safe_status(status)
        self.assertNotIn("charge", before)
        with_charge = provision.safe_status({**status, "charge": charge()})
        self.assertEqual({k: v for k, v in with_charge.items() if k != "charge"}, before)

    def test_status_command_prints_the_checked_object_only(self):
        unchecked = charge(limit=5000, extra="secret-value", state="weird", events=[dict(EVENT, reason="bad reason")])
        status = {"ui_alive": True, "theme": "ocean", "rotation": 90, "configured": False, "charge": unchecked}
        port = SerialStub(reply({"device_id": "deskhub-aabbccddeeff", "protocol_version": "1", "firmware_version": "0.3.1"}) + reply(status))
        output = io.StringIO()
        with patch.object(provision, "open_usb", return_value=port), redirect_stdout(output):
            self.assertEqual(provision.main(["--port", "test", "--status"]), 0)
        printed = json.loads(output.getvalue())
        self.assertNotIn("secret", output.getvalue())
        self.assertNotIn("limit", printed["charge"])
        self.assertNotIn("state", printed["charge"])
        self.assertEqual(printed["charge"]["events"], [])
        self.assertEqual(printed["charge"]["resume"], 75)


if __name__ == "__main__":
    unittest.main()
