from __future__ import annotations

import base64
from contextlib import redirect_stderr
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

from tools import firmware_package as package


def image(version='0.2.0-dev', chip=9, project='deskhub') -> bytes:
    data = bytearray(512)
    data[0] = 0xE9
    data[1] = 1
    struct.pack_into('<H', data, 12, chip)
    struct.pack_into('<I', data, 32, 0xABCD5432)
    data[48:48 + len(version)] = version.encode('ascii')
    data[80:80 + len(project)] = project.encode('ascii')
    return bytes(data)


class SignedPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='deskhub-sign-test-')
        self.root = Path(self.temporary.name)
        self.keys = self.root / 'keys'
        package.keygen(self.keys)
        self.secret = self.keys / 'firmware-private.pem'
        self.public = self.keys / 'firmware-public.pem'
        self.app = self.root / 'app.bin'
        self.app.write_bytes(image())

    def tearDown(self):
        self.temporary.cleanup()

    def test_sign_exact_payload_verifiable_without_private_material(self):
        output = self.root / 'update.dhfw'
        result = package.sign(self.app, self.secret, output)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(set(archive.namelist()), {'app.bin', 'manifest.json'})
            envelope = json.loads(archive.read('manifest.json'))
            raw = base64.b64decode(envelope['payload'], validate=True)
            signature = base64.b64decode(envelope['signature'], validate=True)
            key = serialization.load_pem_public_key(self.public.read_bytes())
            key.verify(signature, raw, ec.ECDSA(hashes.SHA256()))
            payload = json.loads(raw)
            self.assertEqual(raw, json.dumps(payload, sort_keys=True, separators=(',', ':')).encode())
            self.assertEqual(payload['version'], '0.2.0-dev')
            self.assertEqual(payload['image_size'], len(archive.read('app.bin')))
            self.assertEqual(result['key_fingerprint'], package.public_key_pem(self.public.read_bytes())[1])
            self.assertNotIn(b'PRIVATE KEY', archive.read('manifest.json'))
        self.assertEqual(self.secret.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.keys.stat().st_mode & 0o777, 0o700)

    def test_wrong_chip_project_or_mismatched_version_rejected(self):
        for data in (image(chip=0), image(project='other'), b'invalid'):
            self.app.write_bytes(data)
            with self.assertRaises(package.PackageError):
                package.sign(self.app, self.secret, self.root / 'bad.dhfw')
        self.app.write_bytes(image())
        with self.assertRaises(package.PackageError):
            package.sign(self.app, self.secret, self.root / 'bad.dhfw', '0.9.0')
        self.assertFalse((self.root / 'bad.dhfw').exists())

    def test_existing_keys_and_packages_not_overwritten(self):
        with self.assertRaises(package.PackageError):
            package.keygen(self.keys)
        output = self.root / 'update.dhfw'
        package.sign(self.app, self.secret, output)
        original = output.read_bytes()
        with self.assertRaises(FileExistsError):
            package.sign(self.app, self.secret, output)
        self.assertEqual(output.read_bytes(), original)

    def test_wrong_curve_public_private_key_and_key_permissions_rejected(self):
        key = ec.generate_private_key(ec.SECP384R1())
        public = key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
        with self.assertRaises(package.PackageError):
            package.public_key_pem(public)
        with self.assertRaises(package.PackageError):
            package.public_key_pem(self.secret.read_bytes())
        os.chmod(self.secret, 0o644)
        with self.assertRaises(package.PackageError):
            package.private_key(self.secret)

    def test_private_key_symlink_and_error_content_not_exposed(self):
        alias = self.root / 'alias.pem'
        alias.symlink_to(self.secret)
        with self.assertRaises(OSError):
            package.private_key(alias)
        self.secret.write_text('PRIVATE-SIGNING-SENTINEL')
        error = io.StringIO()
        with redirect_stderr(error):
            result = package.main(['sign', '--key', str(self.secret), '--image', str(self.app), '--output', str(self.root / 'bad.dhfw')])
        self.assertEqual(result, 2)
        self.assertNotIn('PRIVATE-SIGNING-SENTINEL', error.getvalue())


if __name__ == '__main__':
    unittest.main()

