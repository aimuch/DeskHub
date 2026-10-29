"""Failure modes of `create_tls.py --ca-dir`, written before the code.

Why it exists: the server certificate lives 397 days. When it ends, every paired device refuses the Agent, because a device
trusts only the CA of its pairing. Issuing a new server certificate from the SAME CA keeps every device, every pairing
package and the CA the browser already trusts. It also lets a new computer take over with a new certificate.

R1  The new server certificate is issued by the old CA: it verifies against the old ca.pem; ca.pem and ca-key.pem in the new
    directory are the old ones, byte for byte; the server key and certificate are new; the SAN has the new names only.
R2  A real handshake: a client that trusts only the OLD ca.pem accepts the NEW certificate for its address, with the host
    name check on, and refuses it for another address. This is the device's view.
R3  Validity: the new certificate never outlives the CA. A CA with less than 30 days left is refused (nothing written);
    one with more than 30 days but less than 397 gives a certificate that ends with the CA.
R4  Refusals write nothing and change nothing in the old directory: no directory, no ca.pem, no ca-key.pem, a ca.pem
    that is not a certificate, a certificate that is not a CA, a CA without keyCertSign, a key that does not belong to it,
    a key that is not a private key, an expired CA, a key readable by group or others, a symlink for either file, a
    directory that is a file, an oversized file, the same directory as the output.
R5  The old directory is never written: every file keeps its bytes and mode.
R6  Permissions of the new directory: 0700; both private keys 0600; the certificates 0644.
R7  The command line: --ca-dir works; a refusal exits with 2 and a message and prints no key; the output never contains
    private key material.
R8  Without --ca-dir nothing changes (the tests of test_create_tls.py).
"""
from __future__ import annotations

import contextlib
import datetime as dt
import importlib.util
import io
import ipaddress
from pathlib import Path
import socket
import ssl
import tempfile
import threading
import unittest

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("create_tls", TOOLS / "create_tls.py")
create_tls = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(create_tls)

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
except ImportError:
    x509 = None

PEM = serialization.Encoding.PEM if x509 else None


