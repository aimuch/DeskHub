"""Failure modes of the power data path (device report -> Agent -> admin API), written before the code.

Rules: docs/rules.md K3 (unknown stays unknown, stale stays stale, nothing invented) and the plausible ranges
of R3: battery 2500-4600 mV, USB 3000-6500 mV, power-chip temperature -40..150 C.

P1  A report from older firmware (only vbus, battery, charging, fault) still validates; the new readings are null.
P2  A full report round-trips: level, battery mV, USB mV, chip temperature, measurement state.
P3  Numbers outside the plausible ranges are rejected, never clamped and never stored.
P4  Wrong types are rejected: booleans, strings, fractions where an integer is required, NaN.
P5  Cross-field rules. A failed measurement carries no numbers. No battery means no level and no battery voltage.
    No USB means no USB voltage. An unknown state carries no number for that thing.
P6  Unknown measurement states and unknown extra fields are rejected.
P7  power_state is "unreported" before the first report, "fresh" while the device is online, "stale" after
    that. A stale record shows no numbers, only when it was last heard from.
P8  A restart of the Agent never turns stored readings back into current ones.
P9  A newer report replaces the older one completely: nothing of the old level survives.
P10 A rejected report leaves the previous valid report in place.
P11 One device's readings never show up on another device.
P12 The list endpoint and the detail endpoint say the same thing.
"""
from __future__ import annotations

from copy import deepcopy
import json

from fastapi.testclient import TestClient
import pytest

from deskhub_agent.app import create_app
from deskhub_agent.management import Management
from deskhub_agent.management_models import DeviceConfiguration, DeviceReport
from deskhub_agent.storage import Store

ORIGIN = "http://testserver"
NUMERIC = ("soc_percent", "battery_mv", "vbus_mv", "chip_temp_c")


def power(**changes):
    value = {"vbus": "present", "battery": "present", "charging": "charging", "fault": False,
             "soc_percent": 67, "battery_mv": 3921, "vbus_mv": 5120, "chip_temp_c": 41.3, "measurement": "ok"}
    value.update(changes)
    return value


@pytest.fixture
def world(settings):
    store = Store(settings.db_path)
    admin_token = store.pair("browser-admin", ["admin:config"], ["*"])
    scopes = ["read:status", "control:displays", "device:sync"]
    tokens = {name: store.pair(name, scopes, ["monitor-desk-01"]) for name in ("terminal-one", "terminal-two")}
    app = create_app(settings, store=store)
    with TestClient(app) as client:
        session = client.post("/api/v1/admin/session", json={"token": admin_token}, headers={"Origin": ORIGIN})
        assert session.status_code == 200, session.text
        report = {"firmware_version": "0.3.0", "board_model": "ESP32-S3-Touch-AMOLED-2.16", "boot_id": "boot-one",
                  "uptime_seconds": 10, "free_heap_bytes": 100000, "applied_revision": 0,
                  "configuration": DeviceConfiguration().model_dump(), "config_error": None,
                  "power": power(),
                  "capabilities": {"management": True, "signed_ota": False, "layouts": ["desk", "clock", "focus"],
                                   "update_key_fingerprint": None}, "update": None}
        yield client, app, store, tokens, report
    store.close()


def sync(client, token, report, **kwargs):
    if kwargs:
        return client.post("/api/v1/device/sync", content=kwargs["raw"], headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    return client.post("/api/v1/device/sync", json=report, headers={"Authorization": "Bearer " + token})


def device(client, name="terminal-one"):
    response = client.get(f"/api/v1/admin/devices/{name}")
    assert response.status_code == 200, response.text
    return response.json()


def with_power(report, **changes):
    copy = deepcopy(report)
    copy["power"] = power(**changes)
    return copy


def age(app, name, seconds):
    app.state.management.seen[name] -= seconds


# P1
def test_old_report_without_measurements_still_validates(world):
    client, app, store, tokens, report = world
    old = deepcopy(report)
    old["power"] = {"vbus": "present", "battery": "absent", "charging": "idle", "fault": False}
    assert sync(client, tokens["terminal-one"], old).status_code == 200
    record = device(client)
    assert record["power_state"] == "fresh"
    assert record["power"]["vbus"] == "present" and record["power"]["battery"] == "absent"
    for key in NUMERIC:
        assert record["power"][key] is None
    assert record["power"]["measurement"] == "not_measured"


# P2
def test_full_report_round_trips(world):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], report).status_code == 200
    record = device(client)
    assert record["power_state"] == "fresh"
    assert record["power"] == power()
    assert DeviceReport.model_validate(report).power.battery_mv == 3921


