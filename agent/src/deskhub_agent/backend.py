"""Real BetterDisplay CLI adapter. No subprocess accepts client-supplied argv.

References (reviewed 2026-10-27):
https://betterdisplay.pro/guide/integration/cli-reference/
https://github.com/waydabber/betterdisplaycli
The installed CLI/help and actual monitor still require G0 verification.
"""
from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import platform
import re

from .errors import AgentError
from .models import DisplayConfig, Settings


class Backend:
    def __init__(self, settings: Settings):
        self.settings = settings
        self.version: str | None = None
        self.process_lock = asyncio.Lock()
        self.health: dict | None = None

    def availability(self) -> tuple[bool, str | None]:
        if self.settings.backend != "betterdisplay":
            return False, "BACKEND_NOT_CONFIGURED"
        if platform.system() != "Darwin":
            return False, "PLATFORM_UNSUPPORTED"
        path = Path(self.settings.backend_path or "")
        if not path.is_file() or not os.access(path, os.X_OK):
            return False, "BACKEND_NOT_INSTALLED"
        if self.health is not None and self.health["availability"] != "ready":
            return False, self.health["reason_code"]
        return True, None

    async def _run(self, *arguments: str) -> str:
        try:
            async with asyncio.timeout(self.settings.backend_timeout_seconds):
                async with self.process_lock:
                    return await self._invoke(*arguments)
        except TimeoutError as error:
            raise AgentError("BACKEND_TIMEOUT", "Backend timed out waiting for its control channel", 504, True) from error

    async def _invoke(self, *arguments: str) -> str:
        available, reason = self.availability()
        if not available:
            raise AgentError("TARGET_UNAVAILABLE", reason or "Backend unavailable", 503)
        process = None
        try:
            process = await asyncio.create_subprocess_exec(self.settings.backend_path, *arguments,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, limit=65536)

            async def bounded_read(stream):
                value = bytearray()
                while True:
                    chunk = await stream.read(4096)
                    if not chunk:
                        return bytes(value)
                    value.extend(chunk)
                    if len(value) > 65536:
                        raise AgentError("BACKEND_OUTPUT_INVALID", "Backend output exceeds 64 KiB", 502)

            async with asyncio.timeout(self.settings.backend_timeout_seconds):
                output, _errors = await asyncio.gather(bounded_read(process.stdout), bounded_read(process.stderr))
                await process.wait()
            if process.returncode:
                # Backend output may contain private machine details; do not echo it.
                raise AgentError("BACKEND_REJECTED", "Display backend returned an error", 502)
            return output.decode("utf-8", errors="strict").strip()
        except (TimeoutError, UnicodeError) as error:
            raise AgentError("BACKEND_TIMEOUT" if isinstance(error, TimeoutError) else "BACKEND_OUTPUT_INVALID",
                             "Backend timed out or returned invalid data", 504, True) from error
        except OSError as error:
            raise AgentError("TARGET_UNAVAILABLE", "Could not start configured backend", 503) from error
        finally:
            if process is not None and process.returncode is None:
                process.kill()
                await process.wait()

    async def probe(self) -> dict:
        # Clear a previous transient fault so the read-only probe can recover when
        # the user's GUI session/backend becomes available again.
        self.health = None
        ready, reason = self.availability()
        if not ready:
            self.health = {"availability": "unavailable", "reason_code": reason, "version": None}
            return self.health
        try:
            output = await self._run("help")
            match = re.search(r"BetterDisplay (?:Version )?([^\n]{1,100})", output)
            if not match:
                raise AgentError("BACKEND_OUTPUT_INVALID", "CLI help did not identify BetterDisplay", 502)
            self.version = match.group(1)
            self.health = {"availability": "ready", "reason_code": None, "version": self.version}
        except AgentError as error:
            self.health = {"availability": "fault", "reason_code": error.code, "version": None}
        return self.health

    @staticmethod
    def parse_inventory(output: str) -> list[dict]:
        """Unknown output fails closed instead of interpreting a display index."""
        try:
            raw = json.loads(output)
        except ValueError as error:
            raise AgentError("BACKEND_OUTPUT_INVALID", "CLI identifiers must be JSON", 502) from error
        found: list[dict] = []

        def walk(value, depth=0):
            if depth > 6:
                raise AgentError("BACKEND_OUTPUT_INVALID", "CLI identifier nesting too deep", 502)
            if isinstance(value, list):
                for item in value:
                    walk(item, depth + 1)
            elif isinstance(value, dict):
                if "UUID" in value:
                    found.append(value)
                else:
                    for child in value.values():
                        if isinstance(child, (dict, list)):
                            walk(child, depth + 1)
        walk(raw)
        if len(found) > 32:
            raise AgentError("BACKEND_OUTPUT_INVALID", "Too many displays in inventory", 502)
        return found

    async def inventory(self) -> list[dict]:
        return self.parse_inventory(await self._run("get", "--identifiers"))

    async def validate_binding(self, display: DisplayConfig) -> None:
        inventory = await self.inventory()
        expected = display.binding
        matches = [item for item in inventory if str(item.get("UUID", "")).lower() == expected.uuid.lower()]
        if len(matches) != 1:
            raise AgentError("TARGET_UNAVAILABLE", "DISPLAY_BINDING_MISSING_OR_AMBIGUOUS", 503)
        actual = matches[0]
        if str(actual.get("serial", "")) != expected.serial or str(actual.get("registryLocation", "")) != expected.registry_location:
            raise AgentError("TARGET_UNAVAILABLE", "DISPLAY_BINDING_CHANGED_RECONFIRM_REQUIRED", 503)
        # A duplicate serial across other UUIDs must also be reviewed, even if an
        # OS UUID happens to remain stable after reconnecting the wrong panel.
        if sum(str(item.get("serial", "")) == expected.serial for item in inventory) != 1:
            raise AgentError("TARGET_UNAVAILABLE", "DISPLAY_SERIAL_AMBIGUOUS_RECONFIRM_REQUIRED", 503)

    @staticmethod
    def selectors(display: DisplayConfig) -> tuple[str, ...]:
        return (f"--UUID={display.binding.uuid}", f"--serial={display.binding.serial}",
                f"--registryLocation={display.binding.registry_location}")

    async def set_input(self, display: DisplayConfig, input_id: str) -> None:
        # Input numeric values only come from the locally configured mapping.
        await self._run("set", *self.selectors(display), "--feature=ddc", "--vcp=0x60",
                        f"--value={display.inputs[input_id]}")

    async def read_input(self, display: DisplayConfig) -> str | None:
        if not display.readback_enabled:
            return None
        await self.validate_binding(display)
        raw = await self._run("get", *self.selectors(display), "--feature=ddc", "--vcp=0x60", "--value")
        # Never infer a value from a descriptive text or cached request.
        if not re.fullmatch(r"(?:0x[0-9a-fA-F]{1,4}|[0-9]{1,5})", raw):
            raise AgentError("VERIFICATION_UNAVAILABLE", "Backend readback format is not recognized", 502)
        numeric = int(raw, 16 if raw.startswith("0x") else 10)
        matches = [key for key, value in display.inputs.items() if value == numeric]
        return matches[0] if len(matches) == 1 else None
