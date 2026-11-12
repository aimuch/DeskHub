from __future__ import annotations

import base64
from copy import deepcopy
from datetime import timedelta
import hashlib
from io import BytesIO
import json
import struct
import zipfile

import pytest
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from fastapi.testclient import TestClient
from pydantic import ValidationError

from deskhub_agent.app import create_app
from deskhub_agent.management_models import DeviceConfiguration, DeviceReport
from deskhub_agent.models import stamp, utcnow
from deskhub_agent.storage import Store
from conftest import PASSWORD

ORIGIN = "http://testserver"


def signed_package(key, *, version="0.2.0-dev", image=None, payload_changes=None, extra=None):
    if image is None:
        image = bytearray(512)
        image[0] = 0xE9
        struct.pack_into("<H", image, 12, 9)
        struct.pack_into("<I", image, 32, 0xABCD5432)
        image[48:48 + len(version)] = version.encode()
        image[80:87] = b"deskhub"
        image = bytes(image)
    payload = {"schema_version": 1, "board_model": "ESP32-S3-Touch-AMOLED-2.16", "chip": "esp32s3",
               "version": version, "config_schema_min": 1, "config_schema_max": 1,
               "image_size": len(image), "image_sha256": hashlib.sha256(image).hexdigest()}
    payload.update(payload_changes or {})
    raw = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    envelope = {"algorithm": "ecdsa-p256-sha256", "payload": base64.b64encode(raw).decode(),
                "signature": base64.b64encode(key.sign(raw, ec.ECDSA(hashes.SHA256()))).decode()}
    result = BytesIO()
    with zipfile.ZipFile(result, "w") as archive:
        archive.writestr("manifest.json", json.dumps(envelope))
        archive.writestr("app.bin", image)
        if extra:
            archive.writestr(extra, "not permitted")
    return result.getvalue(), payload, image


