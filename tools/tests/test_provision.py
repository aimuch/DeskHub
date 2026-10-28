from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("provision", TOOLS / "provision.py")
provision = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(provision)


def config():
    return {
        "wifi": {"ssid": "Desk网络", "password": "secret-password"},
        "agent": {
            "url": "https://desk-mac.local:8765", "agent_id": "agent-test",
            "token": "secret-token-" + "x" * 32,
            "ca_pem": (TOOLS / "tests/fixtures/ca.pem").read_text(),
        },
        "time": {"utc": "2026-10-27T03:00:00Z"},
    }


class SerialStub:
    def __init__(self, response: bytes, short_write=False):
        self.response = io.BytesIO(response)
        self.written = bytearray()
        self.short_write = short_write
        self.closed = False

    def write(self, raw):
        self.written.extend(raw)
        return len(raw) - 1 if self.short_write else len(raw)

    def flush(self):
        pass

    def read(self, count):
        return self.response.read(count)

    def close(self):
        self.closed = True


def response(result):
    return json.dumps({"ok": True, "result": result}).encode() + b"\n"


def identity():
    return {"device_id": "deskhub-aabbccddeeff", "protocol_version": "1", "firmware_version": "0.1.0-dev"}


class ConfigTests(unittest.TestCase):
    def test_update_trust_root_must_be_public_p256(self):
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        key = ec.generate_private_key(ec.SECP256R1())
        public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo).decode()
        value = config()
        value['updates'] = {'public_key_pem': public}
        self.assertEqual(provision.validate_config(value)['updates']['public_key_pem'], public)
        value['updates']['public_key_pem'] = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()).decode()
        with self.assertRaises(provision.ProvisionError):
            provision.validate_config(value)
        value['updates'] = {'public_key_pem': public, 'skip_signature': True}
        with self.assertRaises(provision.ProvisionError):
            provision.validate_config(value)

    def test_normalize_valid_config_and_reject_http(self):
        value = config()
        value["agent"]["url"] += "/"
        self.assertFalse(provision.validate_config(value)["agent"]["url"].endswith("/"))
        value["agent"]["url"] = "http://desk-mac.local:8765"
        with self.assertRaises(provision.ProvisionError):
            provision.validate_config(value)

    def test_reject_url_credentials_path_and_query(self):
        for url in (
            "https://secret:password@host", "https://host/api", "https://host?token=secret",
            "https://host#secret", "https://host:bad", "https://host\n", "https:///",
        ):
            with self.subTest(url=url), self.assertRaises(provision.ProvisionError):
                provision.validate_origin(url)

    def test_missing_ca_and_private_key_rejected(self):
        for ca in ("", "garbage", "-----BEGIN PRIVATE KEY-----\nsecret"):
            value = config()
            value["agent"]["ca_pem"] = ca
            with self.assertRaises(provision.ProvisionError):
                provision.validate_config(value)

    def test_wifi_byte_limits_not_codepoint_count(self):
        value = config()
        value["wifi"]["ssid"] = "网" * 11
        with self.assertRaises(provision.ProvisionError):
            provision.validate_config(value)
        value["wifi"]["ssid"] = "network"
        value["wifi"]["password"] = "短"
        with self.assertRaises(provision.ProvisionError):
            provision.validate_config(value)

    def test_missing_token_rejected_except_planning(self):
        value = config()
        del value["agent"]["token"]
        with self.assertRaises(provision.ProvisionError):
            provision.validate_config(value)
        self.assertEqual(provision.validate_config(value, require_token=False)["agent"]["token"], "")

    def test_invalid_time_rejected(self):
        for date in ("1970-01-01T00:00:00Z", "2026-10-27T00:00:00+08:00", "2026-13-27T00:00:00Z"):
            value = config()
            value["time"]["utc"] = date
            with self.assertRaises(provision.ProvisionError):
                provision.validate_config(value)

    def test_lan_ntp_server_preserved_and_invalid_address_rejected(self):
        value = config()
        value["time"]["ntp_server"] = "192.168.1.1"
        self.assertEqual(provision.validate_config(value)["time"]["ntp_server"], "192.168.1.1")
        value["time"]["ntp_server"] = "ntp.home.local"
        self.assertEqual(provision.validate_config(value)["time"]["ntp_server"], "ntp.home.local")
        for server in ("https://ntp.local", "ntp.local:123", "ntp.local/path", "bad_host", "-bad.local", "2001:db8::1", "999.1.1.1"):
            with self.subTest(server=server), self.assertRaises(provision.ProvisionError):
                provision.valid_ntp_server(server)

    def test_private_file_permissions_size_and_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.json"
            path.write_text(json.dumps(config()))
            path.chmod(0o600)
            self.assertEqual(provision.private_json(path), config())
            path.chmod(0o644)
            with self.assertRaises(provision.ProvisionError):
                provision.private_json(path)
            path.chmod(0o600)
            link = Path(directory) / "link.json"
            link.symlink_to(path)
            with self.assertRaises(provision.ProvisionError):
                provision.private_json(link)
            path.write_bytes(b"x" * (provision.MAX_FRAME_BYTES + 1))
            with self.assertRaises(provision.ProvisionError):
                provision.private_json(path)


