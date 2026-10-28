#!/usr/bin/env python3
"""Standalone, checked USB installer for the extracted DeskHub firmware bundle."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
from importlib import metadata
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys
import time
from typing import Any

BOARD = 'ESP32-S3-Touch-AMOLED-2.16'
FLASH_BYTES = 16 * 1024 * 1024
ESPTOOL_VERSION = '5.3.1'
# Restrict USB installation to this project's existing partition layout. The
# gaps include NVS and PHY data and are deliberately never written here.
ARTIFACTS = {
    0x0: ('bootloader/bootloader.bin', 0x8000),
    0x8000: ('partition_table/partition-table.bin', 0x1000),
    0xF000: ('ota_data_initial.bin', 0x2000),
    0x20000: ('deskhub.bin', 0x600000),
}
SETTINGS = {'flash_mode': 'dio', 'flash_size': '16MB', 'flash_freq': '80m'}
STATUS_SECONDS = 8.0
STATUS_FRAME_BYTES = 16 * 1024
STATUS_READ_BYTES = 128 * 1024
STATUS_ID = re.compile(r'deskhub-[0-9a-f]{12}\Z')
STATUS_VERSION = re.compile(r'[A-Za-z0-9_.+\-]{1,31}\Z')


class BundleError(Exception):
    """A bounded validation or transport failure, without binary data."""


@dataclass(frozen=True)
class Image:
    offset: int
    path: Path
    sha256: str
    size: int


@dataclass(frozen=True)
class Bundle:
    version: str
    images: tuple[Image, ...]


def _json_unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise BundleError('清单包含重复 JSON 字段，未开始写入。')
        value[key] = item
    return value


def verify_bundle(directory: Path, manifest: dict[str, Any] | None = None) -> Bundle:
    """Validate all image bytes before opening a serial port or invoking tools."""
    root = directory.resolve(strict=True)
    if not root.is_dir():
        raise BundleError('请完整解压 USB 刷机包后再运行。')
    if manifest is None:
        source = root / 'manifest.json'
        if source.is_symlink() or not source.is_file() or source.stat().st_size > 65536:
            raise BundleError('包清单不存在或不合法，未开始写入。')
        try:
            manifest = json.loads(source.read_text(encoding='utf-8'), object_pairs_hook=_json_unique)
        except (OSError, ValueError, UnicodeError) as error:
            raise BundleError('无法读取 USB 包清单，未开始写入。') from error
    if not isinstance(manifest, dict) or manifest.get('schema_version') != 1 or type(manifest.get('schema_version')) is not int:
        raise BundleError('USB 包清单版本不支持，未开始写入。')
    if manifest.get('board') != BOARD or manifest.get('chip') != 'esp32s3' or manifest.get('signed_ota') is not False:
        raise BundleError('这不是目标板的 DeskHub USB 刷机包，未开始写入。')
    version = manifest.get('version')
    if not isinstance(version, str) or not re.fullmatch(r'[A-Za-z0-9_.+\-]{1,31}', version):
        raise BundleError('固件版本无效，未开始写入。')
    if manifest.get('flash_settings') != SETTINGS:
        raise BundleError('包的 Flash 参数与本板不匹配，未开始写入。')
    items = manifest.get('files')
    if not isinstance(items, list) or len(items) != len(ARTIFACTS):
        raise BundleError('USB 包必须包含全部 4 个镜像，未开始写入。')
    images: list[Image] = []
    offsets: set[int] = set()
    paths: set[str] = set()
    for item in items:
        if not isinstance(item, dict):
            raise BundleError('镜像记录无效，未开始写入。')
        relative, address = item.get('path'), item.get('offset')
        size, digest = item.get('size_bytes'), item.get('sha256')
        if not isinstance(relative, str) or '\\' in relative or ':' in relative:
            raise BundleError('镜像路径无效，未开始写入。')
        parts = PurePosixPath(relative)
        if parts.is_absolute() or any(part in ('.', '..') for part in relative.split('/')) or not all(relative.split('/')):
            raise BundleError('镜像路径越界，未开始写入。')
        if not isinstance(address, str) or not re.fullmatch(r'0x[0-9a-fA-F]+', address):
            raise BundleError('镜像偏移无效，未开始写入。')
        offset = int(address, 16)
        if offset in offsets or relative in paths:
            raise BundleError('镜像偏移或文件重复，未开始写入。')
        if type(size) is not int or size <= 0 or offset + size > FLASH_BYTES:
            raise BundleError('镜像大小超出目标 Flash，未开始写入。')
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest):
            raise BundleError('镜像校验信息无效，未开始写入。')
        if offset not in ARTIFACTS or ARTIFACTS[offset][0] != relative or size > ARTIFACTS[offset][1]:
            raise BundleError('镜像不符合本项目的固定分区，未开始写入。')
        source = root.joinpath(*parts.parts)
        # Reject symlinks at every component, including links that currently
        # happen to point inside the bundle.
        current = root
        for part in parts.parts:
            current /= part
            if current.is_symlink():
                raise BundleError('镜像不能使用符号链接，未开始写入。')
        try:
            resolved = source.resolve(strict=True)
            if not resolved.is_relative_to(root) or not resolved.is_file() or resolved.stat().st_size != size:
                raise BundleError('镜像文件缺失或大小不符，未开始写入。')
            data = resolved.read_bytes()
        except OSError as error:
            raise BundleError('无法读取完整镜像，未开始写入。') from error
        if hashlib.sha256(data).hexdigest() != digest:
            raise BundleError('镜像 SHA-256 不符，未开始写入。请重新复制刷机包。')
        if offset in (0x0, 0x20000):
            if len(data) < 24 or data[0] != 0xE9 or struct.unpack_from('<H', data, 12)[0] != 9:
                raise BundleError('应用或启动镜像不是 ESP32-S3，未开始写入。')
        if offset == 0x20000:
            if len(data) < 112 or struct.unpack_from('<I', data, 32)[0] != 0xABCD5432:
                raise BundleError('应用描述符无效，未开始写入。')
            image_version = data[48:80].split(b'\0', 1)[0]
            project = data[80:112].split(b'\0', 1)[0]
            if image_version != version.encode('ascii') or project != b'deskhub':
                raise BundleError('应用项目或版本与清单不符，未开始写入。')
        offsets.add(offset)
        paths.add(relative)
        images.append(Image(offset, resolved, digest, size))
    images.sort(key=lambda image: image.offset)
    if offsets != set(ARTIFACTS):
        raise BundleError('USB 包缺少必要的启动镜像，未开始写入。')
    for first, second in zip(images, images[1:]):
        if first.offset + first.size > second.offset:
            raise BundleError('镜像写入范围重叠，未开始写入。')
    return Bundle(version, tuple(images))


def require_esptool() -> None:
    try:
        version = metadata.version('esptool')
    except metadata.PackageNotFoundError as error:
        raise BundleError('请先在当前 Python 环境安装 esptool==5.3.1 与 pyserial==3.5。') from error
    if version != ESPTOOL_VERSION:
        raise BundleError('请使用包说明中的独立虚拟环境安装 esptool==5.3.1，未开始写入。')


def flash(directory: Path, port: str) -> Bundle:
    bundle = verify_bundle(directory)
    require_esptool()
    if not port.strip() or any(ord(c) < 32 for c in port):
        raise BundleError('必须明确指定串口，未开始写入。')
    base = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', port, '--no-stub']
    try:
        probe = subprocess.run(base + ['flash-id'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding='utf-8', errors='replace', timeout=45, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise BundleError('只读识别失败；检查端口/下载模式，未开始写入。') from error
    output = probe.stdout or ''
    if probe.returncode != 0 or not re.search(r'Chip type:\s*ESP32-S3\b', output):
        raise BundleError('未确认 ESP32-S3；检查端口/下载模式，未开始写入。')
    if not re.search(r'Detected flash size:\s*16\s*MB\b', output):
        raise BundleError('未确认 16 MiB Flash，未开始写入。')
    # Recheck after the reset/probe, so an incomplete copy cannot become the
    # write input while the serial connection was being established.
    if verify_bundle(directory) != bundle:
        raise BundleError('包内容在识别期间发生变化，未开始写入。')
    command = base + ['write-flash', '--flash-mode', 'dio', '--flash-size', '16MB', '--flash-freq', '80m']
    for image in bundle.images:
        command.extend((hex(image.offset), str(image.path)))
    print(f'已确认 ESP32-S3 / 16 MiB，开始安装 DeskHub {bundle.version}。', flush=True)
    # Keep esptool's live progress and its default per-image verification. Do
    # not impose an arbitrary timeout that kills an in-progress Flash write.
    try:
        written = subprocess.run(command, check=False)
    except OSError as error:
        raise BundleError('未能执行刷写；请检查工具环境。') from error
    if written.returncode != 0:
        raise BundleError('刷写或校验未成功，不能视为已安装。请按文档重新进入下载模式后重试。')
    return bundle


def list_ports() -> None:
    try:
        from serial.tools import list_ports as serial_ports
    except ImportError as error:
        raise BundleError('列出端口需要 pyserial==3.5，请先按包说明安装依赖。') from error
    ports = sorted(serial_ports.comports(), key=lambda item: item.device)
    if not ports:
        print('未发现串口。请连接可传输数据的 USB 线，必要时按官方 BOOT 流程进入下载模式。')
    for item in ports:
        chip = f'{item.vid:04x}:{item.pid:04x}' if item.vid is not None and item.pid is not None else 'unknown'
        print(f'{item.device}\t{chip}\t{item.description}')
    print('请明确选择端口后使用 --port；工具不会自动选择第一台设备。')


def _status_json_unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('duplicate field')
        value[key] = item
    return value


def _status_constant(value: str) -> None:
    raise ValueError('non-finite number')


class StatusTransport:
    """Two read-only JSON requests sharing one deadline and byte budget."""

    def __init__(self, port: Any, deadline: float):
        self.port, self.deadline, self.total = port, deadline, 0

    def remaining(self) -> float:
        seconds = self.deadline - time.monotonic()
        if seconds <= 0:
            raise BundleError('USB 状态读取超时，未确认设备状态；未执行刷写。')
        return seconds

    def request(self, operation: str) -> dict[str, Any]:
        raw = (json.dumps({'op': operation}, separators=(',', ':')) + '\n').encode('ascii')
        try:
            self.port.write_timeout = min(1.0, self.remaining())
            if self.port.write(raw) != len(raw):
                raise BundleError('USB 状态请求未完整发送，未确认设备状态；未执行刷写。')
        except BundleError:
            raise
        except Exception:
            raise BundleError('USB 状态请求发送失败，未确认设备状态；未执行刷写。') from None
        line = bytearray()
        while True:
            try:
                self.port.timeout = min(0.1, self.remaining())
                char = self.port.read(1)
            except BundleError:
                raise
            except Exception:
                raise BundleError('USB 状态连接中断，未确认设备状态；未执行刷写。') from None
            self.remaining()
            if not char:
                continue
            self.total += len(char)
            if self.total > STATUS_READ_BYTES:
                raise BundleError('USB 状态响应总量超过 128 KiB，未确认设备状态。')
            line.extend(char)
            if len(line) > STATUS_FRAME_BYTES:
                raise BundleError('USB 状态单帧超过 16 KiB，未确认设备状态。')
            if char != b'\n':
                continue
            try:
                item = json.loads(bytes(line), object_pairs_hook=_status_json_unique,
                                  parse_constant=_status_constant)
            except (ValueError, UnicodeError):
                line.clear()
                continue
            line.clear()
            # Startup logs, echoed requests and unrelated JSON are never printed.
            if not isinstance(item, dict) or type(item.get('ok')) is not bool:
                continue
            if item['ok'] is False:
                raise BundleError('设备拒绝只读状态请求，未确认设备状态；未执行刷写。')
            result = item.get('result')
            if not isinstance(result, dict) or not any(key in result for key in
                    ('device_id', 'firmware_version', 'protocol_version', 'board_model')):
                continue
            return result


def _status_identity(result: dict[str, Any]) -> tuple[str, str]:
    device_id, version = result.get('device_id'), result.get('firmware_version')
    if not isinstance(device_id, str) or not STATUS_ID.fullmatch(device_id):
        raise BundleError('USB 未返回有效设备身份，未确认设备状态。')
    if result.get('board_model') != BOARD:
        raise BundleError('USB 设备型号不匹配，未确认设备状态。')
    protocol = result.get('protocol_version')
    if type(protocol) is not str or protocol not in ('1', '1.0', '1.0.0'):
        raise BundleError('USB 设备协议不兼容，未确认设备状态。')
    if not isinstance(version, str) or not STATUS_VERSION.fullmatch(version):
        raise BundleError('USB 固件版本无效，未确认设备状态。')
    return device_id, version


def _safe_status(result: dict[str, Any], identity: tuple[str, str]) -> dict[str, Any]:
    if _status_identity(result) != identity:
        raise BundleError('两次 USB 响应的设备身份或版本不一致，未确认设备状态。')
    if type(result.get('ui_alive')) is not bool or result.get('theme') not in ('mint', 'ocean', 'amber'):
        raise BundleError('USB 显示状态字段无效，未确认设备状态。')
    rotation, revision = result.get('rotation'), result.get('config_revision')
    if type(rotation) is not int or rotation not in (0, 90, 180, 270):
        raise BundleError('USB 屏幕方向字段无效，未确认设备状态。')
    if type(revision) is not int or not 0 <= revision <= 0xFFFFFFFF:
        raise BundleError('USB 配置版本字段无效，未确认设备状态。')
    return {key: result[key] for key in
            ('device_id', 'firmware_version', 'ui_alive', 'theme', 'rotation', 'config_revision')}


def read_status(port: str) -> dict[str, Any]:
    """Read the running firmware without resetting it or loading esptool."""
    if not port.strip() or any(ord(char) < 32 for char in port):
        raise BundleError('读取状态必须明确指定有效串口；未执行刷写。')
    try:
        import serial
    except ImportError:
        raise BundleError('读取状态需要 pyserial==3.5，请先按包说明安装依赖。') from None
    deadline = time.monotonic() + STATUS_SECONDS
    connection = None
    try:
        # Configure closed-port control lines first: never pulse DTR/RTS, send
        # break, flush input, or invoke esptool's reset/download sequence.
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1.0)
        connection.dtr = False
        connection.rts = False
        connection.port = port
        connection.open()
        transport = StatusTransport(connection, deadline)
        identity = _status_identity(transport.request('identify'))
        return _safe_status(transport.request('status'), identity)
    except BundleError:
        raise
    except Exception:
        raise BundleError('无法读取 USB 状态；检查端口、占用状态与固件是否运行。未执行刷写。') from None
    finally:
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path(__file__).resolve().parent,
                        help='已完整解压的刷机包目录（默认本脚本所在目录）')
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--list', action='store_true', help='仅列出串口，不刷写')
    action.add_argument('--check', action='store_true', help='仅离线核对清单和镜像，不连接设备')
    action.add_argument('--status', action='store_true', help='只读运行中的设备状态；必须同时明确指定 --port')
    parser.add_argument('--port', help='明确指定串口；单独使用时刷写，加 --status 时仅读取状态')
    args = parser.parse_args(argv)
    if (args.list or args.check) and args.port is not None:
        parser.error('--list、--check 不能与 --port 同用。')
    if not (args.list or args.check) and not args.port:
        parser.error('刷写或 --status 都需要明确指定 --port。')
    try:
        if args.list:
            list_ports()
        elif args.check:
            bundle = verify_bundle(args.directory)
            print(f'离线校验通过：DeskHub {bundle.version}，4 个镜像尺寸与 SHA-256 一致。未连接或刷写设备。')
        elif args.status:
            status = read_status(args.port)
            print(json.dumps(status, ensure_ascii=False, indent=2))
            if 'touch' not in status:
                print('当前固件未提供触摸诊断，不能据此认定触摸没有错误。', file=sys.stderr)
        else:
            flash(args.directory, args.port)
            print('刷写及 esptool 镜像校验完成。请观察屏幕；手动 BOOT 下载后未运行时，重新上电。')
        return 0
    except KeyboardInterrupt:
        print('状态读取已中断；未执行刷写。' if args.status else
              '操作已中断，刷写未确认完成。按文档重新进入下载模式后重试。', file=sys.stderr)
        return 130
    except (BundleError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
