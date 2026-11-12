from __future__ import annotations

import asyncio
from collections import OrderedDict, deque
from contextlib import asynccontextmanager
import fcntl
import ipaddress
import logging
import os
from pathlib import Path
import time
from uuid import uuid4

from fastapi import Depends, FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from . import __version__
from .backend import Backend
from .errors import AgentError
from .metrics import HostMetrics
from .models import ActionsResponse, CapabilitiesResponse, CommandRequest, CommandResult, DisplaysResponse, ErrorResponse, HostsResponse, Settings, Status, stamp
from .scheduler import Scheduler
from .storage import Store
from .snapshots import DisplaySnapshots
from .management import install_management
from .firmware_packages import MAX_PACKAGE_BYTES

logger = logging.getLogger("deskhub.api")


class RateLimits:
    def __init__(self):
        self.buckets: OrderedDict[str, deque] = OrderedDict()

    def check(self, key: str, maximum: int = 120):
        now = time.monotonic()
        bucket = self.buckets.setdefault(key, deque())
        self.buckets.move_to_end(key)
        while bucket and now - bucket[0] >= 60:
            bucket.popleft()
        if len(bucket) >= maximum:
            raise AgentError("RATE_LIMITED", "Request rate exceeded; retry after 60 seconds", 429, True)
        bucket.append(now)
        while len(self.buckets) > 256:
            self.buckets.popitem(last=False)


