from __future__ import annotations

import argparse
import asyncio
import contextlib
from datetime import timedelta
import json
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import platform
import stat
import sys
import time
from uuid import uuid4

import httpx

from . import __version__
from .backend import Backend
from .config import initialize, load_settings, private_write
from .models import CommandRequest, SCOPES, Settings, stamp, utcnow
from .storage import Store


def private_json(path: Path) -> dict:
    if path.stat().st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        raise ValueError("credential file must have permissions 0600 or stricter")
    if path.stat().st_size > 65536:
        raise ValueError("credential file is too large")
    return json.loads(path.read_text())


def client(args, settings):
    credentials = private_json(Path(args.credentials))
    endpoint = args.url or credentials.get("agent_url")
    if not endpoint:
        raise ValueError("set --url or agent_url in the credential file")
    url = httpx.URL(endpoint)
    import ipaddress
    try:
        loopback = ipaddress.ip_address(url.host).is_loopback
    except ValueError:
        loopback = url.host == "localhost"
    if url.userinfo or url.query or url.fragment or (url.scheme != "https" and not (loopback and url.scheme == "http")):
        raise ValueError("use an HTTPS Agent URL (HTTP is permitted only on loopback)")
    if url.path not in {"", "/"}:
        raise ValueError("Agent URL must not include a path")
    verify = str(args.ca) if args.ca else credentials.get("ca_file") or True
    import ssl
    if isinstance(verify, str):
        verify = ssl.create_default_context(cafile=verify)
    connection = httpx.Client(base_url=str(url).rstrip("/"), headers={"Authorization": f"Bearer {credentials['token']}"},
        verify=verify, timeout=httpx.Timeout(5, connect=2), trust_env=False, follow_redirects=False)
    status = connection.get("/api/v1/status")
    status.raise_for_status()
    expected = credentials.get("agent_id")
    if expected and status.json()["agent_id"] != expected:
        connection.close()
        raise ValueError("Agent identity differs from the paired identity")
    return connection, status.json()


