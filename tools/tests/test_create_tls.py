from __future__ import annotations

import importlib.util
import ipaddress
from pathlib import Path
import ssl
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("create_tls", TOOLS / "create_tls.py")
create_tls = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(create_tls)

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization
    from cryptography.x509.oid import ExtendedKeyUsageOID
except ImportError:
    x509 = None


@unittest.skipIf(x509 is None, "Use Agent's Python environment with cryptography installed")
class CertificateTests(unittest.TestCase):
    def test_valid_chain_san_key_and_private_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "tls"
            files = create_tls.generate(output, ["desk-mac.local", "localhost"], ["192.168.1.10", "::1"])
            ca = x509.load_pem_x509_certificate(files["ca.pem"].read_bytes())
            server = x509.load_pem_x509_certificate(files["server.pem"].read_bytes())
            server.verify_directly_issued_by(ca)
            self.assertTrue(ca.extensions.get_extension_for_class(x509.BasicConstraints).value.ca)
            self.assertFalse(server.extensions.get_extension_for_class(x509.BasicConstraints).value.ca)
            san = server.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
            self.assertEqual(san.get_values_for_type(x509.DNSName), ["desk-mac.local", "localhost"])
            self.assertIn(ipaddress.ip_address("192.168.1.10"), san.get_values_for_type(x509.IPAddress))
            self.assertIn(ExtendedKeyUsageOID.SERVER_AUTH, server.extensions.get_extension_for_class(x509.ExtendedKeyUsage).value)
            key = serialization.load_pem_private_key(files["server-key.pem"].read_bytes(), password=None)
            self.assertEqual(key.public_key().public_numbers(), server.public_key().public_numbers())
            self.assertEqual(files["ca-key.pem"].stat().st_mode & 0o777, 0o600)
            self.assertEqual(files["server-key.pem"].stat().st_mode & 0o777, 0o600)
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            client = ssl.create_default_context(cafile=str(files["ca.pem"]))
            self.assertTrue(client.check_hostname)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(files["server.pem"], files["server-key.pem"])

    def test_certificate_files_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "tls"
            output.mkdir(mode=0o700)
            saved = output / "server-key.pem"
            saved.write_text("existing")
            with self.assertRaises(ValueError):
                create_tls.generate(output, ["localhost"], [])
            self.assertEqual(saved.read_text(), "existing")

    def test_bad_names_no_output_written(self):
        with tempfile.TemporaryDirectory() as directory:
            for name in ("https://host", "*.local", "bad..local", "-bad.local"):
                output = Path(directory) / "tls"
                with self.subTest(name=name), self.assertRaises(ValueError):
                    create_tls.generate(output, [name], [])
                self.assertFalse(output.exists())
            with self.assertRaises(ValueError):
                create_tls.generate(Path(directory) / "tls", [], [])


if __name__ == "__main__":
    unittest.main()