@pytest.fixture
def management(settings, tmp_path, cheap_hash):
    key = ec.generate_private_key(ec.SECP256R1())
    public = tmp_path / "public.pem"
    public.write_bytes(key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    settings.firmware_public_key = str(public)
    store = Store(settings.db_path)
    admin_token = store.pair("browser-admin", ["admin:config"], ["*"])
    device_token = store.pair("terminal-one", ["read:status", "control:displays", "device:sync"], ["monitor-desk-01"])
    app = create_app(settings, store=store)
    with TestClient(app) as client:
        # The way a person starts: the token once, the password at once, then only the password.
        result = client.post("/api/v1/admin/session", json={"token": admin_token}, headers={"Origin": ORIGIN})
        assert result.status_code == 200 and result.json()["setup_required"] is True, result.text
        result = client.put("/api/v1/admin/password", json={"password": PASSWORD},
                            headers={"Origin": ORIGIN, "X-CSRF-Token": result.json()["csrf_token"]})
        assert result.status_code == 200, result.text
        csrf = result.json()["csrf_token"]
        headers = {"Origin": ORIGIN, "X-CSRF-Token": csrf}
        from deskhub_agent.firmware_packages import key_fingerprint
        report = {"firmware_version": "0.1.0-dev", "board_model": "ESP32-S3-Touch-AMOLED-2.16", "boot_id": "boot-one",
                  "uptime_seconds": 10, "free_heap_bytes": 100000, "applied_revision": 0,
                  "configuration": DeviceConfiguration().model_dump(), "config_error": None,
                  "power": {"vbus": "present", "battery": "absent", "charging": "idle", "fault": False},
                  "capabilities": {"management": True, "signed_ota": True, "layouts": ["desk", "clock", "focus"],
                                   "update_key_fingerprint": key_fingerprint(key.public_key())}, "update": None}
        yield client, app, store, key, admin_token, device_token, headers, report
    store.close()


def sync(client, token, report):
    return client.post("/api/v1/device/sync", json=report, headers={"Authorization": "Bearer " + token})


def upload(client, key, headers, **kwargs):
    package, payload, image = signed_package(key, **kwargs)
    response = client.post("/api/v1/admin/firmware", content=package, headers={**headers, "Content-Type": "application/zip"})
    return response, payload, image


def test_admin_cookie_csrf_origin_revocation_and_identity_separation(management):
    client, app, store, key, admin_token, device_token, headers, report = management
    session = client.get("/api/v1/admin/session")
    assert session.status_code == 200
    assert "csrf_token" in session.json() and "token" not in session.json()
    assert client.post("/api/v1/admin/devices/terminal-one/revoke").status_code == 403
    assert client.post("/api/v1/admin/devices/terminal-one/revoke", headers={**headers, "Origin": "https://attacker.invalid"}).status_code == 403
    assert client.post("/api/v1/admin/session", json={"token": admin_token}, headers={"Origin": "https://attacker.invalid"}).status_code == 403
    assert client.post("/api/v1/admin/session", json={"token": device_token}, headers={"Origin": ORIGIN}).status_code == 403
    privileged = store.pair("combined-admin", ["admin:config", "device:sync"], ["*"])
    assert sync(client, privileged, report).status_code == 403
    assert client.get("/api/v1/admin/devices/browser-admin").status_code == 404
    script = TestClient(app)   # a command line: the token, no Origin header
    assert script.post("/api/v1/admin/session", json={"token": admin_token}).status_code == 200
    store.revoke("browser-admin")
    assert script.get("/api/v1/admin/session").status_code == 401   # a token session ends with its token
    assert client.get("/api/v1/admin/session").status_code == 200   # the password session belongs to the account, not to the token


def test_cookie_flags_session_expiry_and_logout(management):
    client, app, store, key, admin_token, device_token, headers, report = management
    assert client.cookies.get("deskhub_session")
    result = client.post("/api/v1/admin/login", json={"password": PASSWORD}, headers={"Origin": ORIGIN})
    cookie = result.headers["set-cookie"]
    assert "HttpOnly" in cookie and "SameSite=strict" in cookie and "Max-Age=1800" in cookie
    newest = result.json()["csrf_token"]
    assert client.delete("/api/v1/admin/session", headers={**headers, "X-CSRF-Token": newest}).status_code == 200
    assert client.get("/api/v1/admin/session").status_code == 401
    result = client.post("/api/v1/admin/login", json={"password": PASSWORD}, headers={"Origin": ORIGIN})
    for value in app.state.management.sessions.values():
        value["deadline"] -= 1801
    assert client.get("/api/v1/admin/session").status_code == 401


def test_revision_only_applied_after_matching_persistent_device_ack(management):
    client, app, store, key, admin_token, token, headers, report = management
    assert sync(client, token, report).status_code == 200
    record = client.get("/api/v1/admin/devices/terminal-one").json()
    assert record["online"] and record["applied_revision"] == 0
    configuration = deepcopy(report["configuration"])
    configuration["layout"]["preset"] = "focus"
    configuration["layout"]["action_ids"] = ["desktop.switch_to_pc", "desktop.switch_to_mac"]
    body = {"expected_revision": 0, "configuration": configuration}
    result = client.put("/api/v1/admin/devices/terminal-one/configuration", json=body, headers=headers)
    assert result.status_code == 200, result.text
    assert result.json()["configuration_state"] == "pending" and result.json()["applied_revision"] == 0
    assert client.put("/api/v1/admin/devices/terminal-one/configuration", json=body, headers=headers).status_code == 409
    desired = sync(client, token, report).json()["desired"]
    assert desired == {"revision": 1, "configuration": configuration}
    report["applied_revision"] = 1
    report["configuration"] = configuration
    assert sync(client, token, report).json()["desired"] is None
    assert client.get("/api/v1/admin/devices/terminal-one").json()["configuration_state"] == "applied"
    # Local changes must not be presented as still matching the remote draft.
    report["configuration"]["display"]["brightness"] = 31
    sync(client, token, report)
    assert client.get("/api/v1/admin/devices/terminal-one").json()["config_error"] == "CONFIGURATION_DIFFERS"


def test_layout_permissions_strict_values_and_illegal_thresholds(management):
    client, app, store, key, admin_token, token, headers, report = management
    body = {"expected_revision": 0, "configuration": deepcopy(report["configuration"])}
    body["configuration"]["layout"]["action_ids"] = ["unregistered.action"]
    assert client.put("/api/v1/admin/devices/terminal-one/configuration", json=body, headers=headers).status_code == 403
    body["configuration"]["layout"]["action_ids"] = []
    for key, value in (("brightness", 0), ("brightness", True), ("dim_s", 30), ("night_enabled", "true")):
        invalid = deepcopy(body)
        invalid["configuration"]["display"][key] = value
        assert client.put("/api/v1/admin/devices/terminal-one/configuration", json=invalid, headers=headers).status_code == 400
    for invalid in (True, 1.0, "1"):
        with pytest.raises(ValidationError):
            DeviceConfiguration.model_validate({"schema_version": invalid})


@pytest.mark.parametrize("theme,rotation", [("mint", 0), ("ocean", 90), ("amber", 180), ("mint", 270)])
def test_theme_and_quarter_turn_configuration_requires_device_ack(management, theme, rotation):
    client, app, store, key, admin_token, token, headers, report = management
    report["firmware_version"] = "0.2.0-dev"
    assert sync(client, token, report).status_code == 200
    configuration = deepcopy(report["configuration"])
    configuration["layout"]["theme"] = theme
    configuration["display"]["rotation"] = rotation
    result = client.put("/api/v1/admin/devices/terminal-one/configuration", json={
        "expected_revision": 0, "configuration": configuration,
    }, headers=headers)
    assert result.status_code == 200, result.text
    assert result.json()["configuration_state"] == "pending"
    desired = sync(client, token, report).json()["desired"]
    assert desired == {"revision": 1, "configuration": configuration}
    report["configuration"] = configuration
    # Matching values alone cannot acknowledge an unapplied revision.
    assert sync(client, token, report).json()["desired"] == desired
    assert client.get("/api/v1/admin/devices/terminal-one").json()["configuration_state"] == "pending"
    report["applied_revision"] = 1
    assert sync(client, token, report).json()["desired"] is None
    assert client.get("/api/v1/admin/devices/terminal-one").json()["configuration_state"] == "applied"
    saved = json.loads(store.connection.execute(
        "SELECT configuration_json FROM device_configurations WHERE device_id='terminal-one'",
    ).fetchone()[0])
    assert saved["layout"]["theme"] == theme and saved["display"]["rotation"] == rotation


@pytest.mark.parametrize("section,field,value", [
    ("layout", "theme", "Mint"), ("layout", "theme", "unknown"),
    ("layout", "theme", 0), ("layout", "theme", None),
    ("display", "rotation", -90), ("display", "rotation", 45),
    ("display", "rotation", 360), ("display", "rotation", "90"),
    ("display", "rotation", 90.0), ("display", "rotation", True),
    ("display", "rotation", False), ("display", "rotation", None),
])
def test_invalid_theme_or_rotation_never_changes_saved_draft_or_report(management, section, field, value):
    client, app, store, key, admin_token, token, headers, report = management
    report["firmware_version"] = "0.2.0-dev"
    assert sync(client, token, report).status_code == 200
    invalid = deepcopy(report)
    invalid["configuration"][section][field] = value
    response = client.put("/api/v1/admin/devices/terminal-one/configuration", json={
        "expected_revision": 0, "configuration": invalid["configuration"],
    }, headers=headers)
    assert response.status_code == 400, response.text
    assert sync(client, token, invalid).status_code == 400
    record = client.get("/api/v1/admin/devices/terminal-one").json()
    assert record["desired_revision"] == 0 and record["desired_configuration"] is None
    assert record["reported_configuration"] == report["configuration"]


def test_theme_rotation_and_ack_are_isolated_between_devices(management):
    client, app, store, key, admin_token, token, headers, report = management
    report["firmware_version"] = "0.2.0-dev"
    other_token = store.pair("terminal-two", ["device:sync"], [])
    other_report = deepcopy(report)
    other_report["boot_id"] = "boot-two"
    assert sync(client, token, report).status_code == 200
    assert sync(client, other_token, other_report).status_code == 200
    first, second = deepcopy(report["configuration"]), deepcopy(report["configuration"])
    first["layout"]["theme"], first["display"]["rotation"] = "ocean", 90
    second["layout"]["theme"], second["display"]["rotation"] = "amber", 270
    for device, configuration in (("terminal-one", first), ("terminal-two", second)):
        result = client.put(f"/api/v1/admin/devices/{device}/configuration", json={
            "expected_revision": 0, "configuration": configuration,
        }, headers=headers)
        assert result.status_code == 200, result.text
    assert sync(client, token, report).json()["desired"]["configuration"] == first
    assert sync(client, other_token, other_report).json()["desired"]["configuration"] == second
    report.update(applied_revision=1, configuration=first)
    assert sync(client, token, report).status_code == 200
    assert client.get("/api/v1/admin/devices/terminal-one").json()["configuration_state"] == "applied"
    assert client.get("/api/v1/admin/devices/terminal-two").json()["configuration_state"] == "pending"
    # Equal revision numbers on different devices do not make their values equal.
    report["configuration"] = second
    assert sync(client, token, report).status_code == 200
    record = client.get("/api/v1/admin/devices/terminal-one").json()
    assert record["configuration_state"] == "failed" and record["config_error"] == "CONFIGURATION_DIFFERS"
    assert client.get("/api/v1/admin/devices/terminal-two").json()["configuration_state"] == "pending"
    other_report.update(applied_revision=1, configuration=second)
    assert sync(client, other_token, other_report).json()["desired"] is None
    assert client.get("/api/v1/admin/devices/terminal-two").json()["configuration_state"] == "applied"


def test_legacy_schema_one_defaults_survive_database_reopen_without_false_ack_failure(management):
    client, app, store, key, admin_token, token, headers, report = management
    from deskhub_agent.management import Management
    legacy_report = deepcopy(report)
    legacy_report["firmware_version"] = "0.2.0-dev"
    legacy_report["applied_revision"] = 3
    legacy_report["configuration"]["layout"].pop("theme")
    legacy_report["configuration"]["display"].pop("rotation")
    legacy_configuration = legacy_report["configuration"]
    with store.lock:
        store.connection.execute("INSERT INTO device_reports VALUES(?,?,?)", (
            "terminal-one", json.dumps(legacy_report), stamp(),
        ))
        store.connection.execute("INSERT INTO device_configurations VALUES(?,?,?,?)", (
            "terminal-one", 3, json.dumps(legacy_configuration), stamp(),
        ))
    reopened = Store(store.path)
    try:
        restarted = Management(app.state.management.settings, reopened)
        record = restarted.device("terminal-one")
        assert not record["online"] and record["configuration_state"] == "applied"
        assert record["reported_configuration"] == record["desired_configuration"]
        assert record["desired_configuration"]["layout"]["theme"] == "mint"
        assert record["desired_configuration"]["display"]["rotation"] == 0
        legacy_report["applied_revision"] = 2
        result = restarted.sync(reopened.authenticate(token), DeviceReport.model_validate(legacy_report))
        assert result["desired"] == {"revision": 3, "configuration": record["desired_configuration"]}
        assert restarted.device("terminal-one")["configuration_state"] == "pending"
        legacy_report["applied_revision"] = 3
        assert restarted.sync(reopened.authenticate(token), DeviceReport.model_validate(legacy_report))["desired"] is None
        assert restarted.device("terminal-one")["configuration_state"] == "applied"
    finally:
        reopened.close()


@pytest.mark.parametrize("case", ["wrong-signature", "wrong-board", "wrong-chip", "wrong-app", "wrong-version", "wrong-hash", "extra-path", "schema-bool", "oversize"])
def test_signed_package_rejects_invalid_inputs_before_storage(management, case):
    client, app, store, key, admin_token, token, headers, report = management
    kwargs = {}
    if case == "wrong-signature":
        key = ec.generate_private_key(ec.SECP256R1())
    elif case == "wrong-board":
        kwargs["payload_changes"] = {"board_model": "some-other-board"}
    elif case == "wrong-hash":
        kwargs["payload_changes"] = {"image_sha256": "0" * 64}
    elif case == "schema-bool":
        kwargs["payload_changes"] = {"schema_version": True}
    elif case == "oversize":
        kwargs["payload_changes"] = {"image_size": 6 * 1024 * 1024 + 1}
    elif case == "extra-path":
        kwargs["extra"] = "../outside.bin"
    else:
        _, _, image = signed_package(key)
        image = bytearray(image)
        if case == "wrong-chip":
            struct.pack_into("<H", image, 12, 0)
        elif case == "wrong-app":
            image[80:87] = b"notdesk"
        elif case == "wrong-version":
            image[48:59] = b"0.9.0-other"
        kwargs["image"] = bytes(image)
    result, payload, image = upload(client, key, headers, **kwargs)
    assert result.status_code == 400, (case, result.text)
    assert client.get("/api/v1/admin/firmware").json()["packages"] == []
    assert not app.state.management.packages_dir.exists()


def test_upgrade_requires_online_power_trust_and_device_confirmation(management):
    client, app, store, key, admin_token, token, headers, report = management
    result, payload, image = upload(client, key, headers)
    assert result.status_code == 201, result.text
    package_id = result.json()["package_id"]
    assert package_id == hashlib.sha256(image).hexdigest()
    assert result.json()["signature_verified"] is True
    body = {"package_id": package_id, "expected_firmware_version": "0.1.0-dev"}
    path = "/api/v1/admin/devices/terminal-one/updates"
    assert client.post(path, json=body, headers=headers).status_code == 409
    report["power"]["vbus"] = "unknown"
    sync(client, token, report)
    assert client.post(path, json=body, headers=headers).json()["error"]["code"] == "UNSAFE_UPDATE_POWER"
    report["power"]["vbus"] = "present"
    report["capabilities"]["update_key_fingerprint"] = "0" * 64
    sync(client, token, report)
    assert client.post(path, json=body, headers=headers).json()["error"]["code"] == "UPDATE_TRUST_MISMATCH"
    from deskhub_agent.firmware_packages import key_fingerprint
    report["capabilities"]["update_key_fingerprint"] = key_fingerprint(key.public_key())
    sync(client, token, report)
    result = client.post(path, json=body, headers=headers)
    assert result.status_code == 202, result.text
    job = result.json()
    assert job["state"] == "queued"
    assert client.post(path, json=body, headers=headers).json()["error"]["code"] == "BUSY"
    assigned = sync(client, token, report).json()["update"]
    assert assigned["job_id"] == job["job_id"] and assigned["image_path"].endswith(package_id + "/image")
    other = store.pair("terminal-two", ["device:sync"], ["*"])
    assert client.get(assigned["image_path"], headers={"Authorization": "Bearer " + other}).status_code == 403
    image_response = client.get(assigned["image_path"], headers={"Authorization": "Bearer " + token})
    assert image_response.content == image and int(image_response.headers["content-length"]) == len(image)
    assert client.get("/api/v1/admin/devices/terminal-one").json()["update"]["state"] == "downloading"
    report["update"] = {"job_id": job["job_id"], "state": "succeeded", "progress_percent": 100, "error_code": None}
    assert sync(client, token, report).status_code == 409
    report["firmware_version"] = "0.2.0-dev"
    assert sync(client, token, report).status_code == 200
    completed = client.get("/api/v1/admin/devices/terminal-one").json()["update"]
    assert completed["state"] == "succeeded" and completed["completed_at"]
    assert client.get(assigned["image_path"], headers={"Authorization": "Bearer " + token}).status_code == 403


def test_expired_job_and_credential_revocation_prevent_late_transport(management):
    client, app, store, key, admin_token, token, headers, report = management
    sync(client, token, report)
    result, payload, image = upload(client, key, headers)
    body = {"package_id": result.json()["package_id"], "expected_firmware_version": "0.1.0-dev"}
    job = client.post("/api/v1/admin/devices/terminal-one/updates", json=body, headers=headers).json()
    store.connection.execute("UPDATE update_jobs SET expires_at=? WHERE job_id=?", (stamp(utcnow() - timedelta(seconds=1)), job["job_id"]))
    expired = client.get('/api/v1/admin/devices/terminal-one').json()['update']
    assert expired['state'] == 'failed' and expired['error_code'] == 'UPDATE_EXPIRED'
    report["update"] = {"job_id": job["job_id"], "state": "downloading", "progress_percent": 1}
    assert sync(client, token, report).json()["update"] is None
    assert client.get("/api/v1/admin/devices/terminal-one").json()["update"]["error_code"] == "UPDATE_EXPIRED"
    report["update"] = None
    client.post("/api/v1/admin/devices/terminal-one/updates", json=body, headers=headers)
    store.revoke("terminal-one")
    assert sync(client, token, report).status_code == 401
    assert client.get("/api/v1/admin/devices/terminal-one").json()["update"]["state"] == "failed"


def test_post_write_verification_is_visible_without_rewinding_to_queued(management):
    client, app, store, key, admin_token, token, headers, report = management
    sync(client, token, report)
    uploaded, _, _ = upload(client, key, headers)
    created = client.post('/api/v1/admin/devices/terminal-one/updates', json={
        'package_id': uploaded.json()['package_id'], 'expected_firmware_version': report['firmware_version'],
    }, headers=headers).json()
    for state, progress in [('writing', 90), ('verifying', 99), ('queued', 0)]:
        report['update'] = {'job_id': created['job_id'], 'state': state, 'progress_percent': progress}
        assert sync(client, token, report).status_code == 200
    current = client.get('/api/v1/admin/devices/terminal-one').json()['update']
    assert current['state'] == 'verifying' and current['progress_percent'] == 99


def test_unknown_terminal_journal_does_not_lock_out_restored_agent(management):
    client, app, store, key, admin_token, token, headers, report = management
    report['update'] = {'job_id': 'upd-old-device-journal', 'state': 'succeeded', 'progress_percent': 100}
    result = sync(client, token, report)
    assert result.status_code == 200 and result.json()['update'] is None
    current = client.get('/api/v1/admin/devices/terminal-one').json()
    assert current['online'] is True and current['update'] is None
    assert store.connection.execute('SELECT count(*) FROM update_jobs').fetchone()[0] == 0
    report['update']['state'] = 'writing'
    assert sync(client, token, report).status_code == 404


def test_foreign_terminal_job_is_never_accepted_as_unknown_history(management):
    client, app, store, key, admin_token, token, headers, report = management
    sync(client, token, report)
    uploaded, _, _ = upload(client, key, headers)
    job = client.post('/api/v1/admin/devices/terminal-one/updates', json={
        'package_id': uploaded.json()['package_id'], 'expected_firmware_version': report['firmware_version'],
    }, headers=headers).json()
    other = store.pair('terminal-two', ['device:sync'], ['*'])
    report['update'] = {'job_id': job['job_id'], 'state': 'succeeded', 'progress_percent': 100}
    assert sync(client, other, report).status_code == 404


def test_reports_persist_but_restart_never_fakes_online(management):
    client, app, store, key, admin_token, token, headers, report = management
    assert sync(client, token, report).status_code == 200
    body = {"expected_revision": 0, "configuration": report["configuration"]}
    assert client.put("/api/v1/admin/devices/terminal-one/configuration", json=body, headers=headers).status_code == 200
    from deskhub_agent.management import Management
    restarted = Management(app.state.management.settings, store)
    record = restarted.device("terminal-one")
    assert record["last_seen"] and not record["online"] and record["desired_revision"] == 1


def test_overview_counts_all_devices_without_leaking_admins(management):
    client, app, store, key, admin_token, token, headers, report = management
    for index in range(12):
        store.pair(f"extra-{index}", ["device:sync"], ["*"])
    result = client.get("/api/v1/admin/overview").json()
    assert result["device_count"] == 13 and result["online_count"] == 0
    listing = client.get("/api/v1/admin/devices").json()
    assert len(listing["devices"]) <= 8 and listing["next_offset"] is not None
    assert all(row["device_id"] != "browser-admin" for row in listing["devices"])


def test_upload_gate_large_stream_and_ordinary_body_budget(management):
    client, app, store, key, admin_token, token, headers, report = management
    unsigned = b"x" * 5000
    assert client.post("/api/v1/admin/firmware", content=unsigned, headers={"Content-Type": "application/zip"}).status_code == 403
    assert client.post("/api/v1/admin/firmware", content=b"x" * (7 * 1024 * 1024 + 1), headers={**headers, "Content-Type": "application/zip"}).status_code == 413
    assert client.post("/api/v1/device/sync", content=unsigned, headers={"Authorization": "Bearer " + token}).status_code == 413
    _, _, image = signed_package(key)
    image += bytes(20000)
    uploaded, payload, actual = upload(client, key, headers, image=image)
    assert uploaded.status_code == 201
    assert sync(client, token, report).status_code == 200
    job = client.post("/api/v1/admin/devices/terminal-one/updates", json={"package_id": payload["image_sha256"], "expected_firmware_version": "0.1.0-dev"}, headers=headers)
    assert job.status_code == 202
    response = client.get(f"/api/v1/device/firmware/{payload['image_sha256']}/image", headers={"Authorization": "Bearer " + token})
    assert response.status_code == 200 and len(response.content) > 16384 and response.content == image


def test_rotation_invalidates_token_session_and_https_cookie_is_secure(management):
    client, app, store, key, admin_token, token, headers, report = management
    import httpx
    client.base_url = httpx.URL("https://testserver")
    response = client.post("/api/v1/admin/login", json={"password": PASSWORD}, headers={"Origin": "https://testserver"})
    assert response.status_code == 200 and "; Secure" in response.headers["set-cookie"]
    script = TestClient(app)   # a command line: the token, no Origin header
    assert script.post("/api/v1/admin/session", json={"token": admin_token}).status_code == 200
    store.pair("browser-admin", ["admin:config"], ["*"], replace=True)
    assert script.get("/api/v1/admin/session").status_code == 401   # a token session ends when its token is replaced
    assert client.get("/api/v1/admin/session").status_code == 200   # the password session does not depend on the token


def test_cli_admin_credential_is_private_separate_and_never_printed(tmp_path, capsys):
    from deskhub_agent.cli import main
    from deskhub_agent.config import load_settings
    config = tmp_path / "agent.json"
    assert main(["--config", str(config), "init"]) == 0
    output = tmp_path / "admin.json"
    assert main(["--config", str(config), "admin-token", "--output", str(output)]) == 0
    value = json.loads(output.read_text())
    assert value["scopes"] == ["admin:config"] and output.stat().st_mode & 0o077 == 0
    captured = capsys.readouterr()
    assert value["token"] not in captured.out + captured.err
    store = Store(load_settings(config).db_path)
    assert store.authenticate(value["token"])["scopes"] == ["admin:config"]
    store.close()


def test_corrupt_deflate_is_a_sanitized_package_error(management):
    client, app, store, key, admin_token, token, headers, report = management
    result = BytesIO()
    with zipfile.ZipFile(result, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("app.bin", b"x" * 1000)
        archive.writestr("manifest.json", "{}")
    with zipfile.ZipFile(BytesIO(result.getvalue())) as archive:
        offset = archive.getinfo("app.bin").header_offset
    damaged = bytearray(result.getvalue())
    name_size, extra_size = struct.unpack_from("<HH", damaged, offset + 26)
    damaged[offset + 30 + name_size + extra_size] = 255
    response = client.post("/api/v1/admin/firmware", content=bytes(damaged), headers={**headers, "Content-Type": "application/zip"})
    assert response.status_code == 400
    assert response.json()["error"]["code"] == "INVALID_FIRMWARE_PACKAGE"
    assert "decompress" not in response.text
    assert not app.state.management.packages_dir.exists()
