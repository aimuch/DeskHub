from __future__ import annotations

import hashlib
import hmac
import json
from pathlib import Path
import secrets
import time
from datetime import timedelta
from contextlib import contextmanager
from uuid import uuid4

from fastapi import Depends, Request
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from starlette.concurrency import run_in_threadpool

from .admin_auth import AdminAuth, PolicyError
from .errors import AgentError
from .firmware_packages import MAX_PACKAGE_BYTES, key_fingerprint, trusted_key, validate_package, write_image
from .management_models import AdminLogin, ConfigurationWrite, DeviceConfiguration, DeviceReport, PasswordLogin, PasswordWrite, PowerReport, TERMINAL_UPDATE_STATES, UpdateCreate
from .models import stamp, utcnow

COOKIE = "deskhub_session"


@contextmanager
def transaction(connection):
    connection.execute("BEGIN IMMEDIATE")
    try:
        yield
        connection.execute("COMMIT")
    except BaseException:
        if connection.in_transaction:
            connection.execute("ROLLBACK")
        raise



class Management:
    def __init__(self, settings, store):
        self.settings, self.store = settings, store
        self.auth = AdminAuth(store)
        self.seen: dict[str, float] = {}
        self.sessions: dict[str, dict] = {}
        self.packages_dir = Path(settings.db_path).parent / "firmware"
        with store.lock:
            store.connection.executescript('''
                CREATE TABLE IF NOT EXISTS device_reports (
                    device_id TEXT PRIMARY KEY, report_json TEXT NOT NULL, last_seen TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS device_configurations (
                    device_id TEXT PRIMARY KEY, revision INTEGER NOT NULL, configuration_json TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS firmware_packages (
                    package_id TEXT PRIMARY KEY, payload_json TEXT NOT NULL, envelope_json TEXT NOT NULL,
                    key_fingerprint TEXT NOT NULL, created_at TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS update_jobs (
                    job_id TEXT PRIMARY KEY, device_id TEXT NOT NULL, package_id TEXT NOT NULL,
                    version TEXT NOT NULL, state TEXT NOT NULL, progress_percent INTEGER NOT NULL,
                    error_code TEXT, created_at TEXT NOT NULL, expires_at TEXT NOT NULL, completed_at TEXT
                );
                CREATE UNIQUE INDEX IF NOT EXISTS one_active_update_per_device ON update_jobs(device_id)
                    WHERE state IN ('queued','downloading','verifying','writing','rebooting','pending_confirmation');
            ''')

    @staticmethod
    def origin(request: Request, *, login=False):
        actual = request.headers.get("origin")
        expected = str(request.base_url).rstrip("/")
        if actual == expected:
            return
        # Local command-line callers can exchange a private admin credential.
        # Cookie-authenticated writes always require Origin plus CSRF.
        if login and actual is None and request.client and request.client.host in {"127.0.0.1", "::1", "testclient"}:
            return
        raise AgentError("ORIGIN_FORBIDDEN", "A same-origin management request is required", 403)

    def prune_sessions(self):
        now = time.monotonic()
        self.sessions = {key: value for key, value in self.sessions.items() if value["deadline"] > now}

    def token_hashes(self) -> list[str]:
        """Hashes of every management token, so that none of them can be chosen as the password."""
        with self.store.lock:
            rows = self.store.connection.execute("SELECT token_hash, scopes FROM devices").fetchall()
        return [row["token_hash"] for row in rows if "admin:config" in json.loads(row["scopes"])]

    def open_session(self, kind: str, *, admin_id: str, credential_hash: str | None = None, generation: int | None = None):
        """kind: "setup" (token, browser, no password yet: may only set the password), "password", or "script" (token, no Origin)."""
        self.prune_sessions()
        if len(self.sessions) >= 32:
            raise AgentError("RATE_LIMITED", "Too many active management sessions", 429, True)
        secret = secrets.token_urlsafe(32)
        csrf = secrets.token_urlsafe(32)
        expires = stamp(utcnow() + timedelta(minutes=30))
        self.sessions[hashlib.sha256(secret.encode()).hexdigest()] = {
            "kind": kind, "admin_id": admin_id, "credential_hash": credential_hash, "generation": generation,
            "csrf_token": csrf, "expires_at": expires, "deadline": time.monotonic() + 1800,
        }
        return secret, {"admin_id": admin_id, "csrf_token": csrf, "expires_at": expires, "setup_required": kind == "setup"}

    def login(self, request: Request, token: str):
        self.origin(request, login=True)
        script = request.headers.get("origin") is None   # origin() lets only a loopback command line come without one
        value = self.store.authenticate(token)
        self.store.authorize(value, "admin:config")
        credential = hashlib.sha256(token.encode()).hexdigest()
        if script:
            return self.open_session("script", admin_id=value["device_id"], credential_hash=credential)
        if self.auth.password_set():
            raise AgentError("TOKEN_LOGIN_DISABLED", "Sign in to the web console with the administrator password", 403)
        return self.open_session("setup", admin_id=value["device_id"], credential_hash=credential, generation=self.auth.generation())

    def password_session(self, generation: int):
        return self.open_session("password", admin_id="admin", generation=generation)

    def end_session(self, request: Request) -> None:
        self.sessions.pop(hashlib.sha256(request.cookies.get(COOKIE, "").encode()).hexdigest(), None)

    def session(self, request: Request, *, write=False, setup_ok=False):
        self.prune_sessions()
        cookie = request.cookies.get(COOKIE, "")
        key = hashlib.sha256(cookie.encode()).hexdigest()
        session = self.sessions.get(key)
        if not session:
            raise AgentError("UNAUTHORIZED", "Sign in to manage DeskHub", 401)
        kind = session["kind"]
        # A set, change or reset of the password ends every session of an older generation at once.
        if kind != "script" and session["generation"] != self.auth.generation():
            self.sessions.pop(key, None)
            raise AgentError("UNAUTHORIZED", "The administrator password changed; sign in again", 401)
        if kind != "password":   # a token session lives only as long as its token
            with self.store.lock:
                row = self.store.connection.execute("SELECT token_hash FROM devices WHERE device_id=? AND revoked=0", (session["admin_id"],)).fetchone()
            if not row or not hmac.compare_digest(row["token_hash"], session["credential_hash"]):
                raise AgentError("UNAUTHORIZED", "Management credential was revoked or rotated", 401)
            self.store.authorize(self.store.identity(session["admin_id"]), "admin:config")
        if kind == "setup" and not setup_ok:
            raise AgentError("PASSWORD_REQUIRED", "Set the administrator password first", 403)
        if write:
            self.origin(request)
            if not hmac.compare_digest(request.headers.get("X-CSRF-Token", ""), session["csrf_token"]):
                raise AgentError("CSRF_REJECTED", "The management CSRF token is missing or invalid", 403)
        return session

    def device(self, device_id: str) -> dict:
        with self.store.lock:
            self.expire_jobs(device_id)
            row = self.store.connection.execute("SELECT * FROM devices WHERE device_id=?", (device_id,)).fetchone()
            report_row = self.store.connection.execute("SELECT * FROM device_reports WHERE device_id=?", (device_id,)).fetchone()
            config_row = self.store.connection.execute("SELECT * FROM device_configurations WHERE device_id=?", (device_id,)).fetchone()
            job_row = self.store.connection.execute("SELECT * FROM update_jobs WHERE device_id=? ORDER BY created_at DESC, rowid DESC LIMIT 1", (device_id,)).fetchone()
        if not row or "admin:config" in json.loads(row["scopes"]):
            raise AgentError("DEVICE_NOT_FOUND", "A managed terminal with that identity was not found", 404)
        # Normalize additive schema-1 defaults on read as well as new requests.
        # Older persisted reports/drafts must compare equal to their current ACK.
        report = DeviceReport.model_validate_json(report_row["report_json"]).model_dump() if report_row else None
        desired = DeviceConfiguration.model_validate_json(config_row["configuration_json"]).model_dump() if config_row else None
        revision = config_row["revision"] if config_row else 0
        applied = report["applied_revision"] if report else 0
        error = report["config_error"] if report else None
        configuration_state = "unavailable" if report is None else "failed" if error else "applied"
        if desired is not None:
            if applied < revision:
                configuration_state = "failed" if error else "pending"
            elif applied == revision and report and report["configuration"] != desired:
                configuration_state = "failed"
                error = error or "CONFIGURATION_DIFFERS"
            elif applied > revision:
                configuration_state = "unavailable"
                error = error or "REMOTE_REVISION_AHEAD"
        online = not row["revoked"] and time.monotonic() - self.seen.get(device_id, -1e9) <= 30
        # Readings only count while the device is online. A stale record keeps when it was last heard from, never numbers.
        power = report["power"] if report else PowerReport().model_dump()
        power_state = "unreported" if report is None else "fresh" if online else "stale"
        if power_state == "stale":
            power = {**power, "soc_percent": None, "battery_mv": None, "vbus_mv": None, "chip_temp_c": None, "measurement": "not_measured"}
        targets = json.loads(row["targets"])
        writable = "control:displays" in json.loads(row["scopes"])
        allowed_actions = [{"action_id": action.action_id, "label": action.label, "target_id": action.target_id}
                          for action in self.settings.actions if action.enabled and writable and ("*" in targets or action.target_id in targets)]
        return {
            "device_id": device_id, "revoked": bool(row["revoked"]),
            "scopes": json.loads(row["scopes"]), "targets": targets, "paired_at": row["created_at"],
            "online": online,
            "last_seen": report_row["last_seen"] if report_row else None,
            "firmware_version": report["firmware_version"] if report else None,
            "board_model": report["board_model"] if report else None,
            "boot_id": report["boot_id"] if report else None,
            "uptime_seconds": report["uptime_seconds"] if report else None,
            "free_heap_bytes": report["free_heap_bytes"] if report else None,
            "power": power, "power_state": power_state, "power_reported_at": report_row["last_seen"] if report_row else None,
            "capabilities": report["capabilities"] if report else None,
            "reported_configuration": report["configuration"] if report else None,
            "desired_revision": revision, "applied_revision": applied,
            "desired_configuration": desired, "config_error": error, "configuration_state": configuration_state,
            "actions": allowed_actions, "update": dict(job_row) if job_row else None,
        }

    def devices(self, offset: int = 0):
        with self.store.lock:
            identifiers = [row[0] for row in self.store.connection.execute("SELECT device_id FROM devices ORDER BY device_id")]
        result = []
        # Keep the shared 16 KiB snapshot contract even with long action labels.
        next_offset = None
        for index in range(offset, len(identifiers)):
            try:
                device = self.device(identifiers[index])
            except AgentError as error:
                if error.code == "DEVICE_NOT_FOUND":
                    continue
                raise
            if result and (len(result) >= 8 or len(json.dumps(result + [device], ensure_ascii=False).encode()) > 14000):
                next_offset = index
                break
            result.append(device)
        return {"devices": result, "next_offset": next_offset}

    def set_configuration(self, device_id: str, value: ConfigurationWrite):
        device = self.device(device_id)
        if device["revoked"]:
            raise AgentError("DEVICE_REVOKED", "Re-pair this terminal before changing its configuration", 409)
        permitted = {action["action_id"] for action in device["actions"]}
        if not set(value.configuration.layout.action_ids) <= permitted:
            raise AgentError("FORBIDDEN", "Layout can only use actions authorized for this terminal", 403)
        supported = device.get("capabilities")
        if supported and value.configuration.layout.preset not in supported["layouts"]:
            raise AgentError("UNSUPPORTED", "This terminal does not support the selected layout", 409)
        with self.store.lock:
            self.store.connection.execute("BEGIN IMMEDIATE")
            try:
                previous = self.store.connection.execute("SELECT revision FROM device_configurations WHERE device_id=?", (device_id,)).fetchone()
                revision = previous[0] if previous else 0
                if revision != value.expected_revision:
                    raise AgentError("REVISION_CONFLICT", "Configuration changed; refresh before saving again", 409)
                revision = max(revision, device["applied_revision"]) + 1
                self.store.connection.execute("INSERT OR REPLACE INTO device_configurations VALUES(?,?,?,?)",
                    (device_id, revision, value.configuration.model_dump_json(), stamp()))
                self.store.connection.execute("COMMIT")
            except BaseException:
                if self.store.connection.in_transaction:
                    self.store.connection.execute("ROLLBACK")
                raise
        return self.device(device_id)

    def add_package(self, raw: bytes):
        payload, envelope, image, fingerprint = validate_package(raw, self.settings.firmware_public_key)
        package_id = payload["image_sha256"]
        with self.store.lock:
            row = self.store.connection.execute("SELECT key_fingerprint FROM firmware_packages WHERE package_id=?", (package_id,)).fetchone()
            if row and row[0] != fingerprint:
                raise AgentError("FIRMWARE_TRUST_CONFLICT", "This image is already stored under a different signing key", 409)
            if not row and self.store.connection.execute("SELECT count(*) FROM firmware_packages").fetchone()[0] >= 20:
                raise AgentError("FIRMWARE_CAPACITY_REACHED", "At most 20 firmware packages may be retained", 409)
            write_image(self.packages_dir / (package_id + ".bin"), image)
            self.store.connection.execute("INSERT OR IGNORE INTO firmware_packages VALUES(?,?,?,?,?)",
                (package_id, json.dumps(payload), json.dumps(envelope), fingerprint, stamp()))
        return self.package(package_id)

    def package(self, package_id):
        with self.store.lock:
            row = self.store.connection.execute("SELECT * FROM firmware_packages WHERE package_id=?", (package_id,)).fetchone()
        if not row:
            raise AgentError("FIRMWARE_NOT_FOUND", "The signed firmware package was not found", 404)
        payload = json.loads(row["payload_json"])
        return {"package_id": package_id, "version": payload["version"], "board_model": payload["board_model"],
                "image_size": payload["image_size"], "image_sha256": payload["image_sha256"],
                "created_at": row["created_at"], "signature_verified": True, "key_fingerprint": row["key_fingerprint"]}

    def packages(self):
        with self.store.lock:
            identifiers = [row[0] for row in self.store.connection.execute("SELECT package_id FROM firmware_packages ORDER BY created_at DESC LIMIT 20")]
        return [self.package(identifier) for identifier in identifiers]

    def create_update(self, device_id: str, value: UpdateCreate):
        device = self.device(device_id)
        package = self.package(value.package_id)
        if not device["online"]:
            raise AgentError("DEVICE_OFFLINE", "The terminal must have reported within 30 seconds", 409)
        if device["firmware_version"] != value.expected_firmware_version:
            raise AgentError("FIRMWARE_REVISION_CONFLICT", "Terminal version changed; refresh before updating", 409)
        if device["firmware_version"] == package["version"]:
            raise AgentError("ALREADY_INSTALLED", "The terminal already reports this firmware version", 409)
        if device["power"]["vbus"] != "present" or device["power"]["fault"]:
            raise AgentError("UNSAFE_UPDATE_POWER", "A recent fault-free USB power report is required", 409)
        capabilities = device["capabilities"]
        if not capabilities or not capabilities["signed_ota"]:
            raise AgentError("UNSUPPORTED", "This terminal does not report signed OTA capability", 409)
        current_fingerprint = key_fingerprint(trusted_key(self.settings.firmware_public_key))
        if capabilities["update_key_fingerprint"] != package["key_fingerprint"] or current_fingerprint != package["key_fingerprint"]:
            raise AgentError("UPDATE_TRUST_MISMATCH", "Agent, package and terminal must use the same independently provisioned update key", 409)
        with self.store.lock:
            self.expire_jobs(device_id)
            active = self.store.connection.execute("SELECT 1 FROM update_jobs WHERE device_id=? AND state IN ('queued','downloading','verifying','writing','rebooting','pending_confirmation')", (device_id,)).fetchone()
            if active:
                raise AgentError("BUSY", "A firmware update is already active for this terminal", 409)
            job_id = "upd-" + uuid4().hex
            self.store.connection.execute("INSERT INTO update_jobs VALUES(?,?,?,?,?,?,?,?,?,?)",
                (job_id, device_id, value.package_id, package["version"], "queued", 0, None, stamp(), stamp(utcnow() + timedelta(minutes=5)), None))
            row = self.store.connection.execute("SELECT * FROM update_jobs WHERE job_id=?", (job_id,)).fetchone()
        return dict(row)

    def expire_jobs(self, device_id):
        # Only queued jobs expire automatically. Once writing may have begun,
        # timeout cannot establish whether the device changed its boot partition.
        self.store.connection.execute("UPDATE update_jobs SET state='failed', error_code='UPDATE_EXPIRED', completed_at=? WHERE device_id=? AND state='queued' AND expires_at < ?", (stamp(), device_id, stamp()))

    def sync(self, identity, report: DeviceReport):
        self.store.authorize(identity, "device:sync")
        if "admin:config" in identity["scopes"]:
            raise AgentError("FORBIDDEN", "Use an independently paired terminal identity to report device state", 403)
        device_id = identity["device_id"]
        with self.store.lock, transaction(self.store.connection):
            self.expire_jobs(device_id)
            if report.update:
                job = self.store.connection.execute("SELECT * FROM update_jobs WHERE job_id=?", (report.update.job_id,)).fetchone()
                if job and job["device_id"] != device_id:
                    raise AgentError("UPDATE_JOB_NOT_FOUND", "This update job does not belong to the reporting terminal", 404)
                if not job and report.update.state not in TERMINAL_UPDATE_STATES:
                    raise AgentError("UPDATE_JOB_NOT_FOUND", "An active update must have an assigned Agent job", 404)
                # A restored/empty Agent database may no longer know a completed
                # device journal. Keep accepting telemetry/configuration, but do
                # not manufacture an Agent success record for that old job.
                if job and job["state"] not in TERMINAL_UPDATE_STATES:
                    state = report.update.state
                    if state == "succeeded" and report.firmware_version != job["version"]:
                        raise AgentError("UPDATE_CONFIRMATION_MISMATCH", "The confirmed firmware version differs from the assigned update", 409)
                    # A duplicated queued report never rewinds an in-progress job.
                    # Verification also occurs after writing the complete image.
                    # A total ordering would hide that real device phase.
                    if state == "queued" and job["state"] != "queued":
                        state = job["state"]
                    progress = max(job["progress_percent"], report.update.progress_percent)
                    self.store.connection.execute("UPDATE update_jobs SET state=?,progress_percent=?,error_code=?,completed_at=? WHERE job_id=?",
                        (state, 100 if state == "succeeded" else progress, report.update.error_code,
                         stamp() if state in TERMINAL_UPDATE_STATES else None, job["job_id"]))
            self.store.connection.execute("INSERT OR REPLACE INTO device_reports VALUES(?,?,?)", (device_id, report.model_dump_json(), stamp()))
            self.seen[device_id] = time.monotonic()
            self.expire_jobs(device_id)
            config = self.store.connection.execute("SELECT * FROM device_configurations WHERE device_id=?", (device_id,)).fetchone()
            job = self.store.connection.execute("SELECT * FROM update_jobs WHERE device_id=? AND state IN ('queued','downloading','verifying','writing','rebooting','pending_confirmation') ORDER BY created_at DESC LIMIT 1", (device_id,)).fetchone()
            update = None
            if job:
                package = self.store.connection.execute("SELECT envelope_json FROM firmware_packages WHERE package_id=?", (job["package_id"],)).fetchone()
                if package:
                    update = {"job_id": job["job_id"], "package_id": job["package_id"], "manifest": json.loads(package[0]),
                              "image_path": f"/api/v1/device/firmware/{job['package_id']}/image", "expires_at": job["expires_at"]}
        desired = {"revision": config["revision"], "configuration": DeviceConfiguration.model_validate_json(config["configuration_json"]).model_dump()} if config and report.applied_revision < config["revision"] else None
        return {"agent_id": self.settings.agent_id, "desired": desired, "update": update}

    def image(self, identity, package_id):
        self.store.authorize(identity, "device:sync")
        if "admin:config" in identity["scopes"]:
            raise AgentError("FORBIDDEN", "A terminal identity is required", 403)
        with self.store.lock:
            self.expire_jobs(identity["device_id"])
            job = self.store.connection.execute("SELECT * FROM update_jobs WHERE device_id=? AND package_id=? AND state IN ('queued','downloading','verifying','writing') ORDER BY created_at DESC LIMIT 1", (identity["device_id"], package_id)).fetchone()
        if not job:
            raise AgentError("FORBIDDEN", "No authorized active update assigns this image to this terminal", 403)
        package = self.package(package_id)
        path = self.packages_dir / (package["package_id"] + ".bin")
        if not path.is_file() or path.stat().st_size != package["image_size"]:
            raise AgentError("FIRMWARE_STORAGE_FAULT", "The retained firmware image is unavailable", 503)
        with self.store.lock:
            self.store.connection.execute("UPDATE update_jobs SET state='downloading' WHERE job_id=? AND state='queued'", (job["job_id"],))
        return path


