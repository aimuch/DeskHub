#!/usr/bin/env python3
"""DeskHub USB provisioning. Secrets travel through a local serial cable only.

Run ``python tools/provision.py --help``. Importing this module has no side effects.
The transport is intentionally independent of pyserial for deterministic tests.
"""

from __future__ import annotations

import argparse
import datetime as dt
import getpass
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import ssl
import stat
import sys
import time
from typing import Any
from urllib.parse import urlsplit

MAX_FRAME_BYTES = 16 * 1024
MAX_READ_BYTES = 128 * 1024
SCOPES = {"read:status", "control:displays", "device:sync", "admin:config"}
IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,95}\Z")
DEVICE_ERROR_CODES = {
    "INVALID_ARGUMENT", "INVALID_CONFIG", "INVALID_JSON", "UNSUPPORTED", "BUSY",
    "NOT_CONFIGURED", "NVS_FAILED", "PERSIST_FAILED", "TIME_INVALID",
    "FRAME_TOO_LARGE", "UNKNOWN_OP", "UNKNOWN_OPERATION", "INTERNAL_ERROR",
    "TIME_SET_FAILED", "CONFIG_COMMIT_FAILED", "PAYLOAD_TOO_LARGE",
}


class ProvisionError(Exception):
    """A safe, fixed error message. Never wrap input values in this exception."""


