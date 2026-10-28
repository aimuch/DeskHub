from __future__ import annotations

import asyncio
import logging
import time

from .backend import Backend
from .errors import AgentError
from .models import CommandRequest, Settings, stamp, utcnow
from .storage import Store

logger = logging.getLogger("deskhub.commands")


class Scheduler:
    def __init__(self, settings: Settings, store: Store, backend: Backend):
        self.settings, self.store, self.backend = settings, store, backend
        self.tasks: set[asyncio.Task] = set()
        self.displays = {display.monitor_id: display for display in settings.displays}
        self.actions = {action.action_id: action for action in settings.actions}

    async def submit(self, identity: dict, request: CommandRequest) -> dict:
        self.store.authorize(identity, "control:displays", request.target_id)
        # Existing results survive expiry and restart. A retry is not a new action.
        existing = self.store.existing(identity, request)
        if existing:
            return existing
        now = utcnow()
        if abs((now - request.sent_at).total_seconds()) > 5:
            raise AgentError("CLOCK_SKEW", "Client time differs by more than 5 seconds; synchronize time", 400)
        if request.expires_at <= now:
            raise AgentError("COMMAND_EXPIRED", "Command expired before acceptance", 410)
        action = self.actions.get(request.action_id)
        if not action or not action.enabled:
            raise AgentError("UNSUPPORTED", "Action is not registered or enabled", 400)
        if action.target_id != request.target_id or action.input_id != request.parameters.input_id:
            raise AgentError("INVALID_ARGUMENT", "Target and parameters do not match the registered action", 400)
        display = self.displays.get(request.target_id)
        if not display:
            raise AgentError("TARGET_NOT_FOUND", "Display is not configured", 404)
        ready, reason = self.backend.availability()
        if not ready:
            raise AgentError("TARGET_UNAVAILABLE", reason or "Backend unavailable", 503)
        result, created = self.store.enqueue(identity, request)
        if created:
            # Use a monotonic deadline as well as UTC, so a later clock rollback
            # cannot extend the command's accepted lifetime.
            deadline = time.monotonic() + max(0, (request.expires_at - now).total_seconds())
            task = asyncio.create_task(self.execute(identity["device_id"], request, result["command_id"], deadline))
            self.tasks.add(task)
            task.add_done_callback(self.tasks.discard)
        return result

    async def execute(self, device_id: str, request: CommandRequest, command_id: str, deadline: float) -> None:
        issued = False
        try:
            identity = self.store.identity(device_id)
            self.store.authorize(identity, "control:displays", request.target_id)
            display = self.displays[request.target_id]
            # Validate the composite binding anew for every action. Missing or
            # duplicated identities cannot fall back to the first display.
            await self.backend.validate_binding(display)
            if utcnow() >= request.expires_at or time.monotonic() >= deadline:
                self.store.update(command_id, state="expired", completed_at=stamp(),
                    verification={"method": "readback", "result": "not_executed", "reason_code": "COMMAND_EXPIRED"})
                return
            # Revocation may happen while the backend inventory was running.
            self.store.authorize(self.store.identity(device_id), "control:displays", request.target_id)
            self.store.update(command_id, state="executing")  # durable before side effect
            logger.info("device_id=%s request_id=%s command_id=%s state=executing", device_id, request.request_id, command_id)
            issued = True
            await self.backend.set_input(display, request.parameters.input_id)
            try:
                observed = await self.backend.read_input(display)
            except AgentError as error:
                observed = None
                reason = error.code if error.code != "TARGET_UNAVAILABLE" else "CONTROL_PATH_LOST_AFTER_SWITCH"
            else:
                reason = "READBACK_UNSUPPORTED" if not display.readback_enabled else "VERIFICATION_UNAVAILABLE"
            if observed == request.parameters.input_id:
                self.store.update(command_id, state="succeeded", completed_at=stamp(),
                    observed={"input_id": observed, "quality": "valid"},
                    verification={"method": "readback", "result": "matched", "reason_code": None})
            else:
                self.store.update(command_id, state="unconfirmed", completed_at=stamp(),
                    observed={"input_id": observed, "quality": "valid" if observed else "unavailable"},
                    verification={"method": "readback", "result": "mismatch" if observed else "unavailable",
                                  "reason_code": "READBACK_MISMATCH" if observed else reason})
        except asyncio.CancelledError:
            self.store.update(command_id, state="unconfirmed" if issued else "cancelled", completed_at=stamp(),
                verification={"method": "readback", "result": "unavailable", "reason_code": "AGENT_STOPPING"})
            raise
        except AgentError as error:
            # A nonzero exit/timeout after invocation does not prove the display
            # ignored the command; retain an honest unknown result.
            self.store.update(command_id, state="unconfirmed" if issued else "failed", completed_at=stamp(),
                verification={"method": "readback", "result": "unavailable" if issued else "not_executed", "reason_code": error.code},
                error={"code": error.code, "message": error.message, "retryable": False, "request_id": request.request_id})
        except Exception:
            logger.exception("command_id=%s request_id=%s internal execution error", command_id, request.request_id)
            self.store.update(command_id, state="unconfirmed" if issued else "failed", completed_at=stamp(),
                verification={"method": "readback", "result": "unavailable", "reason_code": "INTERNAL_ERROR"},
                error={"code": "INTERNAL_ERROR", "message": "Command outcome could not be determined", "retryable": False, "request_id": request.request_id})
        finally:
            logger.info("device_id=%s request_id=%s command_id=%s execution_finished", device_id, request.request_id, command_id)

    async def stop(self) -> None:
        for task in list(self.tasks):
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
