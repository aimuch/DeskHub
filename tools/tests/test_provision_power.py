"""Failure modes of the power object in `provision.py --status`, written before the code.

The device's USB status now carries a "power" object. The tool prints only what it can vouch for (K3: unknown stays
unknown, nothing invented, nothing echoed that it did not check).

U1  A valid power object is printed as it is.
U2  Unknown keys are dropped, including nested objects and long strings. Nothing the tool did not check is echoed.
U3  A number outside the plausible range, a boolean, a string, a fraction where an integer is required, NaN and
    infinity are dropped, not corrected.
U4  Cross-field rules: a failed measurement keeps no voltage or temperature; no battery keeps no level and no
    battery voltage; no USB keeps no USB voltage.
U5  Words the tool does not know (vbus "weird", measurement "good") are dropped.
U6  A power value that is not an object (list, string, number, null) is not printed.
U7  Everything else in the status is cleaned exactly as before.
U8  End to end: `--status` over a stubbed port prints the checked object and nothing from the unchecked one.
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

GOOD = {"vbus": "present", "battery": "present", "charging": "charging", "fault": False,
        "soc_percent": 67, "battery_mv": 3921, "vbus_mv": 5120, "chip_temp_c": 41.3, "measurement": "ok"}
NUMBERS = ("soc_percent", "battery_mv", "vbus_mv", "chip_temp_c")


def power(**changes):
    value = dict(GOOD)
    value.update(changes)
    return value


def cleaned(value):
    return provision.safe_status({"device_id": "deskhub-aabbccddeeff", "power": value}).get("power")


class SerialStub:
    def __init__(self, response: bytes):
        import io as _io
        self.response = _io.BytesIO(response)
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


class PowerStatusTests(unittest.TestCase):
    def test_valid_object_is_printed_as_it_is(self):
        self.assertEqual(cleaned(power()), GOOD)
        quiet = power(battery="absent", charging="idle", soc_percent=None, battery_mv=None)
        self.assertEqual(cleaned(quiet), quiet)
        failed = power(measurement="adc_read_failed", battery_mv=None, vbus_mv=None, chip_temp_c=None)
        self.assertEqual(cleaned(failed), failed)

    def test_range_edges_are_kept(self):
        for key, value in (("battery_mv", 2500), ("battery_mv", 4600), ("vbus_mv", 3000), ("vbus_mv", 6500),
                           ("chip_temp_c", -40), ("chip_temp_c", 150), ("soc_percent", 0), ("soc_percent", 100)):
            with self.subTest(key=key, value=value):
                self.assertEqual(cleaned(power(**{key: value}))[key], value)

    def test_unknown_keys_and_nested_values_are_dropped(self):
        value = power(battery_temp_c=30, token="secret-token", note="x" * 5000, nested={"a": 1}, extra=[1, 2])
        self.assertEqual(cleaned(value), GOOD)
        self.assertNotIn("secret", json.dumps(cleaned(value)))

    def test_implausible_or_mistyped_numbers_are_dropped_not_corrected(self):
        bad = {"battery_mv": [2499, 4601, 0, -1, 65535, True, "3900", 3900.5, [3900]],
               "vbus_mv": [2999, 6501, 0, -5, 20000, False, "5000", 5000.25, {"v": 1}],
               "chip_temp_c": [-40.1, 150.1, 400, float("nan"), float("inf"), float("-inf"), True, "41", [41]],
               "soc_percent": [-1, 101, 255, 67.5, True, "67", [67]]}
        for key, values in bad.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    result = cleaned(power(**{key: value}))
                    self.assertIsNotNone(result)
                    self.assertNotIn(key, result)  # dropped, not shown as zero and not clamped

    def test_cross_field_rules(self):
        failed = cleaned(power(measurement="adc_enable_failed"))
        for key in ("battery_mv", "vbus_mv", "chip_temp_c"):
            self.assertNotIn(key, failed)
        self.assertEqual(failed["soc_percent"], 67)  # the gauge's level is its own register
        for state in ("adc_read_failed", "adc_out_of_range", "not_measured"):
            result = cleaned(power(measurement=state))
            self.assertTrue(all(key not in result for key in ("battery_mv", "vbus_mv", "chip_temp_c")), state)
        for battery in ("absent", "unknown"):
            result = cleaned(power(battery=battery))
            self.assertNotIn("soc_percent", result)
            self.assertNotIn("battery_mv", result)
            self.assertEqual(result["vbus_mv"], 5120)
        for vbus in ("absent", "unknown"):
            result = cleaned(power(vbus=vbus))
            self.assertNotIn("vbus_mv", result)
            self.assertEqual(result["battery_mv"], 3921)

    def test_unknown_words_are_dropped(self):
        for key, word in (("vbus", "weird"), ("battery", "PRESENT"), ("charging", "fault2"), ("measurement", "good"),
                          ("measurement", ""), ("vbus", None), ("fault", "false"), ("fault", 0)):
            with self.subTest(key=key, word=word):
                self.assertNotIn(key, cleaned(power(**{key: word})))
        # a dropped word also drops what depended on it
        self.assertNotIn("battery_mv", cleaned(power(measurement="good")))

    def test_non_object_power_is_not_printed(self):
        for value in ([], [GOOD], "present", 5, None, True):
            with self.subTest(value=value):
                self.assertNotIn("power", provision.safe_status({"device_id": "deskhub-aabbccddeeff", "power": value}))

    def test_rest_of_the_status_is_cleaned_as_before(self):
        status = {"device_id": "deskhub-aabbccddeeff", "firmware_version": "0.3.0", "theme": "ocean", "rotation": 90, "ui_alive": True,
                  "configured": False, "token": "secret-token", "password": "secret-password", "wifi": "未配置"}
        before = provision.safe_status(status)
        self.assertNotIn("power", before)
        self.assertNotIn("token", before)
        self.assertNotIn("password", before)
        with_power = provision.safe_status({**status, "power": power()})
        self.assertEqual({k: v for k, v in with_power.items() if k != "power"}, before)

    def test_status_command_prints_the_checked_object_only(self):
        unchecked = power(battery_mv=9999, extra="secret-value", vbus="weird")
        status = {"ui_alive": True, "theme": "ocean", "rotation": 90, "configured": False, "power": unchecked}
        port = SerialStub(reply({"device_id": "deskhub-aabbccddeeff", "protocol_version": "1", "firmware_version": "0.3.0"}) + reply(status))
        output = io.StringIO()
        with patch.object(provision, "open_usb", return_value=port), redirect_stdout(output):
            self.assertEqual(provision.main(["--port", "test", "--status"]), 0)
        printed = json.loads(output.getvalue())
        self.assertNotIn("secret", output.getvalue())
        self.assertNotIn("battery_mv", printed["power"])
        self.assertNotIn("vbus", printed["power"])
        self.assertNotIn("vbus_mv", printed["power"])  # its "present" was not a known word, so the voltage goes too
        self.assertEqual(printed["power"]["soc_percent"], 67)
        self.assertEqual(printed["power"]["measurement"], "ok")


if __name__ == "__main__":
    unittest.main()
