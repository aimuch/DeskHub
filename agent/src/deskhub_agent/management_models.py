from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from .models import Identifier

BOARD_MODEL = "ESP32-S3-Touch-AMOLED-2.16"
UPDATE_STATES = frozenset({"queued", "downloading", "verifying", "writing", "rebooting", "pending_confirmation", "succeeded", "failed", "rolled_back", "unconfirmed"})
TERMINAL_UPDATE_STATES = frozenset({"succeeded", "failed", "rolled_back", "unconfirmed"})


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)


class DisplayConfiguration(StrictModel):
    brightness: int = Field(default=50, ge=3, le=100)
    rotation: Literal[0, 90, 180, 270] = Field(default=0, description="Clockwise screen angle in 90-degree steps; omitted schema-1 values default to 0.")
    clock_s: int = Field(default=60, ge=5, le=86400)
    dim_s: int = Field(default=120, ge=5, le=86400)
    off_s: int = Field(default=300, ge=0, le=86400)
    night_enabled: bool = True
    night_start: int = Field(default=1320, ge=0, le=1439)
    night_end: int = Field(default=420, ge=0, le=1439)
    night_cap: int = Field(default=15, ge=3, le=100)
    protection: bool = True

    @field_validator("rotation", mode="before")
    @classmethod
    def integer_rotation(cls, value):
        if type(value) is not int:
            raise ValueError("rotation must be an integer angle")
        return value

    @model_validator(mode="after")
    def thresholds(self):
        if self.dim_s < self.clock_s:
            raise ValueError("dimming must follow the clock threshold")
        if self.off_s and self.off_s < self.dim_s:
            raise ValueError("screen off must follow dimming")
        return self


class LayoutConfiguration(StrictModel):
    preset: Literal["desk", "clock", "focus"] = "desk"
    theme: Literal["mint", "ocean", "amber"] = Field(default="mint", description="Built-in device color theme; omitted schema-1 values default to mint.")
    action_ids: list[Identifier] = Field(default_factory=list, max_length=4)
    show_seconds: bool = False
    show_cpu: bool = True

    @field_validator("action_ids")
    @classmethod
    def unique_actions(cls, value):
        if len(set(value)) != len(value):
            raise ValueError("layout action identifiers must be unique")
        return value


class DeviceConfiguration(StrictModel):
    schema_version: Literal[1] = 1

    @field_validator("schema_version", mode="before")
    @classmethod
    def integer_schema(cls, value):
        if type(value) is not int:
            raise ValueError("schema version must be an integer")
        return value

    display: DisplayConfiguration = Field(default_factory=DisplayConfiguration)
    layout: LayoutConfiguration = Field(default_factory=LayoutConfiguration)


class ConfigurationWrite(StrictModel):
    expected_revision: int = Field(ge=0, le=2147483646)
    configuration: DeviceConfiguration


# Plausible ranges of the AXP2101 readings (docs/rules.md R3). A number outside them is a device bug: the report is
# rejected, never clamped, so the console cannot show an invented value.
BATTERY_MV = (2500, 4600)
VBUS_MV = (3000, 6500)
CHIP_TEMP_C = (-40, 150)


