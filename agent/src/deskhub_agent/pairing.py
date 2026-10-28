"""Hotspot pairing, the Agent side (docs/pairing-design.md).

An admin creates a grant; the console shows a pairing package once. A person pastes the package into the device's
setup page. The device pins this Agent by the CA fingerprint inside the package and redeems the grant here for a
per-device token. Rules: K1 (the CA is pinned, one token per device), K4 (a code or token is never logged, never
stored in clear, never shown twice), R7 (one-time, time-limited).
"""
from __future__ import annotations

import base64
from contextlib import contextmanager
from datetime import datetime, timedelta
import hashlib
import hmac
import json
from pathlib import Path
import re
import secrets
import sqlite3
from typing import Literal

from cryptography import x509
from cryptography.hazmat.primitives import serialization
from fastapi import Depends, Request
from pydantic import Field, field_validator, model_validator

from .errors import AgentError
from .management_models import StrictModel
from .models import Identifier, SCOPES, stamp, utcnow, valid_public_url

PREFIX = "DHP1."
ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"  # Crockford base32 without I, L, O, U
GRANT_ID_LENGTH, CODE_LENGTH = 8, 10
ID_PATTERN = r"^[0-9A-HJKMNP-TV-Z]{8}$"
CODE_PATTERN = r"^[0-9A-HJKMNP-TV-Z]{10}$"
MAX_TTL_SECONDS = 300
MAX_PENDING = 8
ATTEMPTS = 5
GRANT_SCOPES = ("read:status", "control:displays", "device:sync")
REJECTED = ("PAIRING_REJECTED", "The pairing request was not accepted", 403)  # the one answer for every refusal
# What an id that does not exist is compared against, so it costs the same as one that does.
DUMMY_SALT = "0" * 32


def _reject() -> AgentError:
    return AgentError(*REJECTED)


# ---------------------------------------------------------------- package
def encode_package(url: str, grant_id: str, code: str, fingerprint: bytes) -> str:
    if not valid_public_url(url):
        raise ValueError("the Agent address must be an https origin")
    if not re.fullmatch(ID_PATTERN, grant_id) or not re.fullmatch(CODE_PATTERN, code):
        raise ValueError("grant id and code use the 32-character alphabet and fixed lengths")
    if not isinstance(fingerprint, bytes) or len(fingerprint) != 32:
        raise ValueError("the CA fingerprint is 32 bytes")
    raw_url = url.encode()
    body = bytes([1, len(raw_url)]) + raw_url + grant_id.encode() + code.encode() + fingerprint
    return PREFIX + base64.urlsafe_b64encode(body + hashlib.sha256(body).digest()[:4]).rstrip(b"=").decode()


def decode_package(text: str) -> dict:
    """The reference decoder. The device implements the same rules in C++ against the same vectors."""
    text = text.strip()
    if not text.startswith(PREFIX):
        raise ValueError("not a DeskHub pairing package")
    payload = text[len(PREFIX):]
    if not payload or not re.fullmatch(r"[A-Za-z0-9_-]+", payload):
        raise ValueError("the package has characters it cannot have")
    try:
        raw = base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4))
    except ValueError as error:
        raise ValueError("the package is not base64") from error
    if base64.urlsafe_b64encode(raw).rstrip(b"=").decode() != payload:
        raise ValueError("the package is not in its canonical form")  # a changed last character is not accepted
    if len(raw) < 2 + 8 + 10 + 32 + 4:
        raise ValueError("the package is too short")
    body, check = raw[:-4], raw[-4:]
    if not hmac.compare_digest(hashlib.sha256(body).digest()[:4], check):
        raise ValueError("the package was damaged while copying")
    if body[0] != 1:
        raise ValueError("unknown package version")
    size = body[1]
    rest = body[2 + size:]
    if size == 0 or len(body) != 2 + size + 8 + 10 + 32:
        raise ValueError("the package has the wrong length")
    try:
        url, grant_id, code = body[2:2 + size].decode("ascii"), rest[:8].decode("ascii"), rest[8:18].decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError("the package has bytes it cannot have") from error
    if not valid_public_url(url) or not re.fullmatch(ID_PATTERN, grant_id) or not re.fullmatch(CODE_PATTERN, code):
        raise ValueError("the package carries an invalid address, id or code")
    return {"url": url, "grant_id": grant_id, "code": code, "fingerprint": rest[18:]}


