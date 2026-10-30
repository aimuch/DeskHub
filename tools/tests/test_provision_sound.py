"""Failure modes of `provision.py --sound-test`, written before the code.

The sound of the device (docs/design Dev-Sound) cannot be heard by a program. The tool asks the device to play its test
tone and prints what the device says about the call. It never says that the sound was heard: the person who listens says so.

S1  The request is exactly {"op": "sound_test"} after the identity check; nothing else is sent, no credential is read.
S2  A valid answer is printed as it is: available, played, enabled, as booleans.
S3  Unknown keys are dropped; a value that is not a boolean is dropped; a text is never echoed.
S4  An answer that is not an object, or has nothing valid in it, prints nothing from the device and the exit code says failure.
S5  The text under the answer says that the call went through and that hearing it is the person's job. It never says "heard".
S6  A device without a speaker (available false) is reported as such, and the exit code is not success.
S7  The option cannot be mixed with another mode (identify, status, set-time, preferences, pairing files); the USB port is
    not opened in that case.
S8  The status reply still carries the audio capability as before; its reason code stays one the tool knows.
"""
from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
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

IDENTITY = {"device_id": "deskhub-aabbccddeeff", "protocol_version": "1", "firmware_version": "0.3.7"}
GOOD = {"available": True, "played": True, "enabled": False}


class SerialStub:
    def __init__(self, response: bytes):
        self.response = io.BytesIO(response)
        self.written = bytearray()
        self.closed = False

    def write(self, raw):
        self.written.extend(raw)
        return len(raw)

    def flush(self):
        pass

    def read(self, count):
        return self.response.read(count)

    def close(self):
        self.closed = True


def frame(result):
    return json.dumps({"ok": True, "result": result}).encode() + b"\n"


def run(answer, *extra):
    port = SerialStub(frame(IDENTITY) + frame(answer))
    out, err = io.StringIO(), io.StringIO()
    with patch.object(provision, "open_usb", return_value=port), \
         patch("builtins.input") as prompt, patch.object(provision, "private_json") as private, \
         redirect_stdout(out), redirect_stderr(err):
        code = provision.main(["--port", "test", "--sound-test", *extra])
    prompt.assert_not_called()
    private.assert_not_called()
    return code, out.getvalue(), err.getvalue(), [json.loads(line) for line in bytes(port.written).splitlines()], port


class SafeSound(unittest.TestCase):
    def test_s2_a_valid_answer_is_kept(self):
        self.assertEqual(provision.safe_sound(GOOD), GOOD)
        for available in (True, False):
            for played in (True, False):
                for enabled in (True, False):
                    value = {"available": available, "played": played, "enabled": enabled}
                    self.assertEqual(provision.safe_sound(value), value)

    def test_s3_unknown_keys_and_wrong_types_are_dropped(self):
        self.assertEqual(provision.safe_sound({**GOOD, "token": "t" * 40, "ssid": "Home", "nested": {"a": 1}}), GOOD)
        self.assertEqual(provision.safe_sound({**GOOD, "played": "yes"}), {"available": True, "enabled": False})
        self.assertEqual(provision.safe_sound({**GOOD, "enabled": 1}), {"available": True, "played": True})
        self.assertEqual(provision.safe_sound({"available": None, "played": [], "enabled": {}}), None)

    def test_s4_nothing_valid_is_nothing(self):
        for value in (None, [], "ok", 5, {}, {"x": True}):
            self.assertIsNone(provision.safe_sound(value))


class SoundTestCommand(unittest.TestCase):
    def test_s1_only_the_sound_op_is_sent(self):
        code, _, err, requests, port = run(GOOD)
        self.assertEqual(code, 0, err)
        self.assertEqual(requests, [{"op": "identify"}, {"op": "sound_test"}])
        self.assertTrue(port.closed)

    def test_s2_the_answer_is_printed(self):
        _, out, _, _, _ = run(GOOD)
        self.assertEqual(json.loads(out[: out.index("\n}") + 2]), {"sound_test": GOOD})

    def test_s3_nothing_else_of_the_device_is_echoed(self):
        _, out, _, _, _ = run({**GOOD, "token": "T" * 40, "ssid": "HomeNet"})
        self.assertNotIn("T" * 40, out)
        self.assertNotIn("HomeNet", out)

    def test_s4_an_answer_without_content_fails(self):
        code, out, err, _, _ = run({"unknown": 1})
        self.assertNotEqual(code, 0)
        self.assertNotIn("sound_test", out)
        self.assertTrue(err)

    def test_s5_the_text_does_not_say_it_was_heard(self):
        _, out, _, _, _ = run(GOOD)
        self.assertIn("听", out)                      # it tells the person to listen
        self.assertIn("未验证", out)                  # and that nothing is verified by the call
        for claim in ("已听到", "听到了", "声音正常", "已验证"):
            self.assertNotIn(claim, out)

    def test_s6_no_speaker_is_not_success(self):
        code, out, err, _, _ = run({"available": False, "played": False, "enabled": False})
        self.assertNotEqual(code, 0)
        self.assertIn("没有", out + err)
        self.assertNotIn("未验证", out)               # nothing was played, so there is nothing to listen to

    def test_s6_a_tone_that_was_not_played_is_not_success(self):
        code, out, err, _, _ = run({"available": True, "played": False, "enabled": True})
        self.assertNotEqual(code, 0)
        self.assertIn("没有播放", out + err)

    def test_s7_it_cannot_be_mixed_with_another_mode(self):
        for extra in (["--identify"], ["--status"], ["--set-time"], ["--theme", "mint"], ["--rotation", "90"],
                      ["--input", "x.json"], ["--agent-config", "a.json"], ["--url", "https://a.local"], ["--replace"], ["--dry-run"]):
            with self.subTest(extra=extra), patch.object(provision, "open_usb") as usb, \
                    redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                code = provision.main(["--port", "test", "--sound-test", *extra])
                self.assertNotEqual(code, 0)
                usb.assert_not_called()


class StatusKeepsAudio(unittest.TestCase):
    def test_s8_the_audio_capability_is_printed_with_a_known_reason(self):
        caps = {"audio": {"support": "unverified", "availability": "ready", "reason_code": "HARDWARE_TEST_PENDING"}}
        cleaned = provision.safe_status({"device_id": "d", "capabilities": caps})
        self.assertEqual(cleaned["capabilities"]["audio"]["availability"], "ready")
        self.assertEqual(cleaned["capabilities"]["audio"]["support"], "unverified")


if __name__ == "__main__":
    unittest.main()