def make_ca(directory: Path, *, days_left: int = 3650, age_days: int = 0, ca: bool = True, key_cert_sign: bool = True, foreign_key: bool = False) -> None:
    """A CA in `directory` as create_tls writes it, with a validity of our choosing."""
    directory.mkdir(mode=0o700)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "DeskHub local CA")])
    now = dt.datetime.now(dt.timezone.utc)
    certificate = (
        x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now - dt.timedelta(days=age_days, minutes=5)).not_valid_after(now + dt.timedelta(days=days_left))
        .add_extension(x509.BasicConstraints(ca=ca, path_length=0 if ca else None), critical=True)
        .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=False, data_encipherment=False, key_agreement=False,
                                     key_cert_sign=key_cert_sign, crl_sign=False, encipher_only=False, decipher_only=False), critical=True)
        .sign(key, hashes.SHA256())
    )
    written = rsa.generate_private_key(public_exponent=65537, key_size=2048) if foreign_key else key
    (directory / "ca.pem").write_bytes(certificate.public_bytes(PEM))
    (directory / "ca-key.pem").write_bytes(written.private_bytes(PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    (directory / "ca-key.pem").chmod(0o600)


def snapshot(directory: Path) -> dict[str, tuple[bytes, int]]:
    return {path.name: (path.read_bytes(), path.stat().st_mode & 0o777) for path in sorted(directory.iterdir())}


@unittest.skipIf(x509 is None, "Use Agent's Python environment with cryptography installed")
class ReuseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.old = self.root / "old"
        create_tls.generate(self.old, ["old-mac.local"], ["10.10.10.5"])

    def tearDown(self):
        self.temp.cleanup()

    def renew(self, out="new", dns=(), ips=("10.10.10.77",), source=None):
        return create_tls.generate(self.root / out, list(dns), list(ips), ca_dir=source or self.old)

    def test_r1_the_new_certificate_is_issued_by_the_old_ca(self):
        files = self.renew(dns=["new-mac.local"])
        old_ca = x509.load_pem_x509_certificate((self.old / "ca.pem").read_bytes())
        server = x509.load_pem_x509_certificate(files["server.pem"].read_bytes())
        server.verify_directly_issued_by(old_ca)
        self.assertEqual(files["ca.pem"].read_bytes(), (self.old / "ca.pem").read_bytes())
        self.assertEqual(files["ca-key.pem"].read_bytes(), (self.old / "ca-key.pem").read_bytes())
        self.assertNotEqual(files["server.pem"].read_bytes(), (self.old / "server.pem").read_bytes())
        self.assertNotEqual(files["server-key.pem"].read_bytes(), (self.old / "server-key.pem").read_bytes())
        san = server.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
        self.assertEqual(san.get_values_for_type(x509.DNSName), ["new-mac.local"])
        self.assertEqual(san.get_values_for_type(x509.IPAddress), [ipaddress.ip_address("10.10.10.77")])
        key = serialization.load_pem_private_key(files["server-key.pem"].read_bytes(), password=None)
        self.assertEqual(key.public_key().public_numbers(), server.public_key().public_numbers())
        self.assertFalse(server.extensions.get_extension_for_class(x509.BasicConstraints).value.ca)

    def test_r2_a_device_that_trusts_only_the_old_ca_accepts_the_new_certificate(self):
        files = self.renew(ips=["127.0.0.1"])
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.load_cert_chain(files["server.pem"], files["server-key.pem"])
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(4)
        port = listener.getsockname()[1]

        def serve():
            for _ in range(2):
                try:
                    connection, _ = listener.accept()
                    with server_context.wrap_socket(connection, server_side=True) as tls:
                        tls.recv(1)
                except (ssl.SSLError, OSError):
                    pass
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        device = ssl.create_default_context(cafile=str(self.old / "ca.pem"))   # the device: one CA, host name checked
        self.assertTrue(device.check_hostname and device.verify_mode == ssl.CERT_REQUIRED)
        with socket.create_connection(("127.0.0.1", port), timeout=5) as raw, device.wrap_socket(raw, server_hostname="127.0.0.1") as tls:
            tls.send(b"x")
        with self.assertRaises(ssl.SSLCertVerificationError):          # the certificate does not name this address
            with socket.create_connection(("127.0.0.1", port), timeout=5) as raw:
                device.wrap_socket(raw, server_hostname="10.10.10.99")
        listener.close()
        thread.join(timeout=5)

    def test_r3_the_certificate_never_outlives_the_ca(self):
        far = self.renew(out="far")
        server = x509.load_pem_x509_certificate(far["server.pem"].read_bytes())
        ca = x509.load_pem_x509_certificate(far["ca.pem"].read_bytes())
        self.assertLessEqual(server.not_valid_after_utc, ca.not_valid_after_utc)
        self.assertGreater(server.not_valid_after_utc - dt.datetime.now(dt.timezone.utc), dt.timedelta(days=390))
        short = self.root / "short"
        make_ca(short, days_left=100)
        clamped = self.renew(out="clamped", source=short)
        server = x509.load_pem_x509_certificate(clamped["server.pem"].read_bytes())
        ca = x509.load_pem_x509_certificate(clamped["ca.pem"].read_bytes())
        self.assertLessEqual(server.not_valid_after_utc, ca.not_valid_after_utc)
        self.assertLess(server.not_valid_after_utc - dt.datetime.now(dt.timezone.utc), dt.timedelta(days=101))
        self.assertGreater(server.not_valid_after_utc - dt.datetime.now(dt.timezone.utc), dt.timedelta(days=90))
        ending = self.root / "ending"
        make_ca(ending, days_left=20)
        self.assert_refused(ending)

    def assert_refused(self, source: Path, out="refused"):
        before = snapshot(source) if source.is_dir() and not source.is_symlink() else None
        with self.assertRaises(ValueError):
            create_tls.generate(self.root / out, [], ["10.10.10.77"], ca_dir=source)
        self.assertFalse((self.root / out).exists(), "a refusal writes nothing")
        if before is not None:
            self.assertEqual(snapshot(source), before, "a refusal changes nothing in the old directory")

    def test_r4_refusals_write_nothing(self):
        self.assert_refused(self.root / "missing")
        file_not_dir = self.root / "plain"
        file_not_dir.write_text("x")
        self.assert_refused(file_not_dir)
        for missing in ("ca.pem", "ca-key.pem"):
            broken = self.root / f"no-{missing}"
            make_ca(broken)
            (broken / missing).unlink()
            self.assert_refused(broken)
        garbage = self.root / "garbage"
        make_ca(garbage)
        (garbage / "ca.pem").write_text("not a certificate")
        self.assert_refused(garbage)
        not_ca = self.root / "not-ca"
        make_ca(not_ca, ca=False)
        self.assert_refused(not_ca)
        no_sign = self.root / "no-sign"
        make_ca(no_sign, key_cert_sign=False)
        self.assert_refused(no_sign)
        wrong_key = self.root / "wrong-key"
        make_ca(wrong_key, foreign_key=True)
        self.assert_refused(wrong_key)
        not_a_key = self.root / "not-a-key"
        make_ca(not_a_key)
        (not_a_key / "ca-key.pem").write_bytes((self.old / "ca.pem").read_bytes())
        (not_a_key / "ca-key.pem").chmod(0o600)
        self.assert_refused(not_a_key)
        expired = self.root / "expired"
        make_ca(expired, days_left=-1, age_days=400)
        self.assert_refused(expired)
        future = self.root / "future"
        make_ca(future, age_days=-5)                      # a certificate that starts in five days
        self.assert_refused(future)
        encrypted = self.root / "encrypted"
        make_ca(encrypted)
        key = serialization.load_pem_private_key((encrypted / "ca-key.pem").read_bytes(), password=None)
        (encrypted / "ca-key.pem").write_bytes(key.private_bytes(PEM, serialization.PrivateFormat.PKCS8, serialization.BestAvailableEncryption(b"secret")))
        self.assert_refused(encrypted)                    # a key that needs a password is refused, not asked for
        for mode in (0o640, 0o604, 0o644, 0o660):
            readable = self.root / f"readable-{mode:o}"
            make_ca(readable)
            (readable / "ca-key.pem").chmod(mode)
            self.assert_refused(readable)
        for name in ("ca.pem", "ca-key.pem"):
            linked = self.root / f"link-{name}"
            make_ca(linked)
            real = self.root / f"real-{name}"
            real.write_bytes((linked / name).read_bytes())
            real.chmod(0o600)
            (linked / name).unlink()
            (linked / name).symlink_to(real)
            self.assert_refused(linked)
        big = self.root / "big"
        make_ca(big)
        (big / "ca.pem").write_bytes((big / "ca.pem").read_bytes() + b"#" * 70000)
        self.assert_refused(big)
        link_dir = self.root / "dirlink"
        link_dir.symlink_to(self.old)
        self.assert_refused(link_dir)

    def test_r4_the_output_rules_still_hold(self):
        out = self.root / "occupied"
        out.mkdir(mode=0o700)
        (out / "server.pem").write_text("keep")
        with self.assertRaises(ValueError):
            create_tls.generate(out, [], ["10.10.10.77"], ca_dir=self.old)
        self.assertEqual((out / "server.pem").read_text(), "keep")
        with self.assertRaises(ValueError):
            create_tls.generate(self.old, [], ["10.10.10.77"], ca_dir=self.old)       # the same directory
        with self.assertRaises(ValueError):
            create_tls.generate(self.root / "nameless", [], [], ca_dir=self.old)       # still needs an address
        self.assertFalse((self.root / "nameless").exists())
        with self.assertRaises(ValueError):
            create_tls.generate(self.root / "wild", ["*.local"], [], ca_dir=self.old)

    def test_r5_the_old_directory_is_never_written(self):
        before = snapshot(self.old)
        self.renew()
        self.renew(out="second")
        self.assertEqual(snapshot(self.old), before)

    def test_r6_permissions(self):
        files = self.renew()
        self.assertEqual((self.root / "new").stat().st_mode & 0o777, 0o700)
        self.assertEqual(files["ca-key.pem"].stat().st_mode & 0o777, 0o600)
        self.assertEqual(files["server-key.pem"].stat().st_mode & 0o777, 0o600)
        self.assertEqual(files["ca.pem"].stat().st_mode & 0o777, 0o644)
        self.assertEqual(files["server.pem"].stat().st_mode & 0o777, 0o644)

    def test_r7_command_line(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = create_tls.main(["--out", str(self.root / "cli"), "--ip", "10.10.10.88", "--ca-dir", str(self.old)])
        self.assertEqual(code, 0)
        self.assertIn("沿用", out.getvalue())
        self.assertNotIn("PRIVATE KEY", out.getvalue() + err.getvalue())
        self.assertTrue((self.root / "cli/server.pem").is_file())
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = create_tls.main(["--out", str(self.root / "cli2"), "--ip", "10.10.10.88", "--ca-dir", str(self.root / "missing")])
        self.assertEqual(code, 2)
        self.assertTrue(err.getvalue().strip())
        self.assertNotIn("PRIVATE KEY", out.getvalue() + err.getvalue())
        self.assertFalse((self.root / "cli2").exists())


if __name__ == "__main__":
    unittest.main()
