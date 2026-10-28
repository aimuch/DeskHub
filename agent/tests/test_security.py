import json
import os
import time

import pytest
from fastapi.testclient import TestClient

from deskhub_agent.app import create_app
from deskhub_agent.models import CommandRequest, Settings
from deskhub_agent.storage import Store
from conftest import TestBackend, complete, payload


def test_requires_auth_for_all_business_endpoints(harness):
    client, _, _, _ = harness
    client.headers.pop("Authorization")
    for path in ("status", "capabilities", "hosts", "displays", "actions", "commands/cmd-missing"):
        assert client.get("/api/v1/" + path).status_code == 401
    assert client.post("/api/v1/commands", json=payload()).status_code == 401
    assert client.get("/healthz").json() == {"alive": True}


def test_permission_resource_and_device_isolation(harness):
    client, store, _, _ = harness
    result = client.post("/api/v1/commands", json=payload()).json()
    complete(client, result["command_id"])
    other = store.pair("other-device", ["read:status", "control:displays"], ["monitor-desk-01"])
    response = client.get("/api/v1/commands/" + result["command_id"], headers={"Authorization": "Bearer " + other})
    assert response.status_code == 404
    readonly = store.pair("read-only", ["read:status"], ["monitor-desk-01"])
    response = client.post("/api/v1/commands", json=payload(), headers={"Authorization": "Bearer " + readonly})
    assert response.status_code == 403
    restricted = store.pair("wrong-target", ["read:status", "control:displays"], ["monitor-other"])
    assert client.get("/api/v1/actions", headers={"Authorization": "Bearer " + restricted}).json()["actions"] == []
    assert client.post("/api/v1/commands", json=payload(), headers={"Authorization": "Bearer " + restricted}).status_code == 403


def test_management_only_pairing_needs_no_display_target(harness):
    client, store, backend, _ = harness
    token = store.pair("management-only", ["read:status", "device:sync"], [])
    headers = {"Authorization": "Bearer " + token}
    assert store.authenticate(token)["targets"] == []
    assert client.get("/api/v1/actions", headers=headers).json()["actions"] == []
    assert client.post("/api/v1/commands", json=payload(), headers=headers).status_code == 403
    assert backend.calls == 0
    with pytest.raises(ValueError):
        store.pair("missing-control-target", ["control:displays", "device:sync"], [])


def test_revoked_token_and_revocation_before_dispatch(harness):
    client, store, backend, _ = harness
    backend.delay = 0.1
    response = client.post("/api/v1/commands", json=payload())
    store.revoke("device-test")
    time.sleep(0.2)
    assert backend.calls == 0
    assert client.get("/api/v1/status").status_code == 401
    with store.lock:
        row = store.connection.execute("SELECT state FROM commands WHERE command_id=?", (response.json()["command_id"],)).fetchone()
    assert row["state"] == "failed"


def test_hash_only_and_private_storage(harness):
    _, store, _, token = harness
    assert os.stat(store.path).st_mode & 0o077 == 0
    assert token.encode() not in store.path.read_bytes()
    with pytest.raises(ValueError):
        store.pair("device-test", ["read:status"], ["*"])


def test_request_and_snapshot_bounds_and_malformed_json(harness):
    client, _, backend, _ = harness
    assert client.post("/api/v1/commands", content=b"x" * 4097, headers={"Content-Type": "application/json"}).status_code == 413
    assert client.post("/api/v1/commands", content="{bad", headers={"Content-Type": "application/json"}).status_code == 400
    assert backend.calls == 0


def test_rate_limit_is_fail_closed(harness):
    client, _, _, _ = harness
    results = [client.get("/api/v1/status").status_code for _ in range(121)]
    assert results[-1] == 429


def test_lan_requires_tls_and_explicit_enabling(settings):
    raw = settings.model_dump()
    for extra in ({"bind": "0.0.0.0"}, {"bind": "0.0.0.0", "lan_enabled": True}):
        with pytest.raises(ValueError):
            Settings.model_validate({**raw, **extra})
    good = Settings.model_validate({**raw, "bind": "0.0.0.0", "lan_enabled": True, "tls_cert": "cert.pem", "tls_key": "key.pem"})
    assert good.lan_enabled


def test_shortcut_conflict_and_unknown_binding_refused(settings):
    raw = settings.model_dump()
    raw["actions"][0]["shortcut"] = "ctrl+alt+m"
    raw["actions"][1]["shortcut"] = "Ctrl+Alt+M"
    with pytest.raises(ValueError):
        Settings.model_validate(raw)
    raw = settings.model_dump()
    raw["displays"][0]["binding"]["serial"] = "0"
    with pytest.raises(ValueError):
        Settings.model_validate(raw)


@pytest.mark.asyncio
async def test_remote_http_rejected_and_network_scope_checked(settings):
    import httpx
    raw = settings.model_dump()
    lan = Settings.model_validate({**raw, "allowed_networks": ["127.0.0.0/8", "192.0.2.0/24"]})
    store = Store(lan.db_path)
    token = store.pair("lan-device", ["read:status"], ["*"])
    app = create_app(lan, store=store, backend=TestBackend(lan))
    transport = httpx.ASGITransport(app=app, client=("192.0.2.4", 4321))
    async with httpx.AsyncClient(transport=transport, base_url="http://192.0.2.1") as client:
        response = await client.get("/api/v1/status", headers={"Authorization": "Bearer " + token})
        assert response.json()["error"]["code"] == "TLS_REQUIRED"
    async with httpx.AsyncClient(transport=transport, base_url="https://192.0.2.1") as client:
        assert (await client.get("/healthz")).status_code == 403
        assert (await client.get("/api/v1/status", headers={"Authorization": "Bearer " + token})).status_code == 200
    denied = httpx.ASGITransport(app=app, client=("198.51.100.2", 4321))
    async with httpx.AsyncClient(transport=denied, base_url="https://192.0.2.1") as client:
        assert (await client.get("/api/v1/status", headers={"Authorization": "Bearer " + token})).status_code == 403
    store.close()