class UsbTests(unittest.TestCase):
    def test_startup_logs_ignored_and_identify_checked(self):
        port = SerialStub(b"I (0) startup\nnot-json\n{bad}\n" + response(identity()))
        result = provision.identify(provision.UsbTransport(port), "deskhub-aabbccddeeff")
        self.assertEqual(result["device_id"], "deskhub-aabbccddeeff")
        self.assertEqual(json.loads(port.written), {"op": "identify"})

    def test_wrong_device_does_not_write_provision(self):
        port = SerialStub(response(identity()))
        with self.assertRaises(provision.ProvisionError):
            provision.identify(provision.UsbTransport(port), "another-device")
        self.assertNotIn(b"provision", port.written)

    def test_unknown_usb_protocol_rejected(self):
        value = identity()
        value["protocol_version"] = "2"
        with self.assertRaises(provision.ProvisionError):
            provision.identify(provision.UsbTransport(SerialStub(response(value))))

    def test_remote_secret_not_in_error(self):
        remote = {"ok": False, "error": {"code": "TOKEN_SUPER_SECRET", "message": "secret-password secret-token"}}
        with self.assertRaises(provision.ProvisionError) as context:
            provision.UsbTransport(SerialStub(json.dumps(remote).encode() + b"\n")).request({"op": "identify"})
        self.assertNotIn("secret", str(context.exception).lower())

    def test_oversize_response_rejected(self):
        with self.assertRaises(provision.ProvisionError):
            provision.UsbTransport(SerialStub(b"x" * (provision.MAX_FRAME_BYTES + 1))).request({"op": "identify"})

    def test_short_write_rejected(self):
        with self.assertRaises(provision.ProvisionError):
            provision.UsbTransport(SerialStub(b"", short_write=True)).request({"op": "identify"})

    def test_timeout_is_bounded(self):
        with self.assertRaises(provision.ProvisionError):
            provision.UsbTransport(SerialStub(b""), timeout=0.001).request({"op": "identify"})

    def test_big_request_rejected_before_write(self):
        port = SerialStub(b"")
        with self.assertRaises(provision.ProvisionError):
            provision.UsbTransport(port).request({"op": "provision", "secret": "x" * provision.MAX_FRAME_BYTES})
        self.assertFalse(port.written)


