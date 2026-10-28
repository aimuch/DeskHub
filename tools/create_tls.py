#!/usr/bin/env python3
"""Create a private DeskHub CA and a hostname-checked server certificate.

Requires the Agent's cryptography dependency. No system trust store is modified.
"""

from __future__ import annotations

import argparse
import datetime as dt
import ipaddress
import os
from pathlib import Path
import re
import sys


def generate(output: Path, dns_names: list[str], ip_addresses: list[str]) -> dict[str, Path]:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

    if not dns_names and not ip_addresses:
        raise ValueError("必须至少指定一个设备实际使用的 DNS 名称或 IP。")
    names = []
    for value in dns_names:
        # Deliberately disallow wildcards and ambiguous URL-like names.
        labels = value.split(".")
        if len(value) > 253 or any(not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label) for label in labels):
            raise ValueError("DNS 名称无效；请输入主机名，不要输入 URL 或通配符。")
        names.append(x509.DNSName(value))
    for value in ip_addresses:
        try:
            names.append(x509.IPAddress(ipaddress.ip_address(value)))
        except ValueError:
            raise ValueError("IP 地址无效。") from None
    if output.is_symlink():
        raise ValueError("输出目录不能是符号链接。")
    if output.exists():
        if not output.is_dir() or any(output.iterdir()):
            raise ValueError("输出目录必须不存在或为空；不会覆盖现有证书。")
        if output.stat().st_mode & 0o077:
            raise ValueError("已有输出目录必须是私有目录（chmod 700）。")
    else:
        output.mkdir(parents=True, mode=0o700)
    now = dt.datetime.now(dt.timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    server_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "DeskHub local CA")])
    ca = (
        x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name)
        .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now - dt.timedelta(minutes=5)).not_valid_after(now + dt.timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=False,
            data_encipherment=False, key_agreement=False, key_cert_sign=True, crl_sign=True,
            encipher_only=False, decipher_only=False), critical=True)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(ca_key.public_key()), critical=False)
        .sign(ca_key, hashes.SHA256())
    )
    server = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "DeskHub Agent")]))
        .issuer_name(ca_name).public_key(server_key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now - dt.timedelta(minutes=5)).not_valid_after(now + dt.timedelta(days=397))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(x509.SubjectAlternativeName(names), critical=False)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=True,
            data_encipherment=False, key_agreement=False, key_cert_sign=False, crl_sign=False,
            encipher_only=False, decipher_only=False), critical=True)
        .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False)
        .sign(ca_key, hashes.SHA256())
    )
    encoding = serialization.Encoding.PEM
    private_format = serialization.PrivateFormat.PKCS8
    clear = serialization.NoEncryption()
    contents = {
        "ca.pem": (ca.public_bytes(encoding), 0o644),
        "ca-key.pem": (ca_key.private_bytes(encoding, private_format, clear), 0o600),
        "server.pem": (server.public_bytes(encoding), 0o644),
        "server-key.pem": (server_key.private_bytes(encoding, private_format, clear), 0o600),
    }
    created = []
    try:
        for name, (content, mode) in contents.items():
            path = output / name
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), mode)
            created.append(path)
            with os.fdopen(fd, "wb") as stream:
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
    except BaseException:
        for path in created:
            path.unlink(missing_ok=True)
        raise
    return {name: (output / name).resolve() for name in contents}


def main(argv=None) -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--out", required=True, type=Path, help="不存在或为空的私有目录")
    cli.add_argument("--dns", action="append", default=[], help="证书 SAN DNS 名称，可重复")
    cli.add_argument("--ip", action="append", default=[], help="证书 SAN IP 地址，可重复")
    args = cli.parse_args(argv)
    try:
        paths = generate(args.out, args.dns, args.ip)
    except ImportError:
        print("请使用已安装 Agent 依赖的 Python 环境运行此工具（需要 cryptography）。", file=sys.stderr)
        return 2
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    except (OSError, KeyboardInterrupt):
        print("无法生成 TLS 文件；检查输出目录与权限。", file=sys.stderr)
        return 2
    print(f"CA 公共证书（经 USB 写入设备）：{paths['ca.pem']}")
    print(f"Agent tls_cert：{paths['server.pem']}")
    print(f"Agent tls_key：{paths['server-key.pem']}")
    print("私钥仅保存在私有目录；系统信任库未修改。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
