import json
import os
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from deskhub_agent.app import create_app
from deskhub_agent.backend import Backend
from deskhub_agent.errors import AgentError
from deskhub_agent.metrics import HostMetrics
from deskhub_agent.models import CommandRequest
from deskhub_agent.storage import Store
from conftest import TestBackend, payload


def test_restart_unknown_execution_and_cancelled_queue_without_replay(settings):
    store = Store(settings.db_path)
    token = store.pair("one", ["read:status", "control:displays"], ["*"])
    identity = store.authenticate(token)
    result, _ = store.enqueue(identity, CommandRequest.model_validate(payload()))
    store.update(result["command_id"], state="executing")
    store.close()
    backend = TestBackend(settings)
    app = create_app(settings, backend=backend)
    with TestClient(app) as client:
        client.headers["Authorization"] = "Bearer " + token
        response = client.get("/api/v1/commands/" + result["command_id"])
        assert response.json()["state"] == "unconfirmed"
        assert response.json()["verification"]["reason_code"] == "AGENT_RESTARTED_DURING_EXECUTION"
        assert backend.calls == 0
    # queued actions from a dead process are never picked up after reconnect.
    store = Store(settings.db_path)
    queued, _ = store.enqueue(store.authenticate(token), CommandRequest.model_validate(payload()))
    store.recover()
    assert store.query(store.authenticate(token), command_id=queued["command_id"])["state"] == "cancelled"
    store.close()


def test_single_agent_owns_database(settings):
    first = create_app(settings, backend=TestBackend(settings))
    second = create_app(settings, backend=TestBackend(settings))
    with TestClient(first):
        with pytest.raises(RuntimeError, match="another Agent"):
            with TestClient(second):
                pass


@pytest.mark.asyncio
async def test_backend_exact_identity_refuses_missing_changed_duplicate(settings):
    backend = Backend(settings)
    records = [{"UUID": "real-test-uuid", "serial": "A123", "registryLocation": "IO/test"}]
    async def inventory():
        return records
    backend.inventory = inventory
    await backend.validate_binding(settings.displays[0])
    records[0]["registryLocation"] = "IO/other"
    with pytest.raises(AgentError):
        await backend.validate_binding(settings.displays[0])
    records[0]["registryLocation"] = "IO/test"
    records.append({"UUID": "different-uuid", "serial": "A123", "registryLocation": "IO/second"})
    with pytest.raises(AgentError):
        await backend.validate_binding(settings.displays[0])


@pytest.mark.asyncio
async def test_backend_argv_and_readback_requires_plain_actual_value(settings):
    backend = Backend(settings)
    calls = []
    async def run(*args):
        calls.append(args)
        if "--identifiers" in args:
            return json.dumps([{"UUID": "real-test-uuid", "serial": "A123", "registryLocation": "IO/test"}])
        return "123"
    backend._run = run
    await backend.set_input(settings.displays[0], "mac-usbc")
    assert calls[-1] == ("set", "--UUID=real-test-uuid", "--serial=A123", "--registryLocation=IO/test", "--feature=ddc", "--vcp=0x60", "--value=123")
    assert await backend.read_input(settings.displays[0]) == "mac-usbc"
    async def unrecognized(*args):
        return "sent to display 123, success"
    backend.validate_binding = lambda display: _nothing()
    backend._run = unrecognized
    with pytest.raises(AgentError):
        await backend.read_input(settings.displays[0])


async def _nothing():
    pass


def test_missing_backend_never_returns_simulated_success(settings):
    backend = Backend(settings)
    assert backend.availability() == (False, "BACKEND_NOT_CONFIGURED")
    assert Backend.parse_inventory('{"displays": [{"UUID": "abc", "serial": "1"}]}')[0]["serial"] == "1"
    with pytest.raises(AgentError):
        Backend.parse_inventory("not json")


def test_metrics_truthful_temperature_warmup_units_and_stale():
    metrics = HostMetrics("test-host")
    first = {value["key"]: value for value in metrics.collect()["metrics"]}
    assert first["cpu.utilization"]["value"] is None
    assert first["network.receive_rate"]["value"] is None
    assert first["cpu.temperature"]["value"] is None
    assert first["cpu.temperature"]["quality"] == "unsupported"
    assert first["network.receive_rate"]["unit"] == "byte_per_second"
    assert first["system.uptime"]["unit"] == "duration_seconds"
    metrics.updated_tick -= 7
    assert any(value["quality"] == "stale" for value in metrics.read()["metrics"])


@pytest.mark.asyncio
async def test_real_subprocess_collects_chunks_and_bounds_output(settings, tmp_path):
    import sys
    backend = Backend(settings.model_copy(update={"backend_path": sys.executable}))
    backend.availability = lambda: (True, None)
    script = tmp_path / "chunks.py"
    script.write_text("import sys,time\nfor part in ('first', 'second', 'third'):\n sys.stdout.write(part);sys.stdout.flush();time.sleep(.01)\n")
    assert await backend._run(str(script)) == "firstsecondthird"
    script.write_text("import sys\nsys.stdout.write('x'*70000);sys.stdout.flush()\n")
    with pytest.raises(AgentError) as error:
        await backend._run(str(script))
    assert error.value.code == "BACKEND_OUTPUT_INVALID"
    backend.settings.backend_timeout_seconds = 0.1
    script.write_text("import time\ntime.sleep(1)\n")
    with pytest.raises(AgentError) as error:
        await backend._run(str(script))
    assert error.value.code == "BACKEND_TIMEOUT"


def test_displays_endpoint_serves_snapshot_without_waiting_for_slow_backend(settings):
    import time
    backend = TestBackend(settings)
    backend.delay = 1
    store = Store(settings.db_path)
    token = store.pair("reader", ["read:status"], ["*"])
    app = create_app(settings, store=store, backend=backend)
    with TestClient(app) as client:
        before = time.monotonic()
        response = client.get("/api/v1/displays", headers={"Authorization": "Bearer " + token})
        assert response.status_code == 200
        assert time.monotonic() - before < 0.5
        assert response.json()["displays"][0]["observed"]["quality"] == "unavailable"
    store.close()


def test_future_database_schema_preserved(settings):
    import sqlite3
    store = Store(settings.db_path)
    store.connection.execute("PRAGMA user_version=99")
    store.close()
    with pytest.raises(ValueError, match="newer"):
        Store(settings.db_path)
    connection = sqlite3.connect(settings.db_path)
    assert connection.execute("PRAGMA user_version").fetchone()[0] == 99
    connection.close()