# P3
@pytest.mark.parametrize("field,value", [
    ("battery_mv", 2499), ("battery_mv", 4601), ("battery_mv", 0), ("battery_mv", -1), ("battery_mv", 65535),
    ("vbus_mv", 2999), ("vbus_mv", 6501), ("vbus_mv", 0), ("vbus_mv", -5), ("vbus_mv", 20000),
    ("chip_temp_c", -40.1), ("chip_temp_c", 150.1), ("chip_temp_c", 400), ("chip_temp_c", -273.15),
    ("soc_percent", -1), ("soc_percent", 101), ("soc_percent", 255),
])
def test_implausible_numbers_are_rejected_not_clamped(world, field, value):
    client, app, store, tokens, report = world
    response = sync(client, tokens["terminal-one"], with_power(report, **{field: value}))
    assert response.status_code == 400, response.text
    assert device(client)["power_state"] == "unreported"


@pytest.mark.parametrize("field,value", [("battery_mv", 2500), ("battery_mv", 4600), ("vbus_mv", 3000), ("vbus_mv", 6500),
                                         ("chip_temp_c", -40), ("chip_temp_c", 150), ("soc_percent", 0), ("soc_percent", 100)])
def test_range_edges_are_accepted(world, field, value):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], with_power(report, **{field: value})).status_code == 200
    assert device(client)["power"][field] == value


# P4
@pytest.mark.parametrize("field,value", [
    ("battery_mv", True), ("battery_mv", "3900"), ("battery_mv", 3900.5), ("battery_mv", [3900]),
    ("vbus_mv", False), ("vbus_mv", "5000"), ("vbus_mv", 5000.25),
    ("soc_percent", 67.5), ("soc_percent", True), ("soc_percent", "67"),
    ("chip_temp_c", True), ("chip_temp_c", "41"), ("chip_temp_c", [41]),
])
def test_wrong_types_are_rejected(world, field, value):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], with_power(report, **{field: value})).status_code == 400


@pytest.mark.parametrize("literal", ["NaN", "Infinity", "-Infinity"])
def test_non_finite_temperature_is_rejected(world, literal):
    client, app, store, tokens, report = world
    text = json.dumps(with_power(report, chip_temp_c=0.0)).replace('"chip_temp_c": 0.0', '"chip_temp_c": ' + literal)
    assert literal in text
    assert sync(client, tokens["terminal-one"], None, raw=text).status_code == 400


# P5
@pytest.mark.parametrize("changes", [
    {"measurement": "adc_read_failed"},                                    # failed measurement but numbers present
    {"measurement": "adc_enable_failed"},
    {"measurement": "adc_out_of_range"},
    {"measurement": "not_measured"},
    {"battery": "absent"},                                                 # no battery, level and voltage present
    {"battery": "absent", "soc_percent": None},                            # no battery, voltage present
    {"battery": "unknown"},
    {"battery": "unknown", "soc_percent": None},
    {"vbus": "absent"},                                                    # no USB, USB voltage present
    {"vbus": "unknown"},
])
def test_cross_field_rules(world, changes):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], with_power(report, **changes)).status_code == 400


@pytest.mark.parametrize("changes", [
    {"measurement": "adc_read_failed", "soc_percent": None, "battery_mv": None, "vbus_mv": None, "chip_temp_c": None},
    {"measurement": "adc_enable_failed", "soc_percent": 55, "battery_mv": None, "vbus_mv": None, "chip_temp_c": None},   # the level is the gauge's own
    {"measurement": "not_measured", "soc_percent": None, "battery_mv": None, "vbus_mv": None, "chip_temp_c": None},
    {"battery": "absent", "soc_percent": None, "battery_mv": None, "charging": "idle"},
    {"vbus": "absent", "vbus_mv": None, "charging": "idle"},
    {"battery": "present", "soc_percent": None},                            # a present battery with no level yet is real
    {"fault": True, "measurement": "not_measured", "soc_percent": None, "battery_mv": None, "vbus_mv": None, "chip_temp_c": None},
])
def test_consistent_reports_are_accepted(world, changes):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], with_power(report, **changes)).status_code == 200


