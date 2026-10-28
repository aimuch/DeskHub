from __future__ import annotations

import re

from datetime import datetime, timezone
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, StringConstraints, field_validator, model_validator

Identifier = Annotated[str, StringConstraints(pattern=r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,95}$")]
SCOPES = frozenset({"read:status", "control:displays", "admin:config", "device:sync"})
TERMINAL_STATES = frozenset({"succeeded", "unconfirmed", "failed", "expired", "cancelled", "partial"})


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def stamp(value: datetime | None = None) -> str:
    return (value or utcnow()).isoformat(timespec="milliseconds").replace("+00:00", "Z")


class CommandParameters(BaseModel):
    model_config = ConfigDict(extra="forbid")
    input_id: Identifier


class CommandRequest(BaseModel):
    model_config = ConfigDict(extra="ignore")
    request_id: Identifier
    idempotency_key: Identifier
    action_id: Identifier
    target_id: Identifier
    parameters: CommandParameters
    sent_at: datetime
    expires_at: datetime | None = None

    @field_validator("sent_at", "expires_at")
    @classmethod
    def utc_timestamp(cls, value: datetime | None) -> datetime | None:
        if value is not None and (value.tzinfo is None or value.utcoffset() is None):
            raise ValueError("timezone is required")
        return value.astimezone(timezone.utc) if value else None

    @model_validator(mode="after")
    def expiry(self) -> CommandRequest:
        from datetime import timedelta
        if self.expires_at is None:
            self.expires_at = self.sent_at + timedelta(seconds=10)
        if not 0 < (self.expires_at - self.sent_at).total_seconds() <= 30:
            raise ValueError("validity must be greater than 0 and at most 30 seconds")
        return self


class ActionConfig(BaseModel):
    model_config = ConfigDict(extra="forbid")
    action_id: Identifier
    label: str = Field(min_length=1, max_length=64)
    target_id: Identifier
    input_id: Identifier
    enabled: bool = True
    shortcut: str | None = Field(default=None, max_length=100)


class DisplayBinding(BaseModel):
    model_config = ConfigDict(extra="forbid")
    uuid: str = Field(min_length=8, max_length=128)
    serial: str = Field(min_length=1, max_length=128)
    registry_location: str = Field(min_length=1, max_length=512)

    @field_validator("serial")
    @classmethod
    def meaningful_serial(cls, value: str) -> str:
        if value.strip().lower() in {"0", "unknown", "n/a", "none"}:
            raise ValueError("missing or zero serial requires a separate binding strategy; refusing automatic control")
        return value


class DisplayConfig(BaseModel):
    model_config = ConfigDict(extra="forbid")
    monitor_id: Identifier
    label: str = Field(min_length=1, max_length=64)
    binding: DisplayBinding
    inputs: dict[Identifier, Annotated[int, Field(strict=True, ge=0, le=65535)]] = Field(min_length=1, max_length=12)
    readback_enabled: bool = False
    compatibility_verified: bool = False
    return_path_verified: bool = False


_PUBLIC_URL = re.compile(r"https://(?:[A-Za-z0-9](?:[A-Za-z0-9.-]{0,98}[A-Za-z0-9])?|\[[0-9A-Fa-f:.]{2,45}\])(?::([0-9]{1,5}))?\Z")


def valid_public_url(value) -> bool:
    """An https origin a device can connect to: host and optional port, nothing else."""
    if not isinstance(value, str) or len(value) > 120:
        return False
    match = _PUBLIC_URL.fullmatch(value)
    if not match:
        return False
    port = match.group(1)
    return port is None or 1 <= int(port) <= 65535


class Settings(BaseModel):
    model_config = ConfigDict(extra="forbid")
    schema_version: Literal[1] = 1
    agent_id: Identifier
    host_id: Identifier = "host-main"
    name: str = Field(default="DeskHub Agent", min_length=1, max_length=64)
    db_path: str = "state/agent.sqlite3"
    bind: str = "127.0.0.1"
    port: int = Field(default=8765, ge=1, le=65535)
    lan_enabled: bool = False
    tls_cert: str | None = None
    tls_key: str | None = None
    firmware_public_key: str | None = None
    # Hotspot pairing (docs/pairing-design.md): the CA certificate that devices pin, and the address devices connect to.
    pairing_ca: str | None = None
    public_url: str | None = None
    allowed_networks: list[str] = Field(default_factory=lambda: ["127.0.0.0/8", "::1/128"], max_length=16)
    backend: Literal["disabled", "betterdisplay"] = "disabled"
    backend_path: str | None = None
    backend_timeout_seconds: float = Field(default=5, ge=0.1, le=5)
    displays: list[DisplayConfig] = Field(default_factory=list, max_length=4)
    actions: list[ActionConfig] = Field(default_factory=list, max_length=12)
    retention_hours: int = Field(default=24, ge=24, le=720)

    @field_validator("public_url")
    @classmethod
    def check_public_url(cls, value):
        if value is not None and not valid_public_url(value):
            raise ValueError("public_url must be https://host or https://host:port, nothing else")
        return value

    @model_validator(mode="after")
    def validate_graph(self) -> Settings:
        import ipaddress
        if self.bind != "localhost":
            address = ipaddress.ip_address(self.bind)
            local = address.is_loopback
        else:
            local = True
        if not local and (not self.lan_enabled or not self.tls_cert or not self.tls_key):
            raise ValueError("LAN binding requires lan_enabled and a TLS certificate/key")
        if bool(self.tls_cert) != bool(self.tls_key):
            raise ValueError("TLS certificate and key must be configured together")
        for network in self.allowed_networks:
            ipaddress.ip_network(network, strict=False)
        if self.backend != "disabled" and (not self.backend_path or not self.backend_path.startswith("/")):
            raise ValueError("backend_path must be an absolute executable path")
        monitors = {display.monitor_id: display for display in self.displays}
        if len(monitors) != len(self.displays):
            raise ValueError("duplicate monitor_id")
        if len({action.action_id for action in self.actions}) != len(self.actions):
            raise ValueError("duplicate action_id")
        shortcuts: set[str] = set()
        for action in self.actions:
            if action.target_id not in monitors or action.input_id not in monitors[action.target_id].inputs:
                raise ValueError("action refers to an unknown target or input")
            if action.enabled and action.shortcut:
                key = action.shortcut.strip().lower()
                if key in shortcuts:
                    raise ValueError("shortcut conflict")
                shortcuts.add(key)
        return self


class ErrorDetail(BaseModel):
    code: str
    message: str
    retryable: bool
    request_id: str | None = None


class ErrorResponse(BaseModel):
    error: ErrorDetail


class Observation(BaseModel):
    input_id: str | None
    quality: Literal["valid", "stale", "unavailable", "unsupported", "invalid"]


class Verification(BaseModel):
    method: str
    result: str
    reason_code: str | None = None


class CommandResult(BaseModel):
    request_id: str
    command_id: str
    state: Literal["queued", "executing", "succeeded", "unconfirmed", "failed", "expired", "cancelled", "partial"]
    target_id: str
    requested: CommandParameters
    observed: Observation
    verification: Verification
    error: ErrorDetail | None = None
    accepted_at: datetime
    completed_at: datetime | None = None


class Status(BaseModel):
    agent_id: str
    agent_version: str
    protocol_version: str
    boot_id: str
    name: str
    health: str
    server_time: datetime
    uptime_seconds: float


class Metric(BaseModel):
    key: str
    label: str
    value: float | int | None
    unit: str
    source: str
    observed_at: datetime
    ttl_ms: int
    quality: Literal["valid", "stale", "unavailable", "unsupported", "invalid"]
    reason_code: str | None = None


class HostSnapshot(BaseModel):
    host_id: str
    online: bool
    observed_at: datetime | None = None
    metrics: list[Metric]


class HostsResponse(BaseModel):
    schema_version: int = 1
    hosts: list[HostSnapshot]


class Capability(BaseModel):
    support: Literal["supported", "unsupported", "unverified"]
    availability: Literal["ready", "unavailable", "fault"]
    reason_code: str | None = None
    constraints: list[str] = Field(default_factory=list)


class ResourceLimits(BaseModel):
    request_bytes: int
    snapshot_bytes: int
    hosts: int
    displays: int
    actions: int


class BackendHealth(BaseModel):
    availability: Literal["ready", "unavailable", "fault"]
    reason_code: str | None = None
    version: str | None = None


class CapabilitiesResponse(BaseModel):
    schema_version: Literal[1] = 1
    capabilities: dict[str, Capability]
    limits: ResourceLimits
    backend: BackendHealth


class ActionRecord(BaseModel):
    action_id: str
    label: str
    target_id: str
    type: Literal["display.set_input"] = "display.set_input"
    parameters: CommandParameters
    enabled: bool


class ActionsResponse(BaseModel):
    schema_version: Literal[1] = 1
    actions: list[ActionRecord] = Field(max_length=12)


class InputRecord(BaseModel):
    input_id: str
    label: str


class DisplayObservation(Observation):
    observed_at: datetime
    ttl_ms: int


class DisplayRoute(BaseModel):
    route_id: str
    agent_id: str
    backend: str
    compatibility_verified: bool
    return_path_verified: bool


class DisplayRecord(BaseModel):
    monitor_id: str
    label: str
    inputs: list[InputRecord] = Field(max_length=12)
    observed: DisplayObservation
    capabilities: dict[str, Capability]
    routes: list[DisplayRoute]
    return_path_verified: bool


class DisplaysResponse(BaseModel):
    schema_version: Literal[1] = 1
    displays: list[DisplayRecord] = Field(max_length=4)
