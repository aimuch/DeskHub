#!/usr/bin/env python3
"""Export the actual Agent API and schemas; --check rejects stale contracts.

This creates an unstarted app with a temporary SQLite database and disabled
hardware backend. No HTTP listener, display call, credential or user state is used.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "agent/src"))
sys.path.insert(0, str(ROOT / "tools"))


def usb_request_schema() -> dict[str, Any]:
    identifier = {"type": "string", "pattern": "^[A-Za-z0-9][A-Za-z0-9_.:-]{0,95}$"}
    utc = {
        "type": "string", "format": "date-time",
        "pattern": "^20(?:2[4-9]|[3-9][0-9])-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})Z$",
        "description": "2024–2099; calendar validity is checked by firmware; no fractional seconds.",
    }
    clock = {"type": "object", "required": ["utc"], "properties": {
        "utc": utc,
        "ntp_server": {"type": "string", "minLength": 1, "maxLength": 253, "default": "pool.ntp.org", "description": "Optional provisioning setting: ASCII DNS hostname or IPv4 address, no URL/path/port. Supports LAN NTP without Internet. set_time does not change the persisted NTP server."},
    }}
    config = {
        "type": "object", "required": ["wifi", "agent", "time"],
        "properties": {
            "wifi": {"type": "object", "required": ["ssid", "password"], "properties": {
                "ssid": {"type": "string", "minLength": 1, "maxLength": 32, "description": "Maximum 32 UTF-8 bytes, not codepoints; no NUL."},
                "password": {"type": "string", "maxLength": 63, "writeOnly": True, "description": "Empty for open network, otherwise 8–63 UTF-8 bytes; no NUL."},
            }},
            "agent": {"type": "object", "required": ["url", "agent_id", "token", "ca_pem"], "properties": {
                "url": {"type": "string", "maxLength": 256, "pattern": "^https://", "description": "HTTPS origin only; hostname verification remains enabled; no userinfo, path, query or fragment."},
                "agent_id": identifier,
                "token": {"type": "string", "minLength": 32, "maxLength": 256, "pattern": "^[!-~]+$", "writeOnly": True},
                "ca_pem": {"type": "string", "maxLength": 8192, "description": "Valid PEM CA certificate(s), maximum 8192 UTF-8 bytes; never a private key."},
            }},
            "time": clock,
            "updates": {"type": "object", "additionalProperties": False, "required": ["public_key_pem"], "properties": {
                "public_key_pem": {"type": "string", "maxLength": 2048, "description": "Optional independently trusted ECDSA P-256 SPKI public key, installed through USB. Private keys rejected. Omission preserves an existing update trust root."},
            }},
        },
    }
    variants = []
    for name in ("identify", "status", "set_time", "provision"):
        variant = {"type": "object", "required": ["op"], "properties": {"op": {"const": name}}}
        if name == "set_time":
            variant["required"].append("time")
            variant["properties"]["time"] = clock
        if name == "provision":
            variant["required"].append("config")
            variant["properties"]["config"] = config
        variants.append(variant)
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema", "$id": "urn:deskhub:usb:v1:request",
        "title": "DeskHub USB request", "description": "UTF-8 JSON Lines over native USB Serial/JTAG. Maximum 16384 bytes including newline. Config is not logged. No arbitrary shell or register operations.",
        "oneOf": variants,
    }


def usb_response_schema() -> dict[str, Any]:
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema", "$id": "urn:deskhub:usb:v1:response",
        "title": "DeskHub USB response", "oneOf": [
            {"type": "object", "required": ["ok", "result"], "properties": {"ok": {"const": True}, "result": {"type": "object"}}, "description": "provision result must include saved:true only after persistent commit."},
            {"type": "object", "required": ["ok", "error"], "properties": {"ok": {"const": False}, "error": {"type": "object", "required": ["code", "message"], "properties": {"code": {"type": "string"}, "message": {"type": "string"}}}}},
        ],
    }


def exported() -> dict[Path, dict[str, Any]]:
    from deskhub_agent.app import create_app
    from deskhub_agent.models import CommandRequest, CommandResult, ErrorResponse, HostsResponse, Metric, Settings, Status
    from deskhub_agent.management_models import DeviceConfiguration, ConfigurationWrite, DeviceReport, FirmwarePayload, UpdateCreate
    from deskhub_agent.storage import Store

    output: dict[Path, dict[str, Any]] = {}
    with tempfile.TemporaryDirectory(prefix="deskhub-contract-") as directory:
        settings = Settings(agent_id="agent-protocol-export", db_path=str(Path(directory) / "state.sqlite3"))
        store = Store(settings.db_path)
        try:
            api = create_app(settings, store=store).openapi()
        finally:
            store.close()
    # Authentication is enforced by a dependency that reads Authorization directly.
    # Describe that actual behavior explicitly for API clients.
    api.setdefault("components", {}).setdefault("securitySchemes", {})["DeviceBearer"] = {
        "type": "http", "scheme": "bearer", "description": "Independent revocable per-device credential, issued through the local management tool. Scopes and target permissions are checked on every business request.",
    }
    api["components"]["securitySchemes"]["AdminSession"] = {
        "type": "apiKey", "in": "cookie", "name": "deskhub_session",
        "description": "HttpOnly SameSite=Strict 30-minute session. A browser signs in with the administrator password (POST /api/v1/admin/login); the management token only starts a first-time session that may do nothing but set the password. A script exchanges a management token without an Origin header and keeps full access; revoking or replacing the token ends that session. Setting, changing or resetting the password ends every password session.",
    }
    api["components"]["securitySchemes"]["AdminCSRF"] = {
        "type": "apiKey", "in": "header", "name": "X-CSRF-Token",
        "description": "Management writes also require matching same-origin Origin. Session endpoints return the CSRF token.",
    }
    for path, methods in api["paths"].items():
        for method, operation in methods.items():
            if method not in {"get", "post", "put", "delete", "patch", "options", "head"}:
                continue
            if path.startswith("/api/v1/admin/"):
                if (path == "/api/v1/admin/session" and method == "post") or path in {"/api/v1/admin/login", "/api/v1/admin/auth"}:
                    operation["security"] = []
                else:
                    requirement = {"AdminSession": []}
                    if method not in {"get", "head"}:
                        requirement["AdminCSRF"] = []
                    operation["security"] = [requirement]
            elif path.startswith("/api/"):
                operation["security"] = [{"DeviceBearer": []}]
            if path == "/api/v1/admin/firmware" and method == "post":
                operation["requestBody"] = {"required": True, "content": {"application/zip": {"schema": {"type": "string", "format": "binary"}}}, "description": "Signed app-only ZIP, at most 7 MiB; independently trusted P-256 signature, board/chip/schema, image size/hash/version all verified before retention."}
            if path == "/api/v1/device/firmware/{package_id}/image" and method == "get":
                operation["responses"]["200"] = {"description": "Only the app image assigned to this authenticated terminal's active update job.", "content": {"application/octet-stream": {"schema": {"type": "string", "format": "binary"}}}}
            # Runtime maps FastAPI validation errors to a sanitized 400 response.
            operation.get("responses", {}).pop("422", None)
    api["info"]["description"] = (
        "DeskHub protocol v1.0; Agent and firmware versions are separate. "
        "Device business endpoints require per-device Bearer credentials; /api/v1/admin uses an independently authenticated cookie session and CSRF-protected same-origin writes. "
        "LAN traffic requires TLS and configured network access. Command bodies are limited to 4096 bytes, snapshots to 16384 bytes. "
        "sent_at is required to enforce a maximum 5 second client clock skew. expires_at defaults to sent_at + 10 seconds, and validity cannot exceed 30 seconds. "
        "Identical retries use the original device identity, idempotency_key, timestamps and payload. "
        "Use GET /api/v1/commands?idempotency_key=... to recover a lost submission response. "
        "A zero backend exit code alone cannot produce succeeded. "
        "A succeeded display command requires matching readback; a lost control path produces unconfirmed. "
        "Current 0.2 development firmware management configuration uses schema 1 with default layout.theme=mint and display.rotation=0; omitted additive fields are normalized before revision ACK comparison. This does not claim management, themes or rotation support for 0.1 firmware."
    )
    api["x-deskhub-limits"] = {"command_bytes": 4096, "snapshot_bytes": 16384, "max_clock_skew_seconds": 5, "default_validity_seconds": 10, "max_validity_seconds": 30}
    output[Path("openapi.json")] = api
    models = {
        "command-request": CommandRequest, "command-result": CommandResult, "error": ErrorResponse,
        "metric": Metric, "hosts": HostsResponse, "status": Status, "agent-config": Settings,
        "device-configuration": DeviceConfiguration, "configuration-write": ConfigurationWrite,
        "device-report": DeviceReport, "firmware-payload": FirmwarePayload, "update-create": UpdateCreate,
    }
    for name, model in models.items():
        schema = model.model_json_schema()
        schema["$schema"] = "https://json-schema.org/draft/2020-12/schema"
        schema["$id"] = f"urn:deskhub:protocol:v1:{name}"
        output[Path("schemas") / f"{name}.schema.json"] = schema
    output[Path("schemas/usb-request.schema.json")] = usb_request_schema()
    output[Path("schemas/usb-response.schema.json")] = usb_response_schema()
    return output


def validate_examples() -> int:
    from deskhub_agent.models import CommandRequest, CommandResult, ErrorResponse, HostsResponse, Metric, Status
    from deskhub_agent.management_models import DeviceConfiguration, DeviceReport
    from provision import valid_utc

    examples = {
        "command-request.json": CommandRequest, "command-unconfirmed.json": CommandResult,
        "command-succeeded.json": CommandResult, "metric-unavailable.json": Metric,
        "error-forbidden.json": ErrorResponse, "status.json": Status,
    }
    for filename, model in examples.items():
        model.model_validate_json((ROOT / "protocol/examples" / filename).read_text())
    identify = json.loads((ROOT / "protocol/examples/usb-identify.json").read_text())
    assert identify == {"op": "identify"}
    set_time = json.loads((ROOT / "protocol/examples/usb-set-time.json").read_text())
    assert set_time["op"] == "set_time"
    valid_utc(set_time["time"]["utc"])
    command = CommandRequest.model_validate_json((ROOT / "protocol/examples/command-request.json").read_text())
    assert 0 < (command.expires_at - command.sent_at).total_seconds() <= 30
    result = CommandResult.model_validate_json((ROOT / "protocol/examples/command-succeeded.json").read_text())
    assert result.observed.input_id == result.requested.input_id and result.observed.quality == "valid"
    assert result.verification.result == "matched"
    DeviceConfiguration.model_validate_json((ROOT / "protocol/examples/device-configuration.json").read_text())
    DeviceReport.model_validate_json((ROOT / "protocol/examples/device-report.json").read_text())
    DeviceReport.model_validate_json((ROOT / "protocol/examples/device-report-power.json").read_text())
    # The pairing package vectors are shared with the device implementation: the Agent's decoder must agree with them.
    from deskhub_agent.pairing import decode_package
    vectors = json.loads((ROOT / "protocol/examples/pairing-package.json").read_text())
    for item in vectors["valid"]:
        decoded = decode_package(item["package"])
        assert (decoded["url"], decoded["grant_id"], decoded["code"], decoded["ca_der"].hex()) == (item["url"], item["grant_id"], item["code"], item["ca_der_hex"])
    for item in vectors["invalid"]:
        try:
            decode_package(item["package"])
        except ValueError:
            continue
        raise AssertionError("an invalid pairing package was accepted: " + item["why"])
    return len(examples) + 6


def main(argv=None) -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--check", action="store_true", help="Reject stale generated files; also validate all sample payloads with implementation models")
    args = cli.parse_args(argv)
    stale = []
    try:
        outputs = exported()
        for relative, payload in outputs.items():
            target = ROOT / "protocol" / relative
            rendered = json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
            if args.check:
                if not target.exists() or target.read_text() != rendered:
                    stale.append(str(relative))
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(rendered)
        count = validate_examples()
        if stale:
            print("Stale protocol files: " + ", ".join(stale), file=sys.stderr)
            print("Run python protocol/export_openapi.py after changing the Agent models/API.", file=sys.stderr)
            return 1
        print(f"Protocol {'checked' if args.check else 'exported'}: {len(outputs)} contracts; {count} implementation-validated examples.")
        return 0
    except Exception as error:
        print(f"Protocol validation failed: {type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