class PowerReport(StrictModel):
    vbus: Literal["present", "absent", "unknown"] = "unknown"
    battery: Literal["present", "absent", "unknown"] = "unknown"
    charging: Literal["charging", "full", "idle", "fault", "unknown"] = "unknown"
    fault: bool = False
    # Added in firmware 0.3.0. Older firmware omits them: they stay null and the state is "not_measured".
    soc_percent: int | None = Field(default=None, ge=0, le=100, description="The power chip's own battery level estimate, not validated against the cell.")
    battery_mv: int | None = Field(default=None, ge=BATTERY_MV[0], le=BATTERY_MV[1])
    vbus_mv: int | None = Field(default=None, ge=VBUS_MV[0], le=VBUS_MV[1])
    chip_temp_c: float | None = Field(default=None, ge=CHIP_TEMP_C[0], le=CHIP_TEMP_C[1], allow_inf_nan=False,
                                      description="The power chip's die temperature. It is never the battery temperature.")
    measurement: Literal["ok", "adc_enable_failed", "adc_read_failed", "adc_out_of_range", "not_measured"] = "not_measured"

    @model_validator(mode="after")
    def consistent(self):
        # A failed measurement carries no numbers; nothing is measured about a battery or USB that is not there.
        if self.measurement != "ok" and any(value is not None for value in (self.battery_mv, self.vbus_mv, self.chip_temp_c)):
            raise ValueError("a failed or missing measurement cannot carry voltages or a temperature")
        if self.battery != "present" and (self.soc_percent is not None or self.battery_mv is not None):
            raise ValueError("without a present battery there is no level and no battery voltage")
        if self.vbus != "present" and self.vbus_mv is not None:
            raise ValueError("without present USB power there is no USB voltage")
        return self


class UpdateReport(StrictModel):
    job_id: Identifier
    state: Literal["queued", "downloading", "verifying", "writing", "rebooting", "pending_confirmation", "succeeded", "failed", "rolled_back", "unconfirmed"]
    progress_percent: int = Field(default=0, ge=0, le=100)
    error_code: str | None = Field(default=None, max_length=96, pattern=r"^[A-Z0-9_]+$")


class DeviceCapabilities(StrictModel):
    management: bool = True
    signed_ota: bool = False
    layouts: list[Literal["desk", "clock", "focus"]] = Field(default_factory=lambda: ["desk", "clock", "focus"], max_length=3)
    update_key_fingerprint: str | None = Field(default=None, pattern=r"^[a-f0-9]{64}$")


class DeviceReport(StrictModel):
    firmware_version: str = Field(min_length=1, max_length=31, pattern=r"^[A-Za-z0-9_.+-]+$")
    board_model: Literal[BOARD_MODEL] = BOARD_MODEL
    boot_id: Identifier
    uptime_seconds: int = Field(ge=0)
    free_heap_bytes: int = Field(ge=0)
    applied_revision: int = Field(default=0, ge=0, le=2147483647)
    configuration: DeviceConfiguration
    config_error: str | None = Field(default=None, max_length=96, pattern=r"^[A-Z0-9_]+$")
    power: PowerReport = Field(default_factory=PowerReport)
    update: UpdateReport | None = None
    capabilities: DeviceCapabilities = Field(default_factory=DeviceCapabilities)


class UpdateCreate(StrictModel):
    package_id: str = Field(pattern=r"^[a-f0-9]{64}$")
    expected_firmware_version: str = Field(min_length=1, max_length=31)


class AdminLogin(StrictModel):
    token: str = Field(min_length=20, max_length=256)


class PasswordLogin(StrictModel):
    password: str = Field(max_length=256)   # no lower bound: a short wrong password gets the same answer as a long one


class PasswordWrite(StrictModel):
    password: str = Field(max_length=256)   # the rules (length, common words) are checked in admin_auth, with a clear answer
    current_password: str | None = Field(default=None, max_length=256)


class FirmwarePayload(StrictModel):
    schema_version: Literal[1] = 1
    board_model: Literal[BOARD_MODEL]
    chip: Literal["esp32s3"]
    version: str = Field(min_length=1, max_length=31, pattern=r"^[A-Za-z0-9_.+-]+$")
    config_schema_min: Literal[1]
    config_schema_max: Literal[1]
    image_size: int = Field(ge=256, le=6 * 1024 * 1024)
    image_sha256: str = Field(pattern=r"^[a-f0-9]{64}$")

    @field_validator("schema_version", "config_schema_min", "config_schema_max", mode="before")
    @classmethod
    def integer_schema(cls, value):
        if type(value) is not int:
            raise ValueError("schema versions must be integers")
        return value
