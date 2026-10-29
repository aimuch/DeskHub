from __future__ import annotations

import asyncio
from datetime import timedelta
from pathlib import Path
from uuid import uuid4

import pytest
from fastapi.testclient import TestClient

from deskhub_agent import admin_auth
from deskhub_agent.app import create_app
from deskhub_agent.backend import Backend
from deskhub_agent.errors import AgentError
from deskhub_agent.models import Settings, stamp, utcnow
from deskhub_agent.storage import Store


class TestBackend(Backend):
    """Only injected by tests. Production settings have no simulation backend."""
    __test__ = False
    calls = 0
    delay = 0
    observed = "mac-usbc"
    failure = None
    binding_failure = None

    def availability(self):
        return True, None

    async def probe(self):
        return {"availability": "ready", "version": "TEST ONLY", "reason_code": None}

    async def validate_binding(self, display):
        if self.delay:
            await asyncio.sleep(self.delay)
        if self.binding_failure:
            raise AgentError("TARGET_UNAVAILABLE", self.binding_failure, 503)

    async def set_input(self, display, input_id):
        self.calls += 1
        if self.delay:
            await asyncio.sleep(self.delay)
        if self.failure:
            raise AgentError(self.failure, "simulated backend failure", 504)

    async def read_input(self, display):
        if self.observed == "exception":
            raise AgentError("TARGET_UNAVAILABLE", "path lost", 503)
        return self.observed


PASSWORD = "q7Z!mK2#pL9v"


@pytest.fixture
def cheap_hash(monkeypatch):
    """The real password hash needs 128 MiB and a fraction of a second. Tests use a small one; the real cost has its own test."""
    monkeypatch.setattr(admin_auth, "SCRYPT_N", 2 ** 4)


def sign_in(client, admin_token, origin="http://testserver"):
    """Start the way a person starts: the token once, the password at once. Returns the response with the new CSRF token."""
    first = client.post("/api/v1/admin/session", json={"token": admin_token}, headers={"Origin": origin})
    assert first.status_code == 200 and first.json()["setup_required"] is True, first.text
    done = client.put("/api/v1/admin/password", json={"password": PASSWORD},
                      headers={"Origin": origin, "X-CSRF-Token": first.json()["csrf_token"]})
    assert done.status_code == 200, done.text
    return done


@pytest.fixture
def settings(tmp_path):
    return Settings(agent_id="agent-test", db_path=str(tmp_path / "state.sqlite3"),
        displays=[{"monitor_id": "monitor-desk-01", "label": "Desk", "binding": {"uuid": "real-test-uuid", "serial": "A123", "registry_location": "IO/test"},
                   "inputs": {"mac-usbc": 123, "pc-dp": 456}, "readback_enabled": True}],
        actions=[{"action_id": "desktop.switch_to_mac", "label": "Mac", "target_id": "monitor-desk-01", "input_id": "mac-usbc"},
                 {"action_id": "desktop.switch_to_pc", "label": "PC", "target_id": "monitor-desk-01", "input_id": "pc-dp"}])


@pytest.fixture
def harness(settings):
    store = Store(settings.db_path)
    token = store.pair("device-test", ["read:status", "control:displays"], ["monitor-desk-01"])
    adapter = TestBackend(settings)
    app = create_app(settings, store=store, backend=adapter)
    with TestClient(app) as client:
        client.headers["Authorization"] = "Bearer " + token
        yield client, store, adapter, token
    store.close()


def payload(**changes):
    now = utcnow()
    return {"request_id": str(uuid4()), "idempotency_key": str(uuid4()), "action_id": "desktop.switch_to_mac",
            "target_id": "monitor-desk-01", "parameters": {"input_id": "mac-usbc"},
            "sent_at": stamp(now), "expires_at": stamp(now + timedelta(seconds=10)), **changes}


def complete(client, command_id):
    import time
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        response = client.get("/api/v1/commands/" + command_id)
        assert response.status_code == 200, response.text
        if response.json()["state"] not in {"queued", "executing"}:
            return response.json()
        time.sleep(0.01)
    raise AssertionError("command did not finish")
