import json
import os

from deskhub_agent.cli import main
from deskhub_agent.config import load_settings
from deskhub_agent.storage import Store


def test_cli_private_pair_revoke_diagnostics_and_no_secret_output(tmp_path, capsys):
    config = tmp_path / "agent.json"
    base = ["--config", str(config)]
    assert main([*base, "init"]) == 0
    assert config.stat().st_mode & 0o077 == 0
    assert main([*base, "init"]) == 1
    credential = tmp_path / "credential.json"
    assert main([*base, "pair", "usb-test", "--target", "monitor-desk-01", "--output", str(credential)]) == 0
    token = json.loads(credential.read_text())["token"]
    assert credential.stat().st_mode & 0o077 == 0
    assert main([*base, "diagnostics"]) == 0
    assert main([*base, "list-displays"]) == 0
    output = capsys.readouterr()
    assert token not in output.out and token not in output.err
    assert "BACKEND_NOT_CONFIGURED" in output.out
    store = Store(load_settings(config).db_path)
    assert store.authenticate(token)["device_id"] == "usb-test"
    store.close()
    assert main([*base, "revoke", "usb-test"]) == 0
    assert main([*base, "devices"]) == 0


# ---- the address printed by `serve` -------------------------------------------------------
# Ways this can fail: no /manage/ path (uvicorn prints only the base, which answers 404);
# http for a TLS listener or https for a plain one; an IPv6 host without brackets; a wildcard
# bind printed as a URL host; a wrong port; a secret in the line; printed after the server
# started (the user needs it before the first request, not after the first error).
import pytest
from types import SimpleNamespace

from deskhub_agent.cli import console_url


@pytest.mark.parametrize("bind,port,tls_cert,expected", [
    ("10.10.10.5", 8765, "tls/server.pem", "https://10.10.10.5:8765/manage/"),
    ("127.0.0.1", 8765, None, "http://127.0.0.1:8765/manage/"),
    ("127.0.0.1", 9000, None, "http://127.0.0.1:9000/manage/"),
    ("localhost", 8765, None, "http://localhost:8765/manage/"),
    ("::1", 8765, None, "http://[::1]:8765/manage/"),
    ("fd00::5", 8765, "tls/server.pem", "https://[fd00::5]:8765/manage/"),
    ("0.0.0.0", 8765, "tls/server.pem", "https://localhost:8765/manage/"),
    ("::", 8765, "tls/server.pem", "https://localhost:8765/manage/"),
])
def test_console_url_has_scheme_host_port_and_manage_path(bind, port, tls_cert, expected):
    url = console_url(SimpleNamespace(bind=bind, port=port, tls_cert=tls_cert))
    assert url == expected
    assert "@" not in url and "token" not in url.lower()


def test_serve_prints_the_console_url_before_the_server_starts(tmp_path, capsys, monkeypatch):
    config = tmp_path / "agent.json"
    assert main(["--config", str(config), "init"]) == 0
    capsys.readouterr()
    seen = {}

    def fake_run(app, **options):
        seen["printed_before_start"] = capsys.readouterr().out
        seen["options"] = options

    monkeypatch.setattr("uvicorn.run", fake_run)
    assert main(["--config", str(config), "serve"]) == 0
    assert "http://127.0.0.1:8765/manage/" in seen["printed_before_start"]
    assert "token" not in seen["printed_before_start"].lower()