# P6
@pytest.mark.parametrize("changes", [{"measurement": "good"}, {"measurement": ""}, {"measurement": None}, {"battery_temp_c": 30}, {"current_ma": 120}])
def test_unknown_states_and_extra_fields_are_rejected(world, changes):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], with_power(report, **changes)).status_code == 400


@pytest.mark.parametrize("state", ["good", "OK", "", "unknown", "adc_failed", None, 1, True])
def test_unknown_measurement_state_is_rejected_even_without_numbers(world, state):
    client, app, store, tokens, report = world
    bare = {"soc_percent": None, "battery_mv": None, "vbus_mv": None, "chip_temp_c": None, "measurement": state}
    assert sync(client, tokens["terminal-one"], with_power(report, **bare)).status_code == 400


# P7
def test_power_state_unreported_fresh_stale(world):
    client, app, store, tokens, report = world
    record = device(client)
    assert record["power_state"] == "unreported"
    assert all(record["power"][key] is None for key in NUMERIC) and record["power"]["measurement"] == "not_measured"
    assert record["power_reported_at"] is None
    assert sync(client, tokens["terminal-one"], report).status_code == 200
    fresh = device(client)
    assert fresh["power_state"] == "fresh" and fresh["power"]["battery_mv"] == 3921
    assert fresh["power_reported_at"] == fresh["last_seen"]
    age(app, "terminal-one", 29)
    assert device(client)["power_state"] == "fresh"
    age(app, "terminal-one", 2)
    stale = device(client)
    assert stale["power_state"] == "stale"
    assert all(stale["power"][key] is None for key in NUMERIC)
    assert stale["power"]["measurement"] == "not_measured"
    assert stale["power_reported_at"] == fresh["last_seen"]


# P8
def test_restart_never_makes_stored_readings_current(world):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], report).status_code == 200
    restarted = Management(app.state.management.settings, store)
    record = restarted.device("terminal-one")
    assert record["power_state"] == "stale" and record["power_reported_at"]
    assert all(record["power"][key] is None for key in NUMERIC)


# P9
def test_newer_report_replaces_everything_of_the_older_one(world):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], report).status_code == 200
    unplugged = with_power(report, battery="absent", charging="idle", soc_percent=None, battery_mv=None)
    assert sync(client, tokens["terminal-one"], unplugged).status_code == 200
    record = device(client)
    assert record["power"]["battery"] == "absent" and record["power"]["soc_percent"] is None and record["power"]["battery_mv"] is None
    assert record["power"]["vbus_mv"] == 5120


# P10
def test_rejected_report_keeps_the_previous_valid_one(world):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], report).status_code == 200
    assert sync(client, tokens["terminal-one"], with_power(report, battery_mv=9999)).status_code == 400
    record = device(client)
    assert record["power"]["battery_mv"] == 3921 and record["power_state"] == "fresh"


# P11
def test_readings_are_isolated_between_devices(world):
    client, app, store, tokens, report = world
    other = with_power(report, battery="absent", charging="idle", soc_percent=None, battery_mv=None, vbus_mv=4980, chip_temp_c=38.5)
    assert sync(client, tokens["terminal-one"], report).status_code == 200
    assert sync(client, tokens["terminal-two"], other).status_code == 200
    one, two = device(client, "terminal-one"), device(client, "terminal-two")
    assert one["power"]["battery_mv"] == 3921 and one["power"]["vbus_mv"] == 5120
    assert two["power"]["battery_mv"] is None and two["power"]["vbus_mv"] == 4980 and two["power"]["chip_temp_c"] == 38.5
    age(app, "terminal-one", 60)
    assert device(client, "terminal-one")["power_state"] == "stale"
    assert device(client, "terminal-two")["power_state"] == "fresh"


# P12
def test_list_and_detail_agree(world):
    client, app, store, tokens, report = world
    assert sync(client, tokens["terminal-one"], report).status_code == 200
    listed = client.get("/api/v1/admin/devices").json()
    items = listed["devices"] if isinstance(listed, dict) else listed
    entry = next(item for item in items if item["device_id"] == "terminal-one")
    detail = device(client)
    for key in ("power", "power_state", "power_reported_at"):
        assert entry[key] == detail[key]
    # a device that never reported is listed as unreported, not as zero
    other = next(item for item in items if item["device_id"] == "terminal-two")
    assert other["power_state"] == "unreported" and all(other["power"][key] is None for key in NUMERIC)