def create_app(settings: Settings, *, backend: Backend | None = None, store: Store | None = None) -> FastAPI:
    database = store or Store(settings.db_path)
    adapter = backend or Backend(settings)
    scheduler = Scheduler(settings, database, adapter)
    metrics = HostMetrics(settings.host_id)
    display_snapshots = DisplaySnapshots(settings, adapter)
    rates = RateLimits()
    boot_id = f"boot-{uuid4().hex}"
    started = time.monotonic()
    networks = [ipaddress.ip_network(value, strict=False) for value in settings.allowed_networks]

    @asynccontextmanager
    async def lifespan(app):
        lock_path = Path(settings.db_path).with_suffix(".lock")
        lock_fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
        background_tasks = []
        try:
            try:
                fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise RuntimeError("another Agent already owns this database; use one process/worker") from error
            database.recover()
            database.cleanup(settings.retention_hours)
            metrics.collect()
            task = asyncio.create_task(metrics.run())
            background_tasks.append(task)
            app.state.backend_health = await adapter.probe()
            display_task = asyncio.create_task(display_snapshots.run())
            background_tasks.append(display_task)
            async def cleanup_history():
                next_cleanup = time.monotonic() + 3600
                while True:
                    await asyncio.sleep(30)
                    app.state.backend_health = await adapter.probe()
                    if time.monotonic() >= next_cleanup:
                        database.cleanup(settings.retention_hours)
                        next_cleanup = time.monotonic() + 3600
            cleanup_task = asyncio.create_task(cleanup_history())
            background_tasks.append(cleanup_task)
            yield
        finally:
            await scheduler.stop()
            for pending_task in background_tasks:
                pending_task.cancel()
            await asyncio.gather(*background_tasks, return_exceptions=True)
            os.close(lock_fd)
            if store is None:
                database.close()

    app = FastAPI(title="DeskHub Agent", version=__version__, lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None,
                  responses={400: {"model": ErrorResponse}, 401: {"model": ErrorResponse}, 403: {"model": ErrorResponse},
                             409: {"model": ErrorResponse}, 413: {"model": ErrorResponse}, 429: {"model": ErrorResponse}})
    app.state.store, app.state.scheduler, app.state.backend = database, scheduler, adapter
    app.state.metrics, app.state.display_snapshots, app.state.boot_id, app.state.started = metrics, display_snapshots, boot_id, started

    @app.exception_handler(AgentError)
    async def agent_error(request, error):
        return JSONResponse(error.body(getattr(request.state, "request_id", None)), status_code=error.status)

    @app.exception_handler(RequestValidationError)
    async def invalid_request(request, error):
        # Pydantic errors include original inputs; never return secret/body fragments.
        return JSONResponse(AgentError("INVALID_ARGUMENT", "Request does not match the protocol schema", 400).body(
            getattr(request.state, "request_id", None)), status_code=400)

    @app.middleware("http")
    async def guard(request: Request, call_next):
        request.state.request_id = f"req-{uuid4().hex}"
        try:
            remote = request.client.host if request.client else ""
            try:
                remote_ip = ipaddress.ip_address(remote)
            except ValueError:
                # ASGI TestClient's synthetic peer; never accepted in production.
                if remote != "testclient":
                    raise AgentError("FORBIDDEN", "Client address is unavailable", 403)
                remote_ip = ipaddress.ip_address("127.0.0.1")
            if not any(remote_ip in network for network in networks):
                raise AgentError("FORBIDDEN", "Client network is not allowed", 403)
            if request.url.path == "/healthz" and not remote_ip.is_loopback:
                raise AgentError("FORBIDDEN", "Health check is local only", 403)
            if not remote_ip.is_loopback and request.url.scheme != "https":
                raise AgentError("TLS_REQUIRED", "LAN access requires HTTPS", 403)
            rates.check(f"ip:{remote_ip}", 240)
            if request.method in {"POST", "PUT", "PATCH"}:
                uploading = request.url.path == "/api/v1/admin/firmware" and request.method == "POST"
                if uploading:
                    app.state.management.session(request, write=True)
                maximum = MAX_PACKAGE_BYTES if uploading else 4096
                try:
                    length = int(request.headers.get("content-length", "0"))
                except ValueError:
                    raise AgentError("INVALID_ARGUMENT", "Invalid content length", 400)
                if length < 0:
                    raise AgentError("INVALID_ARGUMENT", "Invalid content length", 400)
                if length > maximum:
                    raise AgentError("PAYLOAD_TOO_LARGE", "Request body exceeds the configured limit", 413)
                body = bytearray()
                async for chunk in request.stream():
                    if len(body) + len(chunk) > maximum:
                        raise AgentError("PAYLOAD_TOO_LARGE", "Request body exceeds the configured limit", 413)
                    body.extend(chunk)
                request._body = bytes(body)
            response = await call_next(request)
            response.headers["X-Request-ID"] = request.state.request_id
            response.headers["Cache-Control"] = "no-store"
            response.headers["X-Content-Type-Options"] = "nosniff"
            response.headers["X-Frame-Options"] = "DENY"
            response.headers["Content-Security-Policy"] = "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"
            # Firmware bytes are streamed only by an authenticated, job-scoped
            # route. Checked-in static UI assets have their own fixed root.
            binary = (request.url.path.startswith("/api/v1/device/firmware/") and request.url.path.endswith("/image")
                      and response.status_code == 200 and response.headers.get("content-type", "").startswith("application/octet-stream"))
            if binary or request.url.path == "/manage" or request.url.path.startswith("/manage/"):
                return response
            # Responses are bounded before sending any bytes. This avoids returning
            # truncated JSON or requiring the ESP32 to allocate arbitrary snapshots.
            content = bytearray()
            async for chunk in response.body_iterator:
                content.extend(chunk)
                if len(content) > 16384:
                    raise AgentError("SNAPSHOT_TOO_LARGE", "Response exceeds 16 KiB; narrow configured resources", 503)
            from starlette.responses import Response
            response = Response(bytes(content), status_code=response.status_code, headers=dict(response.headers), media_type=response.media_type)
            response.headers["X-Request-ID"] = request.state.request_id
            response.headers["Cache-Control"] = "no-store"
            return response
        except AgentError as error:
            return JSONResponse(error.body(request.state.request_id), status_code=error.status)

    async def identity(request: Request) -> dict:
        header = request.headers.get("Authorization", "")
        if not header.startswith("Bearer "):
            raise AgentError("UNAUTHORIZED", "A paired device credential is required", 401)
        value = database.authenticate(header[7:])
        rates.check(f"device:{value['device_id']}")
        return value

    async def reader(value=Depends(identity)):
        database.authorize(value, "read:status")
        return value

    def permitted(value: dict, target: str) -> bool:
        return "*" in value["targets"] or target in value["targets"]

    @app.get("/healthz")
    async def health():
        return {"alive": True}

    @app.get("/api/v1/status", response_model=Status)
    async def status(value=Depends(reader)):
        return {"agent_id": settings.agent_id, "agent_version": __version__, "protocol_version": "1.0", "boot_id": boot_id,
                "name": settings.name, "health": "ok", "server_time": stamp(), "uptime_seconds": time.monotonic() - started}

    @app.get("/api/v1/capabilities", response_model=CapabilitiesResponse)
    async def capabilities(value=Depends(reader)):
        ready, reason = adapter.availability()
        return {"schema_version": 1, "capabilities": {
            "host_metrics": {"support": "supported", "availability": "ready", "reason_code": None, "constraints": []},
            "display_set_input": {"support": "unverified" if ready else "unsupported", "availability": "ready" if ready else "unavailable",
                                  "reason_code": reason, "constraints": ["Per-monitor compatibility and return path require G0 evidence"]},
            "gateway": {"support": "unsupported", "availability": "unavailable", "reason_code": "NOT_IMPLEMENTED", "constraints": []}},
            "limits": {"request_bytes": 4096, "snapshot_bytes": 16384, "hosts": 4, "displays": 4, "actions": 12},
            "backend": getattr(app.state, "backend_health", {"availability": "unavailable"})}

    @app.get("/api/v1/hosts", response_model=HostsResponse)
    async def hosts(value=Depends(reader)):
        return {"schema_version": 1, "hosts": [metrics.read()]}

    @app.get("/api/v1/actions", response_model=ActionsResponse)
    async def actions(value=Depends(reader)):
        writable = "control:displays" in value["scopes"]
        return {"schema_version": 1, "actions": [
            {"action_id": action.action_id, "label": action.label, "target_id": action.target_id,
             "type": "display.set_input", "parameters": {"input_id": action.input_id}, "enabled": action.enabled}
            for action in settings.actions if writable and action.enabled and permitted(value, action.target_id)]}

    @app.get("/api/v1/displays", response_model=DisplaysResponse)
    async def displays(value=Depends(reader)):
        return {"schema_version": 1, "displays": [display_snapshots.read(display) for display in settings.displays
                                                 if permitted(value, display.monitor_id)]}

    @app.post("/api/v1/commands", status_code=202, response_model=CommandResult)
    async def submit(command: CommandRequest, value=Depends(identity)):
        return await scheduler.submit(value, command)

    @app.get("/api/v1/commands", response_model=CommandResult)
    async def find_command(idempotency_key: str, value=Depends(identity)):
        return database.query(value, key=idempotency_key)

    @app.get("/api/v1/commands/{command_id}", response_model=CommandResult)
    async def command(command_id: str, value=Depends(identity)):
        return database.query(value, command_id=command_id)

    app.state.rates = rates
    install_management(app, settings, database, identity, rates)
    return app
