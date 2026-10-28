#!/usr/bin/env python3
"""Read-only, chunked ESP32-S3 16 MiB backup with a whole-device comparison.

Run with the pinned ESP-IDF environment (esptool 5.3.1). Never writes Flash,
registers controlling power, or eFuses. The original firmware is not executed
between chunks. A failed attempt retains a private .partial file.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rom', action='store_true', help='Read through the ROM loader without a RAM flasher stub')
    args = parser.parse_args()
    import esptool
    if esptool.__version__ != '5.3.1':
        raise RuntimeError('Use the pinned esptool 5.3.1 environment')
    size, block = 16 * 1024 * 1024, 256 * 1024
    output = args.output.resolve()
    partial = output.with_suffix(output.suffix + '.partial')
    if output.exists() or partial.exists():
        raise RuntimeError('Choose a new output path; existing backups are never overwritten')
    output.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if output.parent.stat().st_mode & 0o077:
        raise RuntimeError('Backup directory must be private (0700)')
    digest, sha = hashlib.md5(), hashlib.sha256()
    with esptool.detect_chip(args.port, baud=115200, connect_attempts=3) as connection:
        if connection.CHIP_NAME != 'ESP32-S3':
            raise RuntimeError('Connected chip is not ESP32-S3')
        device = connection if args.rom else esptool.run_stub(connection)
        try:
            esptool.attach_flash(device)
            if 1 << ((device.flash_id() >> 16) & 0xff) != size:
                raise RuntimeError('Detected Flash capacity is not 16 MiB')
            fd = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'wb') as stream:
                for address in range(0, size, block):
                    data = esptool.read_flash(device, address, block, flash_size='16MB', no_progress=True)
                    if not isinstance(data, bytes) or len(data) != block:
                        raise RuntimeError('Incomplete Flash chunk')
                    stream.write(data)
                    stream.flush()
                    os.fsync(stream.fileno())
                    digest.update(data)
                    sha.update(data)
                    print(f'Backup {address + block}/{size} bytes', flush=True)
            # MD5 here compares benign transfer consistency; the durable file
            # additionally gets SHA-256. No signature trust relies on MD5.
            if digest.hexdigest().lower() != device.flash_md5sum(0, size).lower():
                raise RuntimeError('Whole-device comparison differs; partial backup retained')
            os.replace(partial, output)
            record = {'size_bytes': size, 'sha256': sha.hexdigest(), 'whole_device_comparison': 'matched',
                      'chunk_bytes': block, 'reader': 'rom' if args.rom else 'stub',
                      'flash_written': False, 'efuse_written': False}
            metadata = output.with_suffix(output.suffix + '.json')
            fd = os.open(metadata, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as stream:
                json.dump(record, stream, indent=2)
                stream.write('\n')
            print(json.dumps(record, indent=2), flush=True)
        finally:
            try:
                device.hard_reset()
            except Exception:
                pass
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f'Read-only backup failed: {type(error).__name__}. No Flash write was performed.', file=sys.stderr)
        raise SystemExit(2)

