from __future__ import annotations

import os
from pathlib import Path
from uuid import uuid4

from .models import Settings


def private_write(path: Path, content: str) -> None:
    """Atomic write with no group/world access; never use this for logs."""
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(f".{path.name}.{uuid4().hex}.tmp")
    try:
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def load_settings(path: Path) -> Settings:
    path = Path(path).resolve()
    if path.stat().st_size > 65536:
        raise ValueError("configuration exceeds 64 KiB")
    settings = Settings.model_validate_json(path.read_text(encoding="utf-8"))
    for field in ("db_path", "tls_cert", "tls_key", "firmware_public_key"):
        value = getattr(settings, field)
        if value:
            resolved = Path(value).expanduser()
            if not resolved.is_absolute():
                resolved = path.parent / resolved
            setattr(settings, field, str(resolved.resolve()))
    return settings


def initialize(path: Path) -> Settings:
    if path.exists():
        raise ValueError("configuration already exists")
    settings = Settings(agent_id=f"agent-{uuid4().hex}")
    private_write(path, settings.model_dump_json(indent=2) + "\n")
    return load_settings(path)
