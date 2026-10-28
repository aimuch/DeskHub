from __future__ import annotations

import asyncio
from copy import deepcopy
import time

from .backend import Backend
from .errors import AgentError
from .models import Settings, stamp


class DisplaySnapshots:
    """Bounded background probing keeps slow DDC out of HTTP snapshot handlers."""
    def __init__(self, settings: Settings, backend: Backend):
        self.settings, self.backend = settings, backend
        self.values: dict[str, tuple[float, dict]] = {}
        self.poll_lock = asyncio.Lock()

    def blank(self, display) -> dict:
        ready, reason = self.backend.availability()
        supported = display.compatibility_verified and display.return_path_verified
        return {"monitor_id": display.monitor_id, "label": display.label,
            "inputs": [{"input_id": key, "label": key} for key in display.inputs],
            "observed": {"input_id": None, "quality": "unavailable", "observed_at": stamp(), "ttl_ms": 90000},
            "capabilities": {
                "set_input": {"support": "supported" if supported else "unverified",
                              "availability": "unavailable", "reason_code": reason or "BINDING_NOT_YET_CHECKED"},
                "read_input": {"support": "unverified" if display.readback_enabled else "unsupported", "availability": "unavailable",
                               "reason_code": "BINDING_NOT_YET_CHECKED" if display.readback_enabled else "READBACK_DISABLED"},
                "enumerate_inputs": {"support": "unsupported", "availability": "unavailable", "reason_code": "USER_CONFIGURED_MAPPING_ONLY"}},
            "return_path_verified": display.return_path_verified,
            "routes": [{"route_id": f"{self.settings.agent_id}:betterdisplay", "agent_id": self.settings.agent_id,
                        "backend": self.settings.backend, "compatibility_verified": display.compatibility_verified,
                        "return_path_verified": display.return_path_verified}]}

    async def refresh(self, display):
        item = self.blank(display)
        ready, reason = self.backend.availability()
        if ready:
            try:
                # One display probe has a total five-second budget, including
                # binding and optional input readback. Other snapshots stay usable.
                async with asyncio.timeout(5):
                    await self.backend.validate_binding(display)
                    observed = await self.backend.read_input(display) if display.readback_enabled else None
                item["observed"].update(input_id=observed, quality="valid" if observed else "unavailable", observed_at=stamp())
                item["capabilities"]["set_input"].update(availability="ready", reason_code=None)
                if display.readback_enabled:
                    item["capabilities"]["read_input"].update(availability="ready", reason_code=None)
            except (AgentError, TimeoutError) as error:
                reason = error.code if isinstance(error, AgentError) else "BACKEND_TIMEOUT"
                item["capabilities"]["set_input"].update(availability="unavailable", reason_code=reason)
                if display.readback_enabled:
                    item["capabilities"]["read_input"].update(availability="unavailable", reason_code=reason)
        self.values[display.monitor_id] = (time.monotonic(), item)

    def read(self, display) -> dict:
        entry = self.values.get(display.monitor_id)
        if not entry:
            return self.blank(display)
        sampled, original = entry
        item = deepcopy(original)
        if time.monotonic() - sampled > 90:
            if item["observed"]["quality"] == "valid":
                item["observed"]["quality"] = "stale"
            for capability in ("set_input", "read_input"):
                item["capabilities"][capability].update(availability="unavailable", reason_code="SNAPSHOT_STALE")
        return item

    async def run(self):
        while True:
            for display in self.settings.displays:
                await self.refresh(display)
            await asyncio.sleep(30)