class PrivateRotatingHandler(RotatingFileHandler):
    def _open(self):
        fd = os.open(self.baseFilename, os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
        os.chmod(self.baseFilename, 0o600)
        return os.fdopen(fd, "a", encoding=self.encoding)


def configure_logs(settings):
    directory = Path(settings.db_path).parent
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    path = directory / "agent.log"
    fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
    os.close(fd)
    os.chmod(path, 0o600)
    handler = PrivateRotatingHandler(path, maxBytes=1024 * 1024, backupCount=3, encoding="utf-8")
    formatter = logging.Formatter("%(asctime)sZ %(levelname)s %(name)s %(message)s")
    formatter.converter = time.gmtime
    handler.setFormatter(formatter)
    logger = logging.getLogger("deskhub")
    logger.setLevel(logging.INFO)
    logger.addHandler(handler)
    logger.info("agent_id=%s version=%s platform=%s startup", settings.agent_id, __version__, platform.system())


def build_parser():
    parser = argparse.ArgumentParser(description="DeskHub local administration and authenticated control")
    parser.add_argument("--config", type=Path, default=Path.home() / ".config/deskhub/agent.json")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("init", help="create private default configuration (loopback, no display backend)")
    sub.add_parser("serve", help="run in foreground; Ctrl-C stops the Agent")
    pair = sub.add_parser("pair", help="issue one device credential to a private file")
    pair.add_argument("device_id")
    pair.add_argument("--scope", action="append", choices=sorted(SCOPES), default=None)
    pair.add_argument("--target", action="append", default=None)
    pair.add_argument("--output", type=Path, required=True)
    pair.add_argument("--replace", action="store_true")
    admin_token = sub.add_parser("admin-token", help="issue a private, independently revocable browser management credential")
    admin_token.add_argument("--output", type=Path, required=True)
    admin_token.add_argument("--name", default="browser-admin")
    admin_token.add_argument("--replace", action="store_true")
    revoke = sub.add_parser("revoke")
    revoke.add_argument("device_id")
    sub.add_parser("devices")
    sub.add_parser("list-displays", help="read-only real backend inventory; no switching")
    sub.add_parser("diagnostics", help="print a redacted local diagnostic summary")
    sub.add_parser("export-openapi", help="print the API schema without starting a listener")
    shortcut = sub.add_parser("shortcut", help="validate/store an external shortcut mapping")
    shortcut.add_argument("action_id")
    shortcut.add_argument("--key")
    shortcut.add_argument("--disable", action="store_true")
    for name in ("status", "actions", "run", "result", "recover"):
        remote = sub.add_parser(name)
        remote.add_argument("--credentials", type=Path, required=True)
        remote.add_argument("--url")
        remote.add_argument("--ca", type=Path)
        if name == "run":
            remote.add_argument("action_id")
            remote.add_argument("--wait", action="store_true")
        if name == "result":
            remote.add_argument("command_id")
    return parser


def console_url(settings) -> str:
    """Address of the web console for this listener. uvicorn prints only the base URL, which answers 404."""
    host = settings.bind
    if host in {"0.0.0.0", "::"}:
        host = "localhost"  # a wildcard address is not a host a browser can use
    elif ":" in host:
        host = f"[{host}]"
    return f"{'https' if settings.tls_cert else 'http'}://{host}:{settings.port}/manage/"


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "init":
            settings = initialize(args.config)
            print(json.dumps({"agent_id": settings.agent_id, "config": str(args.config.resolve())}, indent=2))
            return 0
        settings = load_settings(args.config)
        if args.command == "serve":
            import uvicorn
            from .app import create_app
            configure_logs(settings)
            print(f"管理台地址：{console_url(settings)}", flush=True)
            # No reload/multiple workers: one authority owns the SQLite journal.
            uvicorn.run(create_app(settings), host=settings.bind, port=settings.port, ssl_certfile=settings.tls_cert,
                ssl_keyfile=settings.tls_key, access_log=False, proxy_headers=False, log_level="info")
        elif args.command in {"pair", "admin-token", "revoke", "devices"}:
            store = Store(settings.db_path)
            try:
                if args.command in {"pair", "admin-token"}:
                    if args.command == "admin-token":
                        args.device_id = args.name
                        args.scope = ["admin:config"]
                        args.target = ["*"]
                    if args.output.exists():
                        raise ValueError("credential output already exists; choose a new file")
                    scopes = args.scope or ["read:status", "control:displays", "device:sync"]
                    targets = args.target or ["*"]
                    token = store.pair(args.device_id, scopes, targets, replace=args.replace)
                    try:
                        private_write(args.output, json.dumps({"device_id": args.device_id, "agent_id": settings.agent_id,
                            "token": token, "scopes": scopes, "targets": targets}, indent=2) + "\n")
                    except BaseException:
                        store.revoke(args.device_id)
                        raise
                    print(json.dumps({"device_id": args.device_id, "credential_file": str(args.output.resolve())}))
                elif args.command == "revoke":
                    store.revoke(args.device_id)
                    print(json.dumps({"device_id": args.device_id, "revoked": True}))
                else:
                    print(json.dumps({"devices": store.devices()}, ensure_ascii=False, indent=2))
            finally:
                store.close()
        elif args.command == "list-displays":
            backend = Backend(settings)
            async def discover():
                health = await backend.probe()
                if health["availability"] != "ready":
                    return {"backend": health, "displays": []}
                return {"backend": health, "displays": await backend.inventory()}
            print(json.dumps(asyncio.run(discover()), ensure_ascii=False, indent=2))
        elif args.command == "diagnostics":
            ready, reason = Backend(settings).availability()
            print(json.dumps({"agent_version": __version__, "agent_id": settings.agent_id,
                "protocol_version": "1.0", "python": platform.python_version(), "platform": platform.system(),
                "backend": settings.backend, "backend_available": ready, "reason_code": reason,
                "display_count": len(settings.displays), "action_count": len(settings.actions),
                "lan_enabled": settings.lan_enabled, "tls_configured": bool(settings.tls_cert)}, indent=2))
        elif args.command == "export-openapi":
            from .app import create_app
            app = create_app(settings)
            try:
                print(json.dumps(app.openapi(), ensure_ascii=False, indent=2))
            finally:
                app.state.store.close()
        elif args.command == "shortcut":
            original = json.loads(args.config.read_text())
            action = next((item for item in original["actions"] if item["action_id"] == args.action_id), None)
            if action is None:
                raise ValueError("unknown action_id")
            action["shortcut"] = None if args.disable else args.key
            if not args.disable and not args.key:
                raise ValueError("provide --key or --disable")
            Settings.model_validate(original)  # includes duplicate shortcut check
            private_write(args.config, json.dumps(original, ensure_ascii=False, indent=2) + "\n")
            print("Saved. Configure your shortcut tool to call deskhubctl run with this action ID; restart Agent after config changes.")
        else:
            connection, status = client(args, settings)
            # client() already used the connection for the identity check, so httpx
            # refuses `with connection:` (it cannot be opened twice). Close it explicitly.
            with contextlib.closing(connection):
                if args.command == "status":
                    print(json.dumps(status, ensure_ascii=False, indent=2))
                elif args.command == "actions":
                    response = connection.get("/api/v1/actions")
                    response.raise_for_status()
                    print(json.dumps(response.json(), ensure_ascii=False, indent=2))
                elif args.command == "recover":
                    recovery = args.credentials.with_name(args.credentials.name + ".last-command.json")
                    original = private_json(recovery)
                    response = connection.get("/api/v1/commands", params={"idempotency_key": original["idempotency_key"]})
                    response.raise_for_status()
                    print(json.dumps(response.json(), ensure_ascii=False, indent=2))
                elif args.command == "result":
                    response = connection.get(f"/api/v1/commands/{args.command_id}")
                    response.raise_for_status()
                    print(json.dumps(response.json(), ensure_ascii=False, indent=2))
                else:
                    response = connection.get("/api/v1/actions")
                    response.raise_for_status()
                    action = next((item for item in response.json()["actions"] if item["action_id"] == args.action_id), None)
                    if action is None:
                        raise ValueError("action is not available to this credential")
                    now = utcnow()
                    command = CommandRequest(request_id=str(uuid4()), idempotency_key=str(uuid4()), action_id=action["action_id"],
                        target_id=action["target_id"], parameters=action["parameters"], sent_at=now, expires_at=now + timedelta(seconds=10))
                    # Save the immutable request before transport. If the POST reply
                    # is lost, retrieve by this original key; never create a new action.
                    recovery = args.credentials.with_name(args.credentials.name + ".last-command.json")
                    private_write(recovery, command.model_dump_json(indent=2) + "\n")
                    try:
                        response = connection.post("/api/v1/commands", json=command.model_dump(mode="json"), timeout=httpx.Timeout(2, connect=2))
                    except httpx.TransportError:
                        response = connection.get("/api/v1/commands", params={"idempotency_key": command.idempotency_key})
                    response.raise_for_status()
                    result = response.json()
                    if args.wait:
                        deadline = time.monotonic() + 15
                        while result["state"] in {"queued", "executing"} and time.monotonic() < deadline:
                            time.sleep(0.25)
                            response = connection.get(f"/api/v1/commands/{result['command_id']}")
                            response.raise_for_status()
                            result = response.json()
                    print(json.dumps(result, ensure_ascii=False, indent=2))
                    return 0 if result["state"] in {"queued", "executing", "succeeded"} else 2
        return 0
    except KeyboardInterrupt:
        return 130
    except Exception as error:
        # httpx exception strings can contain a URL, but never its Authorization.
        # Do not echo validation errors or configuration bodies containing secrets.
        from .errors import AgentError
        safe = error.message if isinstance(error, AgentError) else str(error) if isinstance(error, (ValueError, FileNotFoundError, httpx.HTTPStatusError)) else type(error).__name__
        print(f"DeskHub: {safe}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