class CliTests(unittest.TestCase):
    def test_usb_display_preferences_use_only_requested_fields_without_pairing_or_prompts(self):
        cases = [
            (["--theme", "mint"], {"theme": "mint"}),
            (["--theme", "ocean", "--rotation", "90"], {"theme": "ocean", "rotation": 90}),
            (["--theme", "amber", "--rotation", "180"], {"theme": "amber", "rotation": 180}),
            (["--rotation", "0"], {"rotation": 0}),
            (["--rotation", "270"], {"rotation": 270}),
        ]
        for arguments, preferences in cases:
            with self.subTest(arguments=arguments):
                unpaired = {**identity(), "configured": False}
                port = SerialStub(response(unpaired) + response({"saved": True}))
                output, errors = io.StringIO(), io.StringIO()
                with patch.object(provision, "open_usb", return_value=port), \
                     patch.object(provision, "load_local_agent") as agent, \
                     patch.object(provision, "private_json") as private_input, \
                     patch("builtins.input") as prompt, \
                     patch.object(provision.getpass, "getpass") as password, \
                     redirect_stdout(output), redirect_stderr(errors):
                    result = provision.main(["--port", "test", "--device-id", identity()["device_id"], *arguments])
                self.assertEqual(result, 0, errors.getvalue())
                requests = [json.loads(line) for line in bytes(port.written).splitlines()]
                self.assertEqual(requests, [{"op": "identify"}, {"op": "display_preferences", **preferences}])
                agent.assert_not_called()
                private_input.assert_not_called()
                prompt.assert_not_called()
                password.assert_not_called()
                self.assertTrue(port.closed)
                self.assertEqual(output.getvalue(), "偏好已保存，屏幕应用请确认。\n")

    def test_display_preferences_reject_mixed_modes_before_opening_usb_or_reading_files(self):
        conflicting = [
            ["--identify"], ["--status"], ["--set-time"], ["--dry-run"],
            ["--input", "not-read.json"], ["--agent-config", "not-read.json"],
            ["--ca", "not-read.pem"], ["--url", "https://agent.local"],
            ["--ntp-server", "ntp.local"], ["--update-public-key", "not-read.pem"],
            ["--scope", "device:sync"], ["--target", "monitor-desk-01"], ["--replace"],
        ]
        for arguments in conflicting:
            with self.subTest(arguments=arguments), \
                 patch.object(provision, "open_usb") as usb, \
                 patch.object(provision, "load_local_agent") as agent, \
                 patch.object(provision, "private_json") as private_input, \
                 patch("builtins.input") as prompt, \
                 patch.object(provision.getpass, "getpass") as password, \
                 redirect_stderr(io.StringIO()) as errors:
                result = provision.main(["--port", "test", "--theme", "mint", *arguments])
                self.assertEqual(result, 2)
                self.assertIn("不能与读取、设时或配对参数混用", errors.getvalue())
                usb.assert_not_called()
                agent.assert_not_called()
                private_input.assert_not_called()
                prompt.assert_not_called()
                password.assert_not_called()

    def test_invalid_display_preferences_are_rejected_by_argparse_before_usb(self):
        cases = [["--rotation", value] for value in ("-90", "45", "360", "90.0", "false")]
        cases += [["--theme", value] for value in ("Mint", "unknown")]
        for arguments in cases:
            with self.subTest(arguments=arguments), patch.object(provision, "open_usb") as usb, \
                 redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as result:
                provision.main(["--port", "test", *arguments])
            self.assertEqual(result.exception.code, 2)
            usb.assert_not_called()

    def test_display_preferences_never_claim_saved_without_boolean_true_ack(self):
        for saved in (False, None, 1, "true"):
            with self.subTest(saved=saved):
                port = SerialStub(response(identity()) + response({"saved": saved}))
                output, errors = io.StringIO(), io.StringIO()
                with patch.object(provision, "open_usb", return_value=port), \
                     redirect_stdout(output), redirect_stderr(errors):
                    result = provision.main(["--port", "test", "--rotation", "90"])
                self.assertEqual(result, 2)
                self.assertEqual(output.getvalue(), "")
                self.assertIn("未明确确认偏好已保存", errors.getvalue())
                self.assertTrue(port.closed)

    def test_display_preferences_identity_mismatch_does_not_send_write(self):
        port = SerialStub(response(identity()))
        with patch.object(provision, "open_usb", return_value=port), redirect_stderr(io.StringIO()):
            result = provision.main(["--port", "test", "--theme", "ocean", "--device-id", "wrong-device"])
        self.assertEqual(result, 2)
        self.assertEqual([json.loads(line) for line in bytes(port.written).splitlines()], [{"op": "identify"}])
        self.assertTrue(port.closed)

    def test_read_only_status_preserves_display_diagnostics_but_never_remote_secrets(self):
        status = {"theme": "ocean", "rotation": 90, "ui_alive": True, "configured": False,
                  "token": "secret-token", "password": "secret-password"}
        port = SerialStub(response(identity()) + response(status))
        output = io.StringIO()
        with patch.object(provision, "open_usb", return_value=port), redirect_stdout(output):
            result = provision.main(["--port", "test", "--status"])
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(output.getvalue()), {key: value for key, value in status.items() if key not in {"token", "password"}})
        self.assertEqual([json.loads(line) for line in bytes(port.written).splitlines()], [{"op": "identify"}, {"op": "status"}])
        self.assertNotIn("secret", output.getvalue())

    def test_management_only_pairing_does_not_prompt_for_display_targets(self):
        settings = SimpleNamespace(agent_id="agent-management-only")
        store = Mock()
        store.pair.return_value = "secret-token-" + "x" * 32
        port = SerialStub(response(identity()) + response({"saved": True}))
        output, errors = io.StringIO(), io.StringIO()
        with patch.object(provision, "load_local_agent", return_value=(settings, store)), \
             patch.object(provision, "open_usb", return_value=port), \
             patch("builtins.input", return_value="Desk-network") as prompt, \
             patch.object(provision.getpass, "getpass", return_value="secret-password"), \
             redirect_stdout(output), redirect_stderr(errors):
            result = provision.main([
                "--port", "test", "--agent-config", "test.json", "--url", "https://desk-mac.local:8765",
                "--ca", str(TOOLS / "tests/fixtures/ca.pem"), "--scope", "read:status", "--scope", "device:sync",
            ])
        self.assertEqual(result, 0)
        prompt.assert_called_once_with("Wi-Fi SSID（2.4 GHz）：")
        store.pair.assert_called_once_with("deskhub-aabbccddeeff", ["read:status", "device:sync"], [], replace=False)
        store.revoke.assert_not_called()
        self.assertNotIn("secret", output.getvalue() + errors.getvalue())

    def test_default_control_pairing_rejects_empty_target_before_usb_or_password(self):
        store = Mock()
        with patch.object(provision, "load_local_agent", return_value=(SimpleNamespace(agent_id="agent-test"), store)), \
             patch.object(provision, "open_usb") as open_usb, \
             patch("builtins.input", return_value="") as prompt, \
             patch.object(provision.getpass, "getpass") as password, \
             redirect_stderr(io.StringIO()):
            result = provision.main([
                "--port", "test", "--agent-config", "test.json", "--url", "https://desk-mac.local:8765",
                "--ca", str(TOOLS / "tests/fixtures/ca.pem"),
            ])
        self.assertEqual(result, 2)
        prompt.assert_called_once_with("允许的显示器 ID（逗号分隔，不默认授权全部）：")
        open_usb.assert_not_called()
        password.assert_not_called()
        store.pair.assert_not_called()

    def test_default_control_pairing_still_collects_explicit_target(self):
        store = Mock()
        store.pair.return_value = "secret-token-" + "x" * 32
        port = SerialStub(response(identity()) + response({"saved": True}))
        with patch.object(provision, "load_local_agent", return_value=(SimpleNamespace(agent_id="agent-test"), store)), \
             patch.object(provision, "open_usb", return_value=port), \
             patch("builtins.input", side_effect=["monitor-desk-01", "Desk-network"]) as prompt, \
             patch.object(provision.getpass, "getpass", return_value="secret-password"), \
             redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            result = provision.main([
                "--port", "test", "--agent-config", "test.json", "--url", "https://desk-mac.local:8765",
                "--ca", str(TOOLS / "tests/fixtures/ca.pem"),
            ])
        self.assertEqual(result, 0)
        self.assertEqual(prompt.call_count, 2)
        store.pair.assert_called_once_with("deskhub-aabbccddeeff", ["read:status", "control:displays", "device:sync"], ["monitor-desk-01"], replace=False)

    def test_dry_run_no_usb_no_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.json"
            path.write_text(json.dumps(config()))
            path.chmod(0o600)
            output, errors = io.StringIO(), io.StringIO()
            with patch.object(provision, "open_usb") as open_usb, patch.object(provision, "load_local_agent") as agent:
                with redirect_stdout(output), redirect_stderr(errors):
                    self.assertEqual(provision.main(["--input", str(path), "--dry-run"]), 0)
                open_usb.assert_not_called()
                agent.assert_not_called()
            self.assertIn("仿真", output.getvalue())
            self.assertNotIn("secret", output.getvalue() + errors.getvalue())

    def test_input_bad_certificate_error_no_secret(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.json"
            value = config()
            value["agent"]["ca_pem"] = "secret-token"
            path.write_text(json.dumps(value))
            path.chmod(0o600)
            output = io.StringIO()
            with redirect_stderr(output):
                self.assertEqual(provision.main(["--input", str(path), "--dry-run"]), 2)
            self.assertNotIn("secret", output.getvalue())

    def test_provision_only_reports_persisted_after_ack(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.json"
            path.write_text(json.dumps(config()))
            path.chmod(0o600)
            port = SerialStub(response(identity()) + response({"saved": True}))
            output = io.StringIO()
            with patch.object(provision, "open_usb", return_value=port), redirect_stdout(output):
                self.assertEqual(provision.main(["--input", str(path), "--port", "test"]), 0)
            requests = [json.loads(line) for line in bytes(port.written).splitlines()]
            self.assertEqual([v["op"] for v in requests], ["identify", "provision"])
            self.assertTrue(port.closed)
            self.assertIn("持久化", output.getvalue())
            self.assertNotIn("secret", output.getvalue())

    def test_failed_usb_provision_revokes_new_pair(self):
        """Never retain a newly issued active token after an unacknowledged save."""
        settings = SimpleNamespace(agent_id="agent-test")
        store = Mock()
        store.pair.return_value = "secret-token-" + "x" * 32
        rejected = b'{"ok":false,"error":{"code":"PERSIST_FAILED","message":"secret-password"}}\n'
        port = SerialStub(response(identity()) + rejected)
        output, errors = io.StringIO(), io.StringIO()
        with patch.object(provision, "load_local_agent", return_value=(settings, store)), \
             patch.object(provision, "open_usb", return_value=port), \
             patch("builtins.input", return_value="Desk-network"), \
             patch.object(provision.getpass, "getpass", return_value="secret-password"), \
             redirect_stdout(output), redirect_stderr(errors):
            result = provision.main([
                "--port", "test", "--agent-config", "test.json", "--url", "https://desk-mac.local:8765",
                "--ca", str(TOOLS / "tests/fixtures/ca.pem"), "--target", "monitor-desk-01",
            ])
        self.assertEqual(result, 2)
        store.pair.assert_called_once_with("deskhub-aabbccddeeff", ["read:status", "control:displays", "device:sync"], ["monitor-desk-01"], replace=False)
        store.revoke.assert_called_once_with("deskhub-aabbccddeeff")
        store.close.assert_called_once()
        self.assertTrue(port.closed)
        self.assertNotIn("持久化", output.getvalue())
        self.assertNotIn("secret", output.getvalue() + errors.getvalue())

    def test_unconfirmed_save_is_not_success(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.json"
            path.write_text(json.dumps(config()))
            path.chmod(0o600)
            port = SerialStub(response(identity()) + response({"saved": False}))
            output, errors = io.StringIO(), io.StringIO()
            with patch.object(provision, "open_usb", return_value=port), redirect_stdout(output), redirect_stderr(errors):
                self.assertEqual(provision.main(["--input", str(path), "--port", "test"]), 2)
            self.assertNotIn("持久化", output.getvalue())
            self.assertIn("未明确确认", errors.getvalue())


class AgentPairingIntegrationTests(unittest.TestCase):
    def test_real_management_only_credential_has_no_display_authorization(self):
        try:
            from deskhub_agent.storage import Store
            from deskhub_agent.errors import AgentError
        except ImportError:
            self.skipTest("Install Agent in the Python environment to run integration test")
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "agent.json"
            database = Path(directory) / "state.sqlite3"
            config_path.write_text(json.dumps({"agent_id": "agent-management-only", "db_path": str(database)}))
            port = SerialStub(response(identity()) + response({"saved": True, "config_revision": 1}))
            with patch.object(provision, "open_usb", return_value=port), \
                 patch("builtins.input", return_value="Desk-network") as prompt, \
                 patch.object(provision.getpass, "getpass", return_value="secret-password"), \
                 redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                result = provision.main([
                    "--port", "test", "--agent-config", str(config_path), "--url", "https://desk-mac.local:8765",
                    "--ca", str(TOOLS / "tests/fixtures/ca.pem"), "--scope", "read:status", "--scope", "device:sync",
                ])
            self.assertEqual(result, 0)
            prompt.assert_called_once_with("Wi-Fi SSID（2.4 GHz）：")
            requests = [json.loads(line) for line in bytes(port.written).splitlines()]
            token = requests[1]["config"]["agent"]["token"]
            store = Store(database)
            try:
                paired = store.authenticate(token)
                self.assertEqual(paired["targets"], [])
                self.assertEqual(set(paired["scopes"]), {"read:status", "device:sync"})
                store.authorize(paired, "read:status")
                store.authorize(paired, "device:sync")
                with self.assertRaises(AgentError):
                    store.authorize(paired, "control:displays", "monitor-desk-01")
                self.assertNotIn(token.encode(), database.read_bytes())
            finally:
                store.close()

    def test_usb_pairing_uses_real_hashed_credential_store(self):
        try:
            from deskhub_agent.storage import Store
        except ImportError:
            self.skipTest("Install Agent in the Python environment to run integration test")
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "agent.json"
            database = Path(directory) / "state.sqlite3"
            config_path.write_text(json.dumps({"agent_id": "agent-integration", "db_path": str(database)}))
            port = SerialStub(response(identity()) + response({"saved": True, "config_revision": 1}))
            with patch.object(provision, "open_usb", return_value=port), \
                 patch("builtins.input", return_value="Desk-network"), \
                 patch.object(provision.getpass, "getpass", return_value="secret-password"), \
                 redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                result = provision.main([
                    "--port", "test", "--agent-config", str(config_path), "--url", "https://desk-mac.local:8765",
                    "--ca", str(TOOLS / "tests/fixtures/ca.pem"), "--target", "monitor-desk-01",
                ])
            self.assertEqual(result, 0)
            frame = json.loads(bytes(port.written).splitlines()[1])
            token = frame["config"]["agent"]["token"]
            self.assertEqual(frame["config"]["agent"]["agent_id"], "agent-integration")
            store = Store(database)
            try:
                paired = store.authenticate(token)
                self.assertEqual(paired["device_id"], identity()["device_id"])
                self.assertEqual(paired["targets"], ["monitor-desk-01"])
                self.assertEqual(set(paired["scopes"]), {"read:status", "control:displays", "device:sync"})
            finally:
                store.close()
            self.assertNotIn(token.encode(), database.read_bytes())

    def test_unacknowledged_save_leaves_real_store_credential_revoked(self):
        try:
            from deskhub_agent.errors import AgentError
            from deskhub_agent.storage import Store
        except ImportError:
            self.skipTest("Install Agent in the Python environment to run integration test")
        with tempfile.TemporaryDirectory() as directory:
            config_path = Path(directory) / "agent.json"
            database = Path(directory) / "state.sqlite3"
            config_path.write_text(json.dumps({"agent_id": "agent-integration", "db_path": str(database)}))
            port = SerialStub(response(identity()) + response({"saved": False}))
            with patch.object(provision, "open_usb", return_value=port), \
                 patch("builtins.input", return_value="Desk-network"), \
                 patch.object(provision.getpass, "getpass", return_value="secret-password"), \
                 redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                result = provision.main([
                    "--port", "test", "--agent-config", str(config_path), "--url", "https://desk-mac.local:8765",
                    "--ca", str(TOOLS / "tests/fixtures/ca.pem"), "--target", "monitor-desk-01",
                ])
            self.assertEqual(result, 2)
            token = json.loads(bytes(port.written).splitlines()[1])["config"]["agent"]["token"]
            store = Store(database)
            try:
                with self.assertRaises(AgentError) as context:
                    store.authenticate(token)
                self.assertEqual(context.exception.code, "UNAUTHORIZED")
            finally:
                store.close()


if __name__ == "__main__":
    unittest.main()
