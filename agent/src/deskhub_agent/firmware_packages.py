"""Validate signed application-only packages against an independently trusted key."""
from __future__ import annotations

import base64
import hashlib
from io import BytesIO
import json
import os
from pathlib import Path
import struct
import zipfile
import zlib

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

from .errors import AgentError
from .management_models import FirmwarePayload

MAX_PACKAGE_BYTES = 7 * 1024 * 1024


def trusted_key(path: str | None):
    if not path:
        raise AgentError("UPDATE_TRUST_NOT_CONFIGURED", "Configure a local firmware verification public key first", 409)
    try:
        source = Path(path)
        if not source.is_file() or source.stat().st_size > 4096:
            raise ValueError
        key = serialization.load_pem_public_key(source.read_bytes())
        if not isinstance(key, ec.EllipticCurvePublicKey) or not isinstance(key.curve, ec.SECP256R1):
            raise ValueError
        return key
    except (OSError, ValueError, TypeError):
        raise AgentError("UPDATE_TRUST_INVALID", "Firmware verification key must be a valid local P-256 public key", 409) from None


def key_fingerprint(key) -> str:
    return hashlib.sha256(key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)).hexdigest()


def image_version(image: bytes) -> str:
    # ESP image header (24) + first segment header (8), then esp_app_desc_t.
    if len(image) < 288 or image[0] != 0xE9 or struct.unpack_from("<I", image, 32)[0] != 0xABCD5432:
        raise AgentError("INVALID_FIRMWARE_IMAGE", "Expected an ESP-IDF application image with an application descriptor", 400)
    # chip_id is uint16_t at ESP image header offset 12; ESP32-S3 = 9.
    if struct.unpack_from("<H", image, 12)[0] != 9:
        raise AgentError("WRONG_CHIP", "Application image is not for ESP32-S3", 400)
    if image[80:112].split(b"\0", 1)[0] != b"deskhub":
        raise AgentError("WRONG_APPLICATION", "Application descriptor is not a DeskHub image", 400)
    raw = image[48:80]
    if b"\0" not in raw:
        raise AgentError("INVALID_FIRMWARE_IMAGE", "Application version is not terminated", 400)
    try:
        return raw.split(b"\0", 1)[0].decode("ascii")
    except UnicodeError:
        raise AgentError("INVALID_FIRMWARE_IMAGE", "Application version is invalid", 400) from None


def validate_package(raw: bytes, public_key_path: str | None) -> tuple[dict, dict, bytes, str]:
    if not raw or len(raw) > MAX_PACKAGE_BYTES:
        raise AgentError("PAYLOAD_TOO_LARGE", "Firmware package exceeds 7 MiB", 413)
    key = trusted_key(public_key_path)
    try:
        with zipfile.ZipFile(BytesIO(raw)) as archive:
            entries = archive.infolist()
            if len(entries) != 2 or {entry.filename for entry in entries} != {"app.bin", "manifest.json"}:
                raise AgentError("INVALID_FIRMWARE_PACKAGE", "Package must contain exactly app.bin and manifest.json", 400)
            sizes = {entry.filename: entry.file_size for entry in entries}
            if sizes["manifest.json"] > 8192 or not 256 <= sizes["app.bin"] <= 6 * 1024 * 1024:
                raise AgentError("INVALID_FIRMWARE_PACKAGE", "Package contains an oversized manifest or application", 400)
            for entry in entries:
                if entry.flag_bits & 1 or entry.is_dir() or (entry.external_attr >> 16) & 0o170000 not in {0, 0o100000}:
                    raise AgentError("INVALID_FIRMWARE_PACKAGE", "Encrypted entries, directories and links are not permitted", 400)
                if entry.compress_type not in {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}:
                    raise AgentError("INVALID_FIRMWARE_PACKAGE", "Unsupported package compression", 400)
            manifest_raw = archive.read("manifest.json")
            image = archive.read("app.bin")
        def unique_object(pairs):
            result = {}
            for name, value in pairs:
                if name in result:
                    raise ValueError("duplicate manifest field")
                result[name] = value
            return result
        envelope = json.loads(manifest_raw.decode("utf-8"), object_pairs_hook=unique_object)
        if not isinstance(envelope, dict) or set(envelope) != {"algorithm", "payload", "signature"} or envelope["algorithm"] != "ecdsa-p256-sha256":
            raise ValueError
        payload_raw = base64.b64decode(envelope["payload"], validate=True)
        signature = base64.b64decode(envelope["signature"], validate=True)
        if len(payload_raw) > 2048 or not 8 <= len(signature) <= 80:
            raise ValueError
        key.verify(signature, payload_raw, ec.ECDSA(hashes.SHA256()))
        decoded = json.loads(payload_raw.decode("utf-8"), object_pairs_hook=unique_object)
        payload = FirmwarePayload.model_validate(decoded).model_dump()
        canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
        if payload_raw != canonical:
            raise AgentError("INVALID_FIRMWARE_MANIFEST", "Signed payload must use the canonical manifest encoding", 400)
    except AgentError:
        raise
    except InvalidSignature:
        raise AgentError("FIRMWARE_SIGNATURE_INVALID", "Firmware signature does not match the configured trust key", 400) from None
    except (ValueError, TypeError, KeyError, zipfile.BadZipFile, zlib.error, RuntimeError, NotImplementedError, OSError):
        raise AgentError("INVALID_FIRMWARE_PACKAGE", "Firmware package or signed manifest is invalid", 400) from None
    if payload["image_size"] != len(image) or payload["image_sha256"] != hashlib.sha256(image).hexdigest():
        raise AgentError("FIRMWARE_HASH_MISMATCH", "Application size or hash does not match the signed manifest", 400)
    if image_version(image) != payload["version"]:
        raise AgentError("FIRMWARE_VERSION_MISMATCH", "Application descriptor and signed version do not match", 400)
    return payload, envelope, image, key_fingerprint(key)


def write_image(path: Path, image: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if path.exists():
        if path.read_bytes() != image:
            raise AgentError("FIRMWARE_STORAGE_CONFLICT", "Stored firmware content differs", 409)
        return
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(image)
            output.flush()
            os.fsync(output.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise
