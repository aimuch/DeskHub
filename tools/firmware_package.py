#!/usr/bin/env python3
"""Create local P-256 signing keys and a signed, app-only DeskHub OTA package."""
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import zipfile

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

BOARD = 'ESP32-S3-Touch-AMOLED-2.16'
MAX_IMAGE = 6 * 1024 * 1024
VERSION = re.compile(r'[A-Za-z0-9][A-Za-z0-9._+\-]{0,30}\Z')


class PackageError(ValueError):
    """Fixed diagnostic text, never source contents or secret material."""


def exclusive_write(path: Path, data: bytes, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0)
    fd = os.open(path, flags, mode)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def private_key(path: Path) -> ec.EllipticCurvePrivateKey:
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
    with os.fdopen(fd, 'rb') as stream:
        details = os.fstat(stream.fileno())
        if not stat.S_ISREG(details.st_mode) or details.st_mode & 0o077:
            raise PackageError('签名私钥必须是权限 0600 的普通文件。')
        if hasattr(os, 'getuid') and details.st_uid != os.getuid():
            raise PackageError('签名私钥必须属于当前用户。')
        data = stream.read(8193)
    if len(data) > 8192:
        raise PackageError('签名私钥文件过大。')
    try:
        value = serialization.load_pem_private_key(data, password=None)
    except (ValueError, TypeError):
        raise PackageError('无法读取 P-256 PEM 签名私钥。') from None
    if not isinstance(value, ec.EllipticCurvePrivateKey) or not isinstance(value.curve, ec.SECP256R1):
        raise PackageError('签名私钥必须使用 ECDSA P-256 曲线。')
    return value


def public_key_pem(value: str | bytes) -> tuple[bytes, str]:
    raw = value.encode('ascii') if isinstance(value, str) else value
    if not raw or len(raw) > 2048 or b'PRIVATE KEY' in raw or b'\0' in raw:
        raise PackageError('更新信任根必须是最多 2 KiB 的 P-256 公钥 PEM，不能是私钥。')
    try:
        key = serialization.load_pem_public_key(raw)
    except (ValueError, TypeError):
        raise PackageError('更新公钥 PEM 无效。') from None
    if not isinstance(key, ec.EllipticCurvePublicKey) or not isinstance(key.curve, ec.SECP256R1):
        raise PackageError('更新信任根必须使用 ECDSA P-256 公钥。')
    der = key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    normalized = key.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    return normalized, hashlib.sha256(der).hexdigest()


def keygen(directory: Path) -> dict:
    if directory.is_symlink() or directory.exists() and (not directory.is_dir() or any(directory.iterdir())):
        raise PackageError('签名目录必须不存在或为空，不覆盖已有密钥。')
    if directory.exists() and directory.stat().st_mode & 0o077:
        raise PackageError('已有签名目录必须是私有目录（chmod 700）。')
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    key = ec.generate_private_key(ec.SECP256R1())
    secret = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    created: list[Path] = []
    try:
        for name, data, mode in [('firmware-private.pem', secret, 0o600), ('firmware-public.pem', public, 0o644)]:
            path = directory / name
            exclusive_write(path, data, mode)
            created.append(path)
    except BaseException:
        for path in created:
            path.unlink(missing_ok=True)
        raise
    return {'public_key_file': str((directory / 'firmware-public.pem').resolve()),
            'private_key_file': str((directory / 'firmware-private.pem').resolve()),
            'key_fingerprint': public_key_pem(public)[1]}


def image_version(image: bytes) -> str:
    # ESP-IDF v6.0.2 esp_image_header_t (24) + first segment header (8),
    # followed by esp_app_desc_t. Layout checked against the pinned IDF headers.
    if not 288 <= len(image) <= MAX_IMAGE or image[0] != 0xE9:
        raise PackageError('镜像不是有效大小的 ESP32-S3 应用镜像（最大 6 MiB）。')
    if not 1 <= image[1] <= 16 or struct.unpack_from('<H', image, 12)[0] != 9:
        raise PackageError('镜像芯片不匹配 ESP32-S3。')
    if struct.unpack_from('<I', image, 32)[0] != 0xABCD5432:
        raise PackageError('镜像缺少 ESP-IDF 应用描述符。')
    try:
        value = image[48:80].split(b'\0', 1)[0].decode('ascii')
        project = image[80:112].split(b'\0', 1)[0].decode('ascii')
    except UnicodeError:
        raise PackageError('镜像版本或项目标识无效。') from None
    if not VERSION.fullmatch(value) or project != 'deskhub':
        raise PackageError('镜像必须是带有效版本的 DeskHub 应用。')
    return value


def sign(image_path: Path, key_path: Path, output: Path, version: str | None = None) -> dict:
    if image_path.stat().st_size > MAX_IMAGE:
        raise PackageError('应用镜像超过 6 MiB 分区预算。')
    image = image_path.read_bytes()
    actual = image_version(image)
    if version is not None and version != actual:
        raise PackageError('指定版本与镜像应用描述符不一致。')
    key = private_key(key_path)
    payload = {'schema_version': 1, 'board_model': BOARD, 'chip': 'esp32s3', 'version': actual,
               'config_schema_min': 1, 'config_schema_max': 1, 'image_size': len(image),
               'image_sha256': hashlib.sha256(image).hexdigest()}
    raw = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode('utf-8')
    envelope = {'algorithm': 'ecdsa-p256-sha256', 'payload': base64.b64encode(raw).decode('ascii'),
                'signature': base64.b64encode(key.sign(raw, ec.ECDSA(hashes.SHA256()))).decode('ascii')}
    memory = io.BytesIO()
    with zipfile.ZipFile(memory, 'w', compression=zipfile.ZIP_STORED) as archive:
        archive.writestr('app.bin', image)
        archive.writestr('manifest.json', json.dumps(envelope, sort_keys=True, separators=(',', ':')).encode())
    exclusive_write(output, memory.getvalue(), 0o644)
    public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
    return {'package_file': str(output.resolve()), 'version': actual, 'image_size': len(image),
            'image_sha256': payload['image_sha256'], 'key_fingerprint': public_key_pem(public)[1],
            'hardware_verified': False}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    create = commands.add_parser('keygen', help='生成独立签名密钥，不修改系统信任库')
    create.add_argument('--directory', type=Path, required=True)
    package = commands.add_parser('sign', help='离线签名应用镜像，不执行上传/升级')
    package.add_argument('--key', type=Path, required=True)
    package.add_argument('--image', type=Path, required=True)
    package.add_argument('--output', type=Path, required=True)
    package.add_argument('--version')
    args = parser.parse_args(argv)
    try:
        result = keygen(args.directory) if args.command == 'keygen' else sign(args.image, args.key, args.output, args.version)
    except PackageError as error:
        print(str(error), file=sys.stderr)
        return 2
    except (OSError, ValueError, TypeError):
        print('无法创建签名包；检查路径、权限和密钥格式。不会输出密钥内容。', file=sys.stderr)
        return 2
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

