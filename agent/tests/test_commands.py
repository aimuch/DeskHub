from datetime import timedelta

from deskhub_agent.models import stamp, utcnow
from conftest import complete, payload


def test_idempotency_replay_conflict_and_query(harness):
    client, store, backend, token = harness
    request = payload()
    first = client.post("/api/v1/commands", json=request)
    assert first.status_code == 202, first.text
    command_id = first.json()["command_id"]
    assert complete(client, command_id)["state"] == "succeeded"
    replay = client.post("/api/v1/commands", json=request)
    assert replay.status_code == 202
    assert replay.json()["command_id"] == command_id
    assert backend.calls == 1
    recovered = client.get("/api/v1/commands", params={"idempotency_key": request["idempotency_key"]})
    assert recovered.json()["command_id"] == command_id
    request["parameters"] = {"input_id": "pc-dp"}
    assert client.post("/api/v1/commands", json=request).json()["error"]["code"] == "IDEMPOTENCY_CONFLICT"


def test_nonzero_exit_and_timeout_cannot_be_success(harness):
    client, _, backend, _ = harness
    backend.failure = "BACKEND_TIMEOUT"
    response = client.post("/api/v1/commands", json=payload())
    result = complete(client, response.json()["command_id"])
    assert result["state"] == "unconfirmed"
    assert result["verification"]["reason_code"] == "BACKEND_TIMEOUT"
    assert result["observed"]["input_id"] is None


def test_exit_zero_without_readback_is_unconfirmed(harness):
    client, _, backend, _ = harness
    backend.observed = None
    response = client.post("/api/v1/commands", json=payload())
    result = complete(client, response.json()["command_id"])
    assert result["state"] == "unconfirmed"
    assert result["observed"]["input_id"] is None
    assert backend.calls == 1


def test_lost_control_path_and_mismatched_readback(harness):
    client, _, backend, _ = harness
    backend.observed = "exception"
    response = client.post("/api/v1/commands", json=payload())
    assert complete(client, response.json()["command_id"])["verification"]["reason_code"] == "CONTROL_PATH_LOST_AFTER_SWITCH"
    backend.observed = "pc-dp"
    response = client.post("/api/v1/commands", json=payload())
    result = complete(client, response.json()["command_id"])
    assert result["state"] == "unconfirmed"
    assert result["observed"]["input_id"] == "pc-dp"


def test_serializes_conflicting_actions_with_busy(harness):
    client, _, backend, _ = harness
    backend.delay = 0.1
    first = client.post("/api/v1/commands", json=payload())
    second = client.post("/api/v1/commands", json=payload(action_id="desktop.switch_to_pc", parameters={"input_id": "pc-dp"}))
    assert second.status_code == 409
    assert second.json()["error"]["code"] == "BUSY"
    complete(client, first.json()["command_id"])
    assert backend.calls == 1


def test_expired_in_validation_not_dispatched(harness):
    client, _, backend, _ = harness
    backend.delay = 0.1
    now = utcnow()
    response = client.post("/api/v1/commands", json=payload(sent_at=stamp(now), expires_at=stamp(now + timedelta(milliseconds=30))))
    assert response.status_code == 202
    assert complete(client, response.json()["command_id"])["state"] == "expired"
    assert backend.calls == 0


def test_clock_skew_and_invalid_expiry_rejected(harness):
    client, _, backend, _ = harness
    past = utcnow() - timedelta(seconds=7)
    assert client.post("/api/v1/commands", json=payload(sent_at=stamp(past), expires_at=stamp(past + timedelta(seconds=10)))).json()["error"]["code"] == "CLOCK_SKEW"
    now = utcnow()
    assert client.post("/api/v1/commands", json=payload(sent_at=stamp(now), expires_at=stamp(now + timedelta(seconds=31)))).status_code == 400
    assert client.post("/api/v1/commands", json=payload(sent_at="2026-10-27T00:00:00")).status_code == 400
    assert backend.calls == 0


def test_changed_binding_never_controls_other_monitor(harness):
    client, _, backend, _ = harness
    backend.binding_failure = "DISPLAY_BINDING_CHANGED"
    response = client.post("/api/v1/commands", json=payload())
    result = complete(client, response.json()["command_id"])
    assert result["state"] == "failed"
    assert result["verification"]["result"] == "not_executed"
    assert backend.calls == 0


def test_action_whitelist_rejects_arbitrary_parameters(harness):
    client, _, backend, _ = harness
    response = client.post("/api/v1/commands", json=payload(parameters={"input_id": "mac-usbc", "shell": "echo bad"}))
    assert response.status_code == 400
    assert client.post("/api/v1/commands", json=payload(parameters={"input_id": "pc-dp"})).status_code == 400
    assert client.post("/api/v1/commands", json=payload(action_id="shell.exec")).status_code == 400
    assert backend.calls == 0
