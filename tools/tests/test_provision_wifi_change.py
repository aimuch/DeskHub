"""Write-first failures: allow only known diagnostics; never echo SSID or password.
Unknown enum values, booleans used as times, negative times and non-objects are dropped.
"""
import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location("provision", Path(__file__).resolve().parents[1] / "provision.py")
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)

class WifiChangeStatus(unittest.TestCase):
    def test_valid_and_secret_filter(self):
        good = {"phase": "asking", "reason": "AGENT_UNREACHABLE", "remaining_s": 60, "result_ms": 123, "busy": True, "restored": False}
        self.assertEqual(provision.safe_status({"wifi_change": {**good, "ssid": "secret-name", "password": "secret-password"}}), {"wifi_change": good})
    def test_bad_shapes_and_unknown_words(self):
        for value in (None, [], "testing", {}, {"phase": "secret", "reason": "secret", "remaining_s": True, "result_ms": -1, "busy": "yes"}):
            self.assertNotIn("wifi_change", provision.safe_status({"wifi_change": value}))
    def test_other_status_preserved(self):
        self.assertEqual(provision.safe_status({"device_id": "d", "wifi_change": {"phase": "saved"}}), {"device_id": "d", "wifi_change": {"phase": "saved"}})

if __name__ == "__main__":
    unittest.main()