def install_management(app, settings, database, identity, rates):
    management = Management(settings, database)
    app.state.management = management

    def check(request: Request, *, setup_ok: bool):
        rates.check("admin:" + hashlib.sha256(request.cookies.get(COOKIE, "").encode()).hexdigest(), 120)
        return management.session(request, write=request.method not in {"GET", "HEAD"}, setup_ok=setup_ok)

    async def admin(request: Request):
        # Every page of the console. A session that still has to set the password does not get in.
        return check(request, setup_ok=False)

    async def admin_setup(request: Request):
        # Only the session itself and the password: the three routes that a first-time session may use.
        return check(request, setup_ok=True)

    def start(request: Request, cookie_and_result):
        cookie, result = cookie_and_result
        response = JSONResponse(result)
        response.set_cookie(COOKIE, cookie, max_age=1800, httponly=True, secure=request.url.scheme == "https", samesite="strict", path="/api/v1/admin")
        return response

    def caller(request: Request) -> str:
        return request.client.host if request.client else "unknown"

    @app.get("/api/v1/admin/auth")
    async def auth_state():
        return {"password_set": management.auth.password_set()}

    @app.post("/api/v1/admin/session")
    async def login(request: Request, value: AdminLogin):
        rates.check("login:" + caller(request), 10)
        return start(request, management.login(request, value.token))

    @app.post("/api/v1/admin/login")
    async def password_login(request: Request, value: PasswordLogin):
        management.origin(request)
        rates.check("password:" + caller(request), 10)
        generation = await run_in_threadpool(management.auth.verify, value.password)
        return start(request, management.password_session(generation))

    @app.put("/api/v1/admin/password")
    async def set_password(request: Request, value: PasswordWrite, session=Depends(admin_setup)):
        if session["kind"] == "script":
            raise AgentError("FORBIDDEN", "Set the password in the web console, not with a credential file", 403)
        hashes = management.token_hashes()
        try:
            if session["kind"] == "setup":
                generation = await run_in_threadpool(lambda: management.auth.set_password(value.password, token_hashes=hashes, only_if_unset=True))
            elif value.current_password is None:
                raise AgentError("CURRENT_PASSWORD_REQUIRED", "Enter the current password to change it", 400)
            else:
                generation = await run_in_threadpool(lambda: management.auth.change_password(value.current_password, value.password, token_hashes=hashes))
        except PolicyError as error:
            raise AgentError("WEAK_PASSWORD", error.message, 400) from None
        # New rights get a new cookie and a new CSRF token. The old session ends, and so does every other one.
        management.end_session(request)
        return start(request, management.password_session(generation))

    @app.get("/api/v1/admin/session")
    async def session(value=Depends(admin_setup)):
        return {"admin_id": value["admin_id"], "csrf_token": value["csrf_token"], "expires_at": value["expires_at"],
                "setup_required": value["kind"] == "setup"}

    @app.delete("/api/v1/admin/session")
    async def logout(request: Request, value=Depends(admin_setup)):
        management.end_session(request)
        response = JSONResponse({"logged_out": True})
        response.delete_cookie(COOKIE, path="/api/v1/admin")
        return response

    @app.get("/api/v1/admin/overview")
    async def overview(value=Depends(admin)):
        from . import __version__
        with database.lock:
            terminal_rows = [row for row in database.connection.execute("SELECT device_id, scopes, revoked FROM devices")
                             if "admin:config" not in json.loads(row["scopes"])]
            online_count = sum(not row["revoked"] and time.monotonic() - management.seen.get(row["device_id"], -1e9) <= 30 for row in terminal_rows)
            package_count = database.connection.execute("SELECT count(*) FROM firmware_packages").fetchone()[0]
            job_count = database.connection.execute("SELECT count(*) FROM update_jobs").fetchone()[0]
        return {"agent_id": settings.agent_id, "agent_version": __version__, "name": settings.name,
                "device_count": len(terminal_rows), "online_count": online_count,
                "package_count": package_count, "update_count": job_count,
                "backend": getattr(app.state, "backend_health", {"availability": "unavailable"}),
                "firmware_trust_configured": bool(settings.firmware_public_key),
                "uptime_seconds": time.monotonic() - app.state.started, "boot_id": app.state.boot_id,
                "listen": {"bind": settings.bind, "port": settings.port, "tls": bool(settings.tls_cert), "lan_enabled": settings.lan_enabled},
                "action_count": len(settings.actions), "display_count": len(settings.displays)}

    @app.get("/api/v1/admin/host")
    async def host(value=Depends(admin)):
        return app.state.metrics.read()

    @app.get("/api/v1/admin/displays")
    async def admin_displays(value=Depends(admin)):
        ready, reason = app.state.backend.availability()
        snapshots = app.state.display_snapshots
        # Binding identifiers (UUID, serial, registry path) never leave the Agent.
        return {"backend": {**getattr(app.state, "backend_health", {"availability": "unavailable"}),
                            "configured": settings.backend != "disabled", "ready": ready, "reason_code": reason},
                "displays": [{**snapshots.read(display), "compatibility_verified": display.compatibility_verified,
                              "readback_enabled": display.readback_enabled, "input_ids": list(display.inputs)}
                             for display in settings.displays],
                "actions": [{"action_id": action.action_id, "label": action.label, "target_id": action.target_id,
                             "input_id": action.input_id, "enabled": action.enabled, "shortcut": action.shortcut}
                            for action in settings.actions]}

    @app.get("/api/v1/admin/commands")
    async def admin_commands(limit: int = 20, value=Depends(admin)):
        if not 1 <= limit <= 30:
            raise AgentError("INVALID_ARGUMENT", "limit must be between 1 and 30", 400)
        items = database.recent(limit)
        # The shared 16 KiB response contract also applies to this view.
        truncated = False
        while items and len(json.dumps({"commands": items}, ensure_ascii=False).encode()) > 14000:
            items.pop()
            truncated = True
        return {"commands": items, "truncated": truncated}

    @app.get("/api/v1/admin/devices")
    async def devices(offset: int = 0, value=Depends(admin)):
        if offset < 0 or offset > 100000:
            raise AgentError("INVALID_ARGUMENT", "Invalid device page", 400)
        return management.devices(offset)

    @app.get("/api/v1/admin/devices/{device_id}")
    async def device(device_id: str, value=Depends(admin)):
        return management.device(device_id)

    @app.put("/api/v1/admin/devices/{device_id}/configuration")
    async def configure(device_id: str, value: ConfigurationWrite, session=Depends(admin)):
        return management.set_configuration(device_id, value)

    @app.post("/api/v1/admin/devices/{device_id}/revoke")
    async def revoke(device_id: str, value=Depends(admin)):
        management.device(device_id)
        database.revoke(device_id)
        return management.device(device_id)

    @app.get("/api/v1/admin/firmware")
    async def packages(value=Depends(admin)):
        return {"packages": management.packages()}

    @app.post("/api/v1/admin/firmware", status_code=201)
    async def upload(request: Request, value=Depends(admin)):
        if request.headers.get("content-type", "").split(";", 1)[0] != "application/zip":
            raise AgentError("INVALID_ARGUMENT", "Upload a signed application/zip package", 400)
        return management.add_package(await request.body())

    @app.post("/api/v1/admin/devices/{device_id}/updates", status_code=202)
    async def update(device_id: str, value: UpdateCreate, session=Depends(admin)):
        return management.create_update(device_id, value)

    @app.post("/api/v1/device/sync")
    async def sync(value: DeviceReport, device=Depends(identity)):
        return management.sync(device, value)

    @app.get("/api/v1/device/firmware/{package_id}/image")
    async def image(package_id: str, device=Depends(identity)):
        path = management.image(device, package_id)
        return FileResponse(path, media_type="application/octet-stream", filename="app.bin")

    from .pairing import install_pairing  # imported here: pairing imports management_models, and this file imports the admin dependency
    install_pairing(app, settings, database, rates, admin)

    # Only this checked-in package directory is served; no user-configured path.
    directory = Path(__file__).with_name("web")
    if directory.is_dir():
        app.mount("/manage", StaticFiles(directory=directory, html=True), name="management-ui")
    return management