def ca_fingerprint(pem: str) -> bytes:
    """SHA-256 of the CA certificate in DER form. A private key or anything else is not a CA."""
    try:
        certificate = x509.load_pem_x509_certificate(pem.encode())
    except (ValueError, TypeError) as error:
        raise ValueError("not a certificate") from error
    return hashlib.sha256(certificate.public_bytes(serialization.Encoding.DER)).digest()


def _random(length: int) -> str:
    return "".join(secrets.choice(ALPHABET) for _ in range(length))


def _hash(salt: str, code: str) -> str:
    return hashlib.sha256((salt + code).encode()).hexdigest()


def _time(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00"))


@contextmanager
def _transaction(connection):
    connection.execute("BEGIN IMMEDIATE")
    try:
        yield
        connection.execute("COMMIT")
    except BaseException:
        if connection.in_transaction:
            connection.execute("ROLLBACK")
        raise


# ---------------------------------------------------------------- requests
class GrantCreate(StrictModel):
    scopes: list[Literal["read:status", "control:displays", "device:sync"]] = Field(min_length=1, max_length=3)
    targets: list[Identifier] = Field(default_factory=list, max_length=4)
    ttl_seconds: int = Field(default=MAX_TTL_SECONDS, ge=1, le=MAX_TTL_SECONDS)
    replace: bool = False
    device_id: Identifier | None = None

    @field_validator("scopes", "targets")
    @classmethod
    def unique(cls, value):
        if len(set(value)) != len(value):
            raise ValueError("duplicates are not allowed")
        return value

    @model_validator(mode="after")
    def display_control_needs_targets(self):
        if "control:displays" in self.scopes and not self.targets:
            raise ValueError("display control needs at least one target")
        return self


class Redeem(StrictModel):
    grant_id: str = Field(pattern=ID_PATTERN)
    code: str = Field(pattern=CODE_PATTERN)
    device_id: Identifier


# ---------------------------------------------------------------- the grants
class Pairing:
    def __init__(self, settings, store):
        self.settings, self.store = settings, store
        with store.lock:
            store.connection.executescript("""
                CREATE TABLE IF NOT EXISTS pairing_grants (
                    grant_id TEXT PRIMARY KEY, salt TEXT NOT NULL, code_hash TEXT NOT NULL,
                    scopes TEXT NOT NULL, targets TEXT NOT NULL, device_id TEXT, replace_existing INTEGER NOT NULL,
                    created_at TEXT NOT NULL, expires_at TEXT NOT NULL, attempts_left INTEGER NOT NULL,
                    state TEXT NOT NULL CHECK (state IN ('pending', 'used', 'cancelled', 'burned', 'expired')),
                    redeemed_device_id TEXT, redeemed_at TEXT
                );
            """)

    # An expired grant is marked as such for good: moving the clock back does not bring it back.
    def _expire(self, now: datetime) -> None:
        self.store.connection.execute("UPDATE pairing_grants SET state='expired' WHERE state='pending' AND expires_at <= ?", (stamp(now),))

    def _configuration(self) -> tuple[str, bytes, str]:
        settings = self.settings
        if not settings.public_url or not settings.pairing_ca:
            raise AgentError("PAIRING_NOT_CONFIGURED", "Pairing needs public_url and pairing_ca in the Agent configuration", 409)
        try:
            pem = Path(settings.pairing_ca).read_text()
            return settings.public_url, ca_fingerprint(pem), pem
        except (OSError, UnicodeDecodeError, ValueError) as error:
            raise AgentError("PAIRING_NOT_CONFIGURED", "pairing_ca is not a readable CA certificate", 409) from error

    def create(self, scopes: list[str], targets: list[str], ttl_seconds: int = MAX_TTL_SECONDS, replace: bool = False, device_id: str | None = None) -> dict:
        url, fingerprint, _ = self._configuration()
        if not scopes or not set(scopes) <= set(GRANT_SCOPES) or len(set(scopes)) != len(scopes):
            raise AgentError("INVALID_ARGUMENT", "A grant carries read:status, control:displays or device:sync, never admin:config", 400)
        known = {display.monitor_id for display in self.settings.displays}
        if len(targets) > 4 or len(set(targets)) != len(targets) or not set(targets) <= known:
            raise AgentError("INVALID_ARGUMENT", "Targets must be configured displays; * is not accepted", 400)
        if "control:displays" in scopes and not targets:
            raise AgentError("INVALID_ARGUMENT", "Display control needs at least one target", 400)
        if type(ttl_seconds) is not int or not 1 <= ttl_seconds <= MAX_TTL_SECONDS:
            raise AgentError("INVALID_ARGUMENT", "A grant lives 1 to 300 seconds", 400)
        now = utcnow()
        with self.store.lock:
            self._expire(now)
            waiting = self.store.connection.execute("SELECT count(*) FROM pairing_grants WHERE state='pending'").fetchone()[0]
            if waiting >= MAX_PENDING:
                raise AgentError("TOO_MANY_GRANTS", "Too many grants are waiting; cancel one or wait for it to expire", 429)
            for _ in range(8):
                grant_id, code, salt = _random(GRANT_ID_LENGTH), _random(CODE_LENGTH), secrets.token_hex(16)
                expires = now + timedelta(seconds=ttl_seconds)
                try:
                    self.store.connection.execute(
                        "INSERT INTO pairing_grants (grant_id, salt, code_hash, scopes, targets, device_id, replace_existing, created_at, expires_at, attempts_left, state) "
                        "VALUES (?,?,?,?,?,?,?,?,?,?,'pending')",
                        (grant_id, salt, _hash(salt, code), json.dumps(scopes), json.dumps(targets), device_id, int(replace), stamp(now), stamp(expires), ATTEMPTS))
                    break
                except sqlite3.IntegrityError:  # an id collision: try another
                    continue
            else:
                raise AgentError("PAIRING_UNAVAILABLE", "Could not create a grant", 503)
        return {"grant_id": grant_id, "package": encode_package(url, grant_id, code, fingerprint), "expires_at": stamp(expires),
                "ca_fingerprint": fingerprint.hex(), "url": url, "scopes": scopes, "targets": targets, "replace": replace, "device_id": device_id}

    def list(self) -> list[dict]:
        with self.store.lock:
            self._expire(utcnow())
            rows = self.store.connection.execute("SELECT * FROM pairing_grants ORDER BY created_at DESC, rowid DESC").fetchall()
        return [{"grant_id": row["grant_id"], "state": row["state"], "created_at": row["created_at"], "expires_at": row["expires_at"],
                 "scopes": json.loads(row["scopes"]), "targets": json.loads(row["targets"]), "replace": bool(row["replace_existing"]),
                 "device_id": row["device_id"], "redeemed_device_id": row["redeemed_device_id"], "redeemed_at": row["redeemed_at"],
                 "attempts_left": row["attempts_left"]} for row in rows]

    def cancel(self, grant_id: str) -> dict:
        if not re.fullmatch(ID_PATTERN, grant_id):
            raise AgentError("INVALID_ARGUMENT", "Invalid grant id", 400)
        with self.store.lock:
            self._expire(utcnow())
            row = self.store.connection.execute("SELECT state FROM pairing_grants WHERE grant_id=?", (grant_id,)).fetchone()
            if not row:
                raise AgentError("GRANT_NOT_FOUND", "No such grant", 404)
            if row["state"] != "pending":
                raise AgentError("GRANT_NOT_PENDING", "Only a waiting grant can be cancelled", 409)
            self.store.connection.execute("UPDATE pairing_grants SET state='cancelled' WHERE grant_id=? AND state='pending'", (grant_id,))
        return {"grant_id": grant_id, "state": "cancelled"}

    def redeem(self, grant_id: str, code: str, device_id: str) -> dict:
        """The only unauthenticated write. Every refusal is the same answer; nothing here says which check failed."""
        if not re.fullmatch(ID_PATTERN, grant_id) or not re.fullmatch(CODE_PATTERN, code):
            raise _reject()
        with self.store.lock:
            self._expire(utcnow())
            row = self.store.connection.execute("SELECT * FROM pairing_grants WHERE grant_id=?", (grant_id,)).fetchone()
            # Always compare, also for an id that does not exist, so the work is the same.
            matched = hmac.compare_digest(_hash(row["salt"] if row else DUMMY_SALT, code), row["code_hash"] if row else _hash(DUMMY_SALT, "0" * CODE_LENGTH))
            if row is None or row["state"] != "pending":
                raise _reject()  # expired, used, cancelled and burned grants cost no attempt
            pinned = row["device_id"]
            wrong_device = pinned is not None and not hmac.compare_digest(pinned.encode(), device_id.encode())
            if not matched or wrong_device:
                self.store.connection.execute(
                    "UPDATE pairing_grants SET attempts_left=attempts_left-1, state=CASE WHEN attempts_left-1<=0 THEN 'burned' ELSE state END WHERE grant_id=?", (grant_id,))
                raise _reject()
            replace = bool(row["replace_existing"])
            existing = self.store.connection.execute("SELECT revoked FROM devices WHERE device_id=?", (device_id,)).fetchone()
            if existing and not existing["revoked"] and not replace:
                raise _reject()  # a paired device needs replace in the grant; the grant is not consumed
            scopes, targets = json.loads(row["scopes"]), json.loads(row["targets"])
            # Use the grant first, then issue the token: a crash in between wastes a grant and never issues two tokens.
            with _transaction(self.store.connection):
                used = self.store.connection.execute(
                    "UPDATE pairing_grants SET state='used', redeemed_device_id=?, redeemed_at=? WHERE grant_id=? AND state='pending'",
                    (device_id, stamp(utcnow()), grant_id))
                if used.rowcount != 1:
                    raise _reject()
            try:
                token = self.store.pair(device_id, scopes, targets, replace=replace)
                pem = Path(self.settings.pairing_ca).read_text() if self.settings.pairing_ca else ""
            except (ValueError, OSError, sqlite3.Error) as error:
                raise AgentError("PAIRING_UNAVAILABLE", "The grant was used but no credential could be issued; create a new grant", 503) from error
        return {"agent_id": self.settings.agent_id, "token": token, "ca_pem": pem, "scopes": scopes, "targets": targets}


def install_pairing(app, settings, database, rates, admin) -> Pairing:
    pairing = Pairing(settings, database)
    app.state.pairing = pairing

    @app.post("/api/v1/admin/pairing-grants", status_code=201)
    async def create_grant(value: GrantCreate, session=Depends(admin)):
        return pairing.create(list(value.scopes), list(value.targets), value.ttl_seconds, value.replace, value.device_id)

    @app.get("/api/v1/admin/pairing-grants")
    async def list_grants(session=Depends(admin)):
        return {"grants": pairing.list()}

    @app.delete("/api/v1/admin/pairing-grants/{grant_id}")
    async def cancel_grant(grant_id: str, session=Depends(admin)):
        return pairing.cancel(grant_id)

    @app.post("/api/v1/pairing/redeem")
    async def redeem(request: Request, value: Redeem):
        if request.url.scheme != "https":
            raise AgentError("HTTPS_REQUIRED", "Pairing is only served over HTTPS", 400)
        try:
            rates.check("pairing:" + (request.client.host if request.client else "unknown"), 10)
            rates.check("pairing:*", 30)
        except AgentError:
            raise
        except Exception as error:  # a broken limiter refuses; it never lets a request through
            raise AgentError("PAIRING_UNAVAILABLE", "Pairing is unavailable", 503) from error
        return pairing.redeem(value.grant_id, value.code, value.device_id)

    return pairing