def private_json(path: Path) -> dict[str, Any]:
    """Read a bounded, owned, private regular file without following symlinks."""
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
        with os.fdopen(fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
                raise ProvisionError("输入文件必须是仅当前用户可读写的普通文件（chmod 600）。")
            if hasattr(os, "getuid") and info.st_uid != os.getuid():
                raise ProvisionError("输入文件必须属于当前用户。")
            raw = stream.read(MAX_FRAME_BYTES + 1)
        if len(raw) > MAX_FRAME_BYTES:
            raise ProvisionError("输入文件超过 16 KiB。")
        value = json.loads(raw)
        if not isinstance(value, dict):
            raise ProvisionError("输入文件顶层必须是 JSON 对象。")
        return value
    except ProvisionError:
        raise
    except (OSError, ValueError, UnicodeError):
        raise ProvisionError("无法读取私有 JSON 输入文件。") from None


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def valid_utc(value: Any) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", value):
        raise ProvisionError("时间必须是 YYYY-MM-DDTHH:MM:SSZ 格式的 UTC 时间。")
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        if not 2024 <= parsed.year <= 2099:
            raise ValueError
    except ValueError:
        raise ProvisionError("时间无效或不在 2024–2099 年范围。") from None
    return value


def valid_ntp_server(value: Any) -> str:
    if not isinstance(value, str) or not 1 <= len(value) <= 253 or not value.isascii():
        raise ProvisionError("NTP 服务地址必须是 ASCII 主机名或 IPv4 地址，最多 253 字符。")
    try:
        address = ipaddress.ip_address(value)
        if address.version != 4:
            raise ProvisionError("当前 NTP 配置支持主机名或 IPv4 地址。")
        return value
    except ValueError:
        pass
    labels = value.split(".")
    if any(not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", part) for part in labels):
        raise ProvisionError("NTP 服务地址必须是主机名或 IPv4，不可含 URL、路径或端口。")
    # An invalid dotted numeric address must not silently become a DNS name.
    if all(part.isdigit() for part in labels) and len(labels) == 4:
        raise ProvisionError("NTP IPv4 地址无效。")
    return value


def validate_origin(value: Any) -> str:
    if not isinstance(value, str) or len(value) > 256:
        raise ProvisionError("Agent 地址无效。")
    try:
        parsed = urlsplit(value)
        if (
            parsed.scheme != "https" or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or parsed.query or parsed.fragment
            or parsed.path not in ("", "/") or any(c.isspace() for c in value)
            or any(ord(c) < 32 for c in value)
        ):
            raise ValueError
        _ = parsed.port
    except ValueError:
        raise ProvisionError("Agent 必须使用 HTTPS 根地址，不可含凭据、路径、查询或片段。") from None
    return value.rstrip("/")


def validate_ca(value: Any) -> str:
    if not isinstance(value, str) or len(value.encode("utf-8")) > 8192:
        raise ProvisionError("CA 证书为空或超过 8 KiB。")
    if "PRIVATE KEY" in value or "-----BEGIN CERTIFICATE-----" not in value:
        raise ProvisionError("请提供 PEM 证书，不能提供私钥。")
    try:
        ssl.create_default_context(cadata=value)
    except (ssl.SSLError, ValueError):
        raise ProvisionError("CA PEM 证书无效。") from None
    return value


def validate_config(value: Any, *, require_token: bool = True) -> dict[str, Any]:
    """Normalize only supported v0.1 fields; no secrets appear in errors."""
    try:
        if not isinstance(value, dict):
            raise TypeError
        wifi, agent = value["wifi"], value["agent"]
        if not isinstance(wifi, dict) or not isinstance(agent, dict):
            raise TypeError
        ssid, password = wifi["ssid"], wifi["password"]
        if not isinstance(ssid, str) or not 1 <= len(ssid.encode("utf-8")) <= 32 or "\x00" in ssid:
            raise ProvisionError("Wi-Fi SSID 必须是 1–32 个 UTF-8 字节。")
        if not isinstance(password, str) or "\x00" in password:
            raise ProvisionError("Wi-Fi 密码格式无效。")
        # ESP-IDF station password storage accepts at most 63 UTF-8 bytes.
        if password and not 8 <= len(password.encode("utf-8")) <= 63:
            raise ProvisionError("Wi-Fi 密码须为空（开放网络）或 8–63 个 UTF-8 字节。")
        agent_id = agent["agent_id"]
        if not isinstance(agent_id, str) or not IDENTIFIER.fullmatch(agent_id):
            raise ProvisionError("Agent 身份格式无效。")
        token = agent.get("token", "")
        if not isinstance(token, str) or (require_token and not 32 <= len(token) <= 256):
            raise ProvisionError("独立设备凭据缺失或长度无效。")
        if token and (not 32 <= len(token) <= 256 or any(ord(c) < 33 or ord(c) > 126 for c in token)):
            raise ProvisionError("设备凭据格式无效。")
        url, ca_pem = validate_origin(agent["url"]), validate_ca(agent["ca_pem"])
        clock = value.get("time", {"utc": utc_now()})
        if not isinstance(clock, dict):
            raise TypeError
        normalized = {
            "wifi": {"ssid": ssid, "password": password},
            "agent": {"url": url, "agent_id": agent_id, "token": token, "ca_pem": ca_pem},
            "time": {"utc": valid_utc(clock["utc"]), "ntp_server": valid_ntp_server(clock.get("ntp_server", "pool.ntp.org"))},
        }
        if "updates" in value:
            updates = value["updates"]
            if not isinstance(updates, dict) or set(updates) != {"public_key_pem"}:
                raise ProvisionError("更新配置仅接受 public_key_pem 公钥字段。")
            normalized["updates"] = {"public_key_pem": validate_update_key(updates["public_key_pem"])}
        encode_frame({"op": "provision", "config": normalized})
        return normalized
    except ProvisionError:
        raise
    except (KeyError, TypeError, UnicodeError):
        raise ProvisionError("配置缺少 wifi、agent 或 time 的必要字段。") from None


def validate_update_key(value: Any) -> str:
    if not isinstance(value, str):
        raise ProvisionError("更新信任根必须是 P-256 公钥 PEM。")
    try:
        try:
            from firmware_package import public_key_pem
        except ImportError:
            from tools.firmware_package import public_key_pem
        normalized, _ = public_key_pem(value)
        return normalized.decode("ascii")
    except (ValueError, TypeError, UnicodeError):
        raise ProvisionError("更新信任根无效：需要 P-256 公钥 PEM，不接受私钥。") from None


def read_update_key(path: Path) -> str:
    try:
        if path.stat().st_size > 2048:
            raise ProvisionError("更新公钥超过 2 KiB。")
        return validate_update_key(path.read_text(encoding="ascii"))
    except (OSError, UnicodeError):
        raise ProvisionError("无法读取更新公钥 PEM。") from None


def encode_frame(payload: dict[str, Any]) -> bytes:
    try:
        raw = (json.dumps(payload, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")
    except (TypeError, ValueError, UnicodeError):
        raise ProvisionError("无法编码 USB 请求。") from None
    if len(raw) > MAX_FRAME_BYTES:
        raise ProvisionError("USB 请求超过 16 KiB。")
    return raw


class UsbTransport:
    """Bounded JSON-lines protocol; skips startup/log lines without exposing them."""

    def __init__(self, port: Any, *, timeout: float = 8.0):
        self.port = port
        self.timeout = timeout

    def request(self, payload: dict[str, Any]) -> dict[str, Any]:
        raw = encode_frame(payload)
        try:
            count = self.port.write(raw)
            if count != len(raw):
                raise ProvisionError("USB 请求未完整写入；请重新连接后读取设备状态。")
            self.port.flush()
        except ProvisionError:
            raise
        except Exception:
            raise ProvisionError("USB 写入失败；设备可能尚未收到完整请求。") from None
        started, total, line = time.monotonic(), 0, bytearray()
        while time.monotonic() - started < self.timeout:
            try:
                char = self.port.read(1)
            except Exception:
                raise ProvisionError("USB 连接中断；配置是否保存须重新查询。") from None
            if not char:
                continue
            total += len(char)
            if total > MAX_READ_BYTES:
                raise ProvisionError("USB 响应总量超限。")
            line.extend(char)
            if len(line) > MAX_FRAME_BYTES:
                raise ProvisionError("USB 单行响应超过 16 KiB。")
            if char != b"\n":
                continue
            try:
                item = json.loads(bytes(line))
            except (ValueError, UnicodeError):
                line.clear()
                continue
            line.clear()
            if not isinstance(item, dict) or type(item.get("ok")) is not bool:
                continue
            if not item["ok"]:
                code = item.get("error", {}).get("code") if isinstance(item.get("error"), dict) else None
                safe_code = code if isinstance(code, str) and code in DEVICE_ERROR_CODES else "DEVICE_REJECTED"
                # The remote message may echo credentials. Only a stable code is safe.
                raise ProvisionError(f"设备拒绝请求：{safe_code}。")
            result = item.get("result")
            if not isinstance(result, dict):
                raise ProvisionError("USB 响应缺少 result 对象。")
            return result
        raise ProvisionError("USB 响应超时；请读取设备状态，勿假定配置已保存。")


def identify(transport: UsbTransport, expected_device_id: str | None = None) -> dict[str, Any]:
    result = transport.request({"op": "identify"})
    device_id = result.get("device_id")
    if not isinstance(device_id, str) or not IDENTIFIER.fullmatch(device_id):
        raise ProvisionError("设备未返回有效的独立 device_id。")
    if expected_device_id and device_id != expected_device_id:
        raise ProvisionError("USB 设备身份与指定 device_id 不符；未写入配置。")
    if str(result.get("protocol_version", "")) not in ("1", "1.0", "1.0.0"):
        raise ProvisionError("USB 协议版本不兼容；未写入配置。")
    if not isinstance(result.get("firmware_version"), str):
        raise ProvisionError("设备未返回固件版本；未写入配置。")
    return result


def load_local_agent(path: Path) -> tuple[Any, Any]:
    try:
        from deskhub_agent.config import load_settings
        from deskhub_agent.storage import Store

        settings = load_settings(path)
        return settings, Store(settings.db_path)
    except Exception:
        raise ProvisionError("无法读取本机 Agent；请安装 agent 包并检查 --agent-config。") from None


def read_ca(path: Path) -> str:
    try:
        if path.stat().st_size > 8192:
            raise ProvisionError("CA 证书超过 8 KiB。")
        return validate_ca(path.read_text(encoding="ascii"))
    except ProvisionError:
        raise
    except (OSError, UnicodeError):
        raise ProvisionError("无法读取 CA PEM 证书。") from None


def open_usb(port: str, baudrate: int) -> Any:
    try:
        import serial
    except ImportError:
        raise ProvisionError("缺少 pyserial，请安装 tools/requirements.txt。") from None
    try:
        return serial.Serial(port=port, baudrate=baudrate, timeout=0.1, write_timeout=2, exclusive=True)
    except Exception:
        raise ProvisionError("无法打开 USB 串口；检查端口、占用状态与权限。") from None


def parser() -> argparse.ArgumentParser:
    cli = argparse.ArgumentParser(description="DeskHub USB 配置工具；密码和 Token 不接受命令行参数。")
    cli.add_argument("--port", help="原生 USB Serial/JTAG 串口，例如 /dev/cu.usbmodem...")
    cli.add_argument("--baudrate", type=int, default=115200)
    cli.add_argument("--agent-config", type=Path, help="本机 Agent 配置，用于产生每设备凭据")
    cli.add_argument("--ca", type=Path, help="Agent 的受信 CA PEM（公开证书，不是私钥）")
    cli.add_argument("--url", help="HTTPS Agent 根地址，例如 https://desk-mac.local:8765")
    cli.add_argument("--ntp-server", help="可选局域网 NTP 主机名或 IPv4 地址，默认 pool.ntp.org")
    cli.add_argument("--update-public-key", type=Path, help="通过可信 USB 安装的固件更新 P-256 公钥 PEM（不接受私钥）")
    cli.add_argument("--input", type=Path, help="权限 0600 的完整 USB config JSON（含凭据），不写入日志")
    cli.add_argument("--device-id", help="指定预期的 USB 设备身份；不匹配则拒绝写入")
    cli.add_argument("--scope", action="append", choices=sorted(SCOPES), help="允许能力，默认读状态、显示器控制及设备管理同步")
    cli.add_argument("--target", action="append", help="授权显示器 ID，可重复；显示器控制必选，不默认 *；仅状态/设备管理同步时可省略")
    cli.add_argument("--replace", action="store_true", help="明确替换该设备已有凭据；USB失败时旧凭据不会恢复")
    cli.add_argument("--identify", action="store_true", help="仅读取 USB 设备身份")
    cli.add_argument("--status", action="store_true", help="仅读取并输出脱敏状态")
    cli.add_argument("--set-time", action="store_true", help="只通过 USB 将设备设为当前电脑 UTC 时间")
    cli.add_argument("--sound-test", action="store_true", help="只通过 USB 让设备播放一次试听音；程序听不到，响没响要靠人听")
    cli.add_argument("--theme", choices=("mint", "ocean", "amber"), help="只通过 USB 保存屏幕主题；可与 --rotation 同用，未配对也可用")
    cli.add_argument("--rotation", type=int, choices=(0, 90, 180, 270), help="只通过 USB 保存顺时针屏幕旋转角度；可与 --theme 同用，未配对也可用")
    cli.add_argument("--dry-run", action="store_true", help="仅校验 --input，明确仿真；不连接USB、不配对、不写入")
    return cli


POWER_WORDS = {
    "vbus": {"present", "absent", "unknown"},
    "battery": {"present", "absent", "unknown"},
    "charging": {"charging", "full", "idle", "fault", "unknown"},
    "measurement": {"ok", "adc_enable_failed", "adc_read_failed", "adc_out_of_range", "not_measured"},
}
# Plausible ranges of the AXP2101 readings (docs/rules.md R3), the same ones the firmware and the Agent enforce.
POWER_RANGES = {"soc_percent": (0, 100), "battery_mv": (2500, 4600), "vbus_mv": (3000, 6500), "chip_temp_c": (-40, 150)}


def safe_power(value: Any) -> dict[str, Any] | None:
    """The power object of a status reply, reduced to what can be vouched for.

    Unknown keys, words the tool does not know, mistyped or implausible numbers and numbers that contradict the rest
    (a failed measurement, no battery, no USB) are dropped, not corrected. A null from the device stays null: unknown.
    """
    if not isinstance(value, dict):
        return None
    cleaned: dict[str, Any] = {}
    for key, words in POWER_WORDS.items():
        word = value.get(key)
        if isinstance(word, str) and word in words:
            cleaned[key] = word
    if isinstance(value.get("fault"), bool):
        cleaned["fault"] = value["fault"]
    for key, (low, high) in POWER_RANGES.items():
        number = value.get(key)
        if number is None and key in value:
            cleaned[key] = None
        elif type(number) is int or (key == "chip_temp_c" and type(number) is float):
            if math.isfinite(number) and low <= number <= high:
                cleaned[key] = number
    if cleaned.get("measurement") != "ok":
        for key in ("battery_mv", "vbus_mv", "chip_temp_c"):
            if cleaned.get(key) is not None:
                del cleaned[key]
    if cleaned.get("battery") != "present":
        for key in ("soc_percent", "battery_mv"):
            if cleaned.get(key) is not None:
                del cleaned[key]
    if cleaned.get("vbus") != "present" and cleaned.get("vbus_mv") is not None:
        del cleaned["vbus_mv"]
    return cleaned


CHARGE_STATES = {"off", "charging", "holding", "safety_stop", "failsafe", "unconfirmed"}
CHARGE_WORDS = {"switch": {"charging", "stopped", "unknown"}}
EVENT_WORDS = {"request": {"charge", "stop"}, "result": {"ok", "failed", "mismatch", "refused"}}
CHARGE_RANGES = {"limit": (70, 100), "resume": (1, 95), "stop_failures": (0, 99)}
MAX_CHARGE_EVENTS = 16
REASON_CODE = re.compile(r"[A-Z_]{0,40}\Z")


def safe_charge_event(value: Any) -> dict[str, Any] | None:
    """One write to the charge switch, or None. An event with anything wrong in it is dropped whole, never repaired."""
    if not isinstance(value, dict) or set(value) != {"at_ms", "request", "result", "reason"}:
        return None
    at_ms, reason = value["at_ms"], value["reason"]
    if type(at_ms) is not int or at_ms < 0 or not isinstance(reason, str) or not REASON_CODE.match(reason):
        return None
    for key, words in EVENT_WORDS.items():
        if not isinstance(value[key], str) or value[key] not in words:
            return None
    return dict(value)


def safe_charge(value: Any) -> dict[str, Any] | None:
    """The charge protection object of a status reply, reduced to what can be vouched for (docs/charge-protection-design.md).

    Same rules as the power object: unknown keys, words the tool does not know, mistyped or implausible numbers and
    values that contradict each other are dropped, not corrected.
    """
    if not isinstance(value, dict):
        return None
    cleaned: dict[str, Any] = {}
    for key in ("enabled", "faulted"):
        if isinstance(value.get(key), bool):
            cleaned[key] = value[key]
    for key, (low, high) in CHARGE_RANGES.items():
        number = value.get(key)
        if type(number) is int and low <= number <= high:
            cleaned[key] = number
    state, switch, reason = value.get("state"), value.get("switch"), value.get("reason")
    if isinstance(state, str) and state in CHARGE_STATES:
        cleaned["state"] = state
    if isinstance(switch, str) and switch in CHARGE_WORDS["switch"]:
        cleaned["switch"] = switch
    if isinstance(reason, str) and REASON_CODE.match(reason):
        cleaned["reason"] = reason
    if "enabled" in cleaned and "state" in cleaned and cleaned["enabled"] != (cleaned["state"] != "off"):
        del cleaned["state"]  # protection off cannot be in an "on" state, and on cannot be "off"
    if cleaned.get("state") == "unconfirmed" and cleaned.get("switch") not in (None, "unknown"):
        del cleaned["switch"]  # a controller that is unsure does not know the switch
    events = value.get("events")
    if isinstance(events, list):
        checked = [event for event in map(safe_charge_event, events) if event is not None]
        cleaned["events"] = checked[-MAX_CHARGE_EVENTS:]
    return cleaned


PAIRING_PHASES = {"closed", "open", "joining", "redeeming", "save_failed", "done"}
PAIRING_OPEN_PHASES = {"open", "joining", "redeeming", "save_failed"}
PAIRING_WHYS = {"none", "not_allowed", "random_failed", "wrong_phase", "wrong_code", "too_many_wrong_codes", "code_required", "timeout", "cancelled",
                "invalid_input", "too_many_attempts", "wifi_failed", "agent_unreachable", "certificate_expired", "hostname_mismatch", "chain_invalid",
                "redeem_refused", "bad_reply", "save_failed", "readback_mismatch"}


def safe_pairing(value: Any) -> dict[str, Any] | None:
    """The hotspot pairing object of a status reply (docs/pairing-design.md section 13): three members, nothing else.

    The setup code, the hotspot password, the Wi-Fi name, the grant code and the token are not in it, and if a device
    sends them anyway they are not echoed. Words the tool does not know are dropped, not passed on.
    """
    if not isinstance(value, dict):
        return None
    cleaned: dict[str, Any] = {}
    if isinstance(value.get("phase"), str) and value["phase"] in PAIRING_PHASES:
        cleaned["phase"] = value["phase"]
    if isinstance(value.get("why"), str) and value["why"] in PAIRING_WHYS:
        cleaned["why"] = value["why"]
    if isinstance(value.get("open"), bool):
        cleaned["open"] = value["open"]
    if "open" in cleaned and "phase" in cleaned and cleaned["open"] != (cleaned["phase"] in PAIRING_OPEN_PHASES):
        del cleaned["open"]  # a closed session cannot be open, and an open one cannot be closed
    return cleaned or None


def safe_sound(value: Any) -> dict[str, Any] | None:
    """The answer of the device to the test tone (docs/design Dev-Sound): three booleans, nothing else.

    A text or any other key is not echoed. The answer says whether the speaker answered, whether the call played a tone,
    and whether the alert sound is switched on; it does not say that anyone heard it.
    """
    if not isinstance(value, dict):
        return None
    cleaned = {key: value[key] for key in ("available", "played", "enabled") if isinstance(value.get(key), bool)}
    return cleaned or None


def safe_status(result: dict[str, Any]) -> dict[str, Any]:
    # Allowlist scalar diagnostics; no arbitrary echoed remote fields are printed.
    keys = {"device_id", "firmware_version", "protocol_version", "board_model", "configured", "wifi", "agent", "wifi_state", "agent_state", "time_quality", "config_revision", "provision_revision", "update_key_fingerprint", "applied_revision", "ui_alive", "theme", "rotation"}
    cleaned = {key: value for key, value in result.items() if key in keys and isinstance(value, (str, int, float, bool, type(None)))}
    caps = result.get("capabilities")
    if isinstance(caps, dict):
        known = {"usb_provisioning", "charge_limit", "battery_soc", "audio", "display", "touch", "rtc", "power", "management", "signed_ota"}
        safe_caps = {}
        for key, value in caps.items():
            if key not in known:
                continue
            if isinstance(value, str) and value in {"supported", "unsupported", "unverified", "implemented_unverified"}:
                safe_caps[key] = value
            elif isinstance(value, dict):
                support, availability = value.get("support"), value.get("availability")
                if support in {"supported", "unsupported", "unverified"} and availability in {"ready", "unavailable", "fault"}:
                    safe_caps[key] = {"support": support, "availability": availability}
        if safe_caps:
            cleaned["capabilities"] = safe_caps
    power = safe_power(result.get("power"))
    if power is not None:
        cleaned["power"] = power
    charge = safe_charge(result.get("charge"))
    if charge is not None:
        cleaned["charge"] = charge
    pairing = safe_pairing(result.get("pairing"))
    if pairing is not None:
        cleaned["pairing"] = pairing
    return cleaned


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    serial_port = store = None
    newly_paired_device: str | None = None
    completed = False
    try:
        preferences = args.theme is not None or args.rotation is not None
        if preferences and any((args.identify, args.status, args.set_time, args.sound_test, args.dry_run,
                                args.input, args.agent_config, args.ca, args.url, args.ntp_server,
                                args.update_public_key, args.scope, args.target, args.replace)):
            raise ProvisionError("--theme、--rotation 仅可与 --port、--baudrate、--device-id 同用；不能与读取、设时或配对参数混用。")
        if args.sound_test and any((preferences, args.identify, args.status, args.set_time, args.dry_run,
                                    args.input, args.agent_config, args.ca, args.url, args.ntp_server,
                                    args.update_public_key, args.scope, args.target, args.replace)):
            raise ProvisionError("--sound-test 仅可与 --port、--baudrate、--device-id 同用；不能与读取、设时、偏好或配对参数混用。")
        if args.dry_run:
            if not args.input:
                raise ProvisionError("--dry-run 需要 --input 私有配置文件。")
            validate_config(private_json(args.input))
            print("仿真：配置校验通过；未连接 USB、未产生凭据、未写入设备。")
            return 0
        if not args.port:
            raise ProvisionError("请使用 --port 指定 USB 串口。")
        if sum(bool(v) for v in (args.identify, args.status, args.set_time)) > 1:
            raise ProvisionError("--identify、--status、--set-time 只能选择一个。")
        config = validate_config(private_json(args.input)) if args.input else None
        settings = None
        if not (args.identify or args.status or args.set_time or args.sound_test or preferences) and config is None:
            if not args.agent_config or not args.ca:
                raise ProvisionError("交互配置需要 --agent-config 和 --ca。")
            settings, store = load_local_agent(args.agent_config)
            scopes = args.scope or ["read:status", "control:displays", "device:sync"]
            targets = args.target or []
            if "control:displays" in scopes and not targets:
                targets = [v.strip() for v in input("允许的显示器 ID（逗号分隔，不默认授权全部）：").split(",") if v.strip()]
            if ("control:displays" in scopes and not targets) or len(targets) > 4 or len(set(targets)) != len(targets) or any(v != "*" and not IDENTIFIER.fullmatch(v) for v in targets):
                raise ProvisionError("请指定有效的显示器授权范围。")
            url = args.url or input("Agent HTTPS 地址：").strip()
            ssid = input("Wi-Fi SSID（2.4 GHz）：")
            password = getpass.getpass("Wi-Fi 密码（开放网络留空）：")
            config = validate_config({
                "wifi": {"ssid": ssid, "password": password},
                "agent": {"url": url, "agent_id": settings.agent_id, "ca_pem": read_ca(args.ca)},
                "time": {"utc": utc_now(), "ntp_server": args.ntp_server or "pool.ntp.org"},
            }, require_token=False)
        serial_port = open_usb(args.port, args.baudrate)
        transport = UsbTransport(serial_port)
        info = identify(transport, args.device_id)
        if preferences:
            request = {"op": "display_preferences"}
            if args.theme is not None:
                request["theme"] = args.theme
            if args.rotation is not None:
                request["rotation"] = args.rotation
            saved = transport.request(request)
            if saved.get("saved") is not True:
                raise ProvisionError("设备未明确确认偏好已保存；请重新连接并读取 --status。")
            print("偏好已保存，屏幕应用请确认。")
            return 0
        if args.sound_test:
            answer = safe_sound(transport.request({"op": "sound_test"}))
            if answer is None:
                raise ProvisionError("设备的回答里没有可用内容；请读取 --status 后再试。")
            print(json.dumps({"sound_test": answer}, ensure_ascii=False, indent=2))
            if answer.get("available") is not True:
                raise ProvisionError("设备没有检测到扬声器，没有播放试听音。")
            if answer.get("played") is not True:
                raise ProvisionError("设备没有播放试听音（可能离上一次太近）；稍等几秒再试。")
            print("设备确认已调用试听音。程序听不到：请听设备的扬声器；这条调用只说明命令成功，声音本身仍是未验证。")
            return 0
        if args.identify:
            print(json.dumps(safe_status(info), ensure_ascii=False, indent=2))
            return 0
        if args.status:
            print(json.dumps(safe_status(transport.request({"op": "status"})), ensure_ascii=False, indent=2))
            return 0
        if args.set_time:
            transport.request({"op": "set_time", "time": {"utc": utc_now()}})
            print("设备已确认 USB 设时；RTC 保持能力仍需真机验证。")
            return 0
        assert config is not None
        if settings is not None:
            try:
                config["agent"]["token"] = store.pair(info["device_id"], scopes, targets, replace=args.replace)
            except ValueError:
                raise ProvisionError("此设备已有凭据；明确替换时使用 --replace，USB失败后须重新配置。") from None
            except Exception:
                raise ProvisionError("本机 Agent 创建凭据失败；未向设备写入。") from None
            newly_paired_device = info["device_id"]
        # Refresh immediately before sending, including bundles generated earlier.
        config["time"]["utc"] = utc_now()
        if args.ntp_server:
            config["time"]["ntp_server"] = valid_ntp_server(args.ntp_server)
        if args.update_public_key:
            config["updates"] = {"public_key_pem": read_update_key(args.update_public_key)}
        config = validate_config(config)
        saved = transport.request({"op": "provision", "config": config})
        if saved.get("saved") is not True:
            raise ProvisionError("设备未明确确认配置已保存；请重新连接并读取 --status。")
        completed = True
        print("设备已确认配置持久化。请在屏幕检查 Wi-Fi、Agent 身份与安全连接状态。")
        return 0
    except (ProvisionError, KeyboardInterrupt, EOFError) as error:
        print(str(error) if isinstance(error, ProvisionError) else "配置已取消。", file=sys.stderr)
        return 2
    except Exception:
        print("配置失败；为保护凭据，未输出底层异常。可重新连接并查询 --status。", file=sys.stderr)
        return 2
    finally:
        if newly_paired_device and not completed and store is not None:
            try:
                store.revoke(newly_paired_device)
                print("未获设备保存确认，新产生的凭据已吊销；重新配置后再使用。", file=sys.stderr)
            except Exception:
                print("新凭据吊销未确认；请在本机使用 deskhubctl revoke 手动吊销该设备。", file=sys.stderr)
        if serial_port is not None:
            try:
                serial_port.close()
            except Exception:
                pass
        if store is not None and callable(getattr(store, "close", None)):
            try:
                store.close()
            except Exception:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
