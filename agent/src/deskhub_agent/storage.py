from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import secrets
import sqlite3
import threading
from datetime import timedelta
from uuid import uuid4

from pydantic import TypeAdapter

from .errors import AgentError
from .models import CommandRequest, Identifier, SCOPES, stamp, utcnow


class Store:
    """SQLite is the side-effect journal and credential authority, not a replay queue."""

    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        # Create privately before sqlite opens it (umask does not protect an existing file).
        fd = os.open(self.path, os.O_CREAT | os.O_RDWR, 0o600)
        os.close(fd)
        os.chmod(self.path, 0o600)
        self.lock = threading.RLock()
        self.connection = sqlite3.connect(self.path, check_same_thread=False, isolation_level=None, timeout=5)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA journal_mode=DELETE")
        self.connection.execute("PRAGMA synchronous=FULL")
        version = self.connection.execute("PRAGMA user_version").fetchone()[0]
        if version > 1:
            self.connection.close()
            raise ValueError("database schema is newer than this Agent; preserve it and use the matching version")
        self.connection.executescript('''
            CREATE TABLE IF NOT EXISTS devices (
                device_id TEXT PRIMARY KEY, token_hash TEXT NOT NULL UNIQUE,
                scopes TEXT NOT NULL, targets TEXT NOT NULL, revoked INTEGER NOT NULL DEFAULT 0,
                created_at TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS commands (
                command_id TEXT PRIMARY KEY, device_id TEXT NOT NULL, idempotency_key TEXT NOT NULL,
                fingerprint TEXT NOT NULL, request_json TEXT NOT NULL, target_id TEXT NOT NULL,
                state TEXT NOT NULL, result_json TEXT NOT NULL, accepted_at TEXT NOT NULL,
                UNIQUE(device_id, idempotency_key)
            );
            CREATE UNIQUE INDEX IF NOT EXISTS one_active_target
              ON commands(target_id) WHERE state IN ('queued', 'executing');
            CREATE INDEX IF NOT EXISTS command_age ON commands(accepted_at);
        ''')
        self.connection.execute("PRAGMA user_version=1")

    def close(self) -> None:
        with self.lock:
            self.connection.close()

    def pair(self, device_id: str, scopes: list[str], targets: list[str], replace: bool = False) -> str:
        TypeAdapter(Identifier).validate_python(device_id)
        if not scopes or not set(scopes) <= SCOPES:
            raise ValueError("unknown or empty scope")
        if ("control:displays" in scopes and not targets) or len(targets) > 4 or len(set(targets)) != len(targets):
            raise ValueError("display control requires 1–4 unique target IDs, or ['*']")
        for target in targets:
            if target != "*":
                TypeAdapter(Identifier).validate_python(target)
        token = secrets.token_urlsafe(32)
        with self.lock, self.connection:
            existing = self.connection.execute("SELECT revoked FROM devices WHERE device_id=?", (device_id,)).fetchone()
            if existing and not existing["revoked"] and not replace:
                raise ValueError("device is already paired; use explicit replacement")
            self.connection.execute("""INSERT OR REPLACE INTO devices
                (device_id, token_hash, scopes, targets, revoked, created_at) VALUES(?,?,?,?,0,?)""",
                (device_id, hashlib.sha256(token.encode()).hexdigest(), json.dumps(scopes), json.dumps(targets), stamp()))
        return token

    def revoke(self, device_id: str) -> None:
        with self.lock, self.connection:
            self.connection.execute("UPDATE devices SET revoked=1 WHERE device_id=?", (device_id,))
            if self.connection.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='update_jobs'").fetchone():
                self.connection.execute("""UPDATE update_jobs SET
                    state=CASE WHEN state='queued' THEN 'failed' ELSE 'unconfirmed' END,
                    error_code='DEVICE_REVOKED',completed_at=? WHERE device_id=?
                    AND state IN ('queued','downloading','verifying','writing','rebooting','pending_confirmation')""", (stamp(), device_id))

    def authenticate(self, token: str) -> dict:
        if not 20 <= len(token) <= 256:
            raise AgentError("UNAUTHORIZED", "A paired device credential is required", 401)
        with self.lock:
            row = self.connection.execute("SELECT * FROM devices WHERE token_hash=? AND revoked=0",
                (hashlib.sha256(token.encode()).hexdigest(),)).fetchone()
        if not row:
            raise AgentError("UNAUTHORIZED", "Invalid or revoked device credential", 401)
        return self._identity(row)

    def identity(self, device_id: str) -> dict:
        with self.lock:
            row = self.connection.execute("SELECT * FROM devices WHERE device_id=? AND revoked=0", (device_id,)).fetchone()
        if not row:
            raise AgentError("FORBIDDEN", "Device was revoked before execution", 403)
        return self._identity(row)

    @staticmethod
    def _identity(row) -> dict:
        return {"device_id": row["device_id"], "scopes": json.loads(row["scopes"]), "targets": json.loads(row["targets"])}

    def devices(self) -> list[dict]:
        with self.lock:
            return [{**self._identity(row), "revoked": bool(row["revoked"]), "created_at": row["created_at"]}
                    for row in self.connection.execute("SELECT * FROM devices ORDER BY device_id")]

    @staticmethod
    def authorize(identity: dict, scope: str, target_id: str | None = None) -> None:
        if scope not in identity["scopes"]:
            raise AgentError("FORBIDDEN", "Credential does not permit this operation", 403)
        if target_id and "*" not in identity["targets"] and target_id not in identity["targets"]:
            raise AgentError("FORBIDDEN", "Credential does not permit this target", 403)

    @staticmethod
    def fingerprint(request: CommandRequest) -> str:
        # request_id is a tracing value, but the immutable action, timestamps and key
        # must all match on replay. Do not silently change expiry when retrying.
        body = request.model_dump(mode="json")
        return hashlib.sha256(json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()

    def existing(self, identity: dict, request: CommandRequest) -> dict | None:
        with self.lock:
            row = self.connection.execute("SELECT * FROM commands WHERE device_id=? AND idempotency_key=?",
                (identity["device_id"], request.idempotency_key)).fetchone()
            if not row:
                return None
            if row["fingerprint"] != self.fingerprint(request):
                raise AgentError("IDEMPOTENCY_CONFLICT", "Idempotency key already has a different payload", 409)
            self.authorize(identity, "control:displays", row["target_id"])
            return json.loads(row["result_json"])

    def enqueue(self, identity: dict, request: CommandRequest) -> tuple[dict, bool]:
        with self.lock:
            self.connection.execute("BEGIN IMMEDIATE")
            try:
                existing = self.existing(identity, request)
                if existing:
                    self.connection.execute("COMMIT")
                    return existing, False
                active = self.connection.execute("SELECT 1 FROM commands WHERE target_id=? AND state IN ('queued','executing')",
                    (request.target_id,)).fetchone()
                if active:
                    raise AgentError("BUSY", "A command is already active for this display", 409, True)
                result = {"request_id": request.request_id, "command_id": f"cmd-{uuid4().hex}", "state": "queued",
                          "target_id": request.target_id, "requested": {"input_id": request.parameters.input_id},
                          "observed": {"input_id": None, "quality": "unavailable"},
                          "verification": {"method": "readback", "result": "pending", "reason_code": None},
                          "error": None, "accepted_at": stamp(), "completed_at": None}
                self.connection.execute("INSERT INTO commands VALUES(?,?,?,?,?,?,?,?,?)",
                    (result["command_id"], identity["device_id"], request.idempotency_key, self.fingerprint(request),
                     request.model_dump_json(), request.target_id, "queued", json.dumps(result), result["accepted_at"]))
                self.connection.execute("COMMIT")
                return result, True
            except BaseException:
                if self.connection.in_transaction:
                    self.connection.execute("ROLLBACK")
                raise

    def update(self, command_id: str, **changes) -> dict:
        with self.lock:
            row = self.connection.execute("SELECT result_json FROM commands WHERE command_id=?", (command_id,)).fetchone()
            if not row:
                raise KeyError(command_id)
            result = {**json.loads(row["result_json"]), **changes}
            self.connection.execute("UPDATE commands SET state=?, result_json=? WHERE command_id=?",
                (result["state"], json.dumps(result), command_id))
            return result

    def query(self, identity: dict, command_id: str | None = None, key: str | None = None) -> dict:
        with self.lock:
            column, value = ("command_id", command_id) if command_id else ("idempotency_key", key)
            row = self.connection.execute(f"SELECT * FROM commands WHERE device_id=? AND {column}=?",
                (identity["device_id"], value)).fetchone()
        if not row:
            raise AgentError("COMMAND_NOT_FOUND", "No command belongs to this device and identifier", 404)
        self.authorize(identity, "control:displays", row["target_id"])
        return json.loads(row["result_json"])

    def recent(self, limit: int) -> list[dict]:
        """Newest-first command summary for the management console.

        Idempotency keys and raw request bodies stay out of the view; rows whose
        stored JSON is unreadable are skipped instead of failing the whole list.
        """
        with self.lock:
            rows = self.connection.execute(
                "SELECT device_id, request_json, result_json FROM commands ORDER BY accepted_at DESC, rowid DESC LIMIT ?",
                (limit,)).fetchall()
        summaries = []
        for row in rows:
            try:
                request, result = json.loads(row["request_json"]), json.loads(row["result_json"])
                summaries.append({
                    "command_id": result["command_id"], "device_id": row["device_id"], "action_id": request["action_id"],
                    "target_id": result["target_id"], "requested_input": result["requested"]["input_id"],
                    "state": result["state"], "observed_input": result["observed"]["input_id"],
                    "verification": result["verification"]["result"], "reason_code": result["verification"].get("reason_code"),
                    "error_code": (result.get("error") or {}).get("code"),
                    "accepted_at": result["accepted_at"], "completed_at": result.get("completed_at")})
            except (ValueError, KeyError, TypeError):
                continue
        return summaries

    def recover(self) -> None:
        """Called only after acquiring the single-agent process lock."""
        with self.lock:
            rows = list(self.connection.execute("SELECT command_id,state FROM commands WHERE state IN ('queued','executing')"))
            for row in rows:
                executing = row["state"] == "executing"
                self.update(row["command_id"], state="unconfirmed" if executing else "cancelled", completed_at=stamp(),
                            verification={"method": "readback", "result": "unavailable",
                                          "reason_code": "AGENT_RESTARTED_DURING_EXECUTION" if executing else "AGENT_RESTARTED_BEFORE_EXECUTION"})

    def cleanup(self, retention_hours: int) -> None:
        with self.lock:
            self.connection.execute("DELETE FROM commands WHERE state NOT IN ('queued','executing') AND accepted_at < ?",
                (stamp(utcnow() - timedelta(hours=retention_hours)),))
