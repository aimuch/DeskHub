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
import stat
import sys


CA_FILE_LIMIT = 65536      # a CA certificate or key is a few KB; more than this is not one
MIN_CA_DAYS = 30           # a CA that ends sooner than this cannot sign a certificate worth installing
SERVER_DAYS = 397


def _read_ca_file(path: Path, *, secret: bool) -> bytes:
    """One file of the old CA directory: a real file (no symlink), small, and a key only if nobody else can read it."""
    try:
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    except OSError:
        raise ValueError(f"无法读取 {path.name}：文件不存在，或者是符号链接。") from None
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > CA_FILE_LIMIT:
            raise ValueError(f"{path.name} 不是一个普通的小文件。")
        if secret and info.st_mode & 0o077:
            raise ValueError(f"{path.name} 是私钥，权限必须是 0600 或更严（chmod 600）。")
        return stream.read(CA_FILE_LIMIT + 1)


def load_ca(ca_dir: Path, now: dt.datetime):
    """The CA of an earlier run: its certificate, its key and the bytes of both files. Nothing in `ca_dir` is written."""
    from cryptography import x509
    from cryptography.exceptions import UnsupportedAlgorithm
    from cryptography.hazmat.primitives import serialization

    if ca_dir.is_symlink() or not ca_dir.is_dir():
        raise ValueError("--ca-dir 必须是一个真实的目录，不能是符号链接。")
    certificate_pem = _read_ca_file(ca_dir / "ca.pem", secret=False)
    key_pem = _read_ca_file(ca_dir / "ca-key.pem", secret=True)
    try:
        certificate = x509.load_pem_x509_certificate(certificate_pem)
    except ValueError:
        raise ValueError("ca.pem 不是一份证书。") from None
    try:
        key = serialization.load_pem_private_key(key_pem, password=None)
    except (ValueError, TypeError, UnsupportedAlgorithm):
        raise ValueError("ca-key.pem 不是一把没有加密的私钥。") from None
    der = serialization.PublicFormat.SubjectPublicKeyInfo
    if key.public_key().public_bytes(serialization.Encoding.DER, der) != certificate.public_key().public_bytes(serialization.Encoding.DER, der):
        raise ValueError("ca-key.pem 不是 ca.pem 的私钥。")
    try:
        is_ca = certificate.extensions.get_extension_for_class(x509.BasicConstraints).value.ca
        may_sign = certificate.extensions.get_extension_for_class(x509.KeyUsage).value.key_cert_sign
    except x509.ExtensionNotFound:
        raise ValueError("ca.pem 不是 CA 证书。") from None
    if not is_ca or not may_sign:
        raise ValueError("ca.pem 不是能签发证书的 CA 证书。")
    if certificate.not_valid_before_utc > now:
        raise ValueError("ca.pem 还没有生效。")
    if certificate.not_valid_after_utc - now < dt.timedelta(days=MIN_CA_DAYS):
        raise ValueError(f"CA 已经过期，或者不到 {MIN_CA_DAYS} 天就过期：用它签发的证书没有意义。请重新生成 CA 并重新配对。")
    return certificate, key, certificate_pem, key_pem


def generate(output: Path, dns_names: list[str], ip_addresses: list[str], ca_dir: Path | None = None) -> dict[str, Path]:
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
    now = dt.datetime.now(dt.timezone.utc)
    reused = load_ca(ca_dir, now) if ca_dir is not None else None   # before anything is created: a refusal writes nothing
    if output.exists():
        if not output.is_dir() or any(output.iterdir()):
            raise ValueError("输出目录必须不存在或为空；不会覆盖现有证书。")
        if output.stat().st_mode & 0o077:
            raise ValueError("已有输出目录必须是私有目录（chmod 700）。")
    else:
        output.mkdir(parents=True, mode=0o700)
    server_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    if reused is not None:
        # The CA of an earlier run signs the new certificate: devices that trust it keep trusting the Agent.
        ca, ca_key, ca_pem, ca_key_pem = reused
        ca_name = ca.subject
    else:
        ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
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
        ca_pem = ca.public_bytes(serialization.Encoding.PEM)
        ca_key_pem = ca_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    server_ends = min(now + dt.timedelta(days=SERVER_DAYS), ca.not_valid_after_utc)   # never longer than the CA that vouches for it
    server = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "DeskHub Agent")]))
        .issuer_name(ca_name).public_key(server_key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now - dt.timedelta(minutes=5)).not_valid_after(server_ends)
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
        "ca.pem": (ca_pem, 0o644),
        "ca-key.pem": (ca_key_pem, 0o600),
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
    cli.add_argument("--ca-dir", type=Path, help="沿用这个目录里已有的 ca.pem 和 ca-key.pem 签发新的服务器证书：设备和浏览器信任的 CA 不变，不用重新配对。"
                                                  "用于服务器证书到期（397 天）、或换了电脑地址")
    args = cli.parse_args(argv)
    try:
        paths = generate(args.out, args.dns, args.ip, ca_dir=args.ca_dir)
    except ImportError:
        print("请使用已安装 Agent 依赖的 Python 环境运行此工具（需要 cryptography）。", file=sys.stderr)
        return 2
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    except (OSError, KeyboardInterrupt):
        print("无法生成 TLS 文件；检查输出目录与权限。", file=sys.stderr)
        return 2
    if args.ca_dir is not None:
        print(f"沿用了 {args.ca_dir} 里的 CA：ca.pem 和 ca-key.pem 与旧的一字不差；已配对的设备和浏览器里信任的 CA 不用变。")
        print("设备里存的 Agent 地址不会变：如果地址变了，设备要重新配对，或者让新电脑用回旧地址。")
    print(f"CA 公共证书（经 USB 写入设备）：{paths['ca.pem']}")
    print(f"Agent tls_cert：{paths['server.pem']}")
    print(f"Agent tls_key：{paths['server-key.pem']}")
    print("私钥仅保存在私有目录；系统信任库未修改。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
