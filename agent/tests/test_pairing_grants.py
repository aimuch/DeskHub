"""Failure modes of the Agent side of hotspot pairing (docs/pairing-design.md), written before the code.

Rules: K1 (trust chain: TLS, per-device token, the device trusts the CA that the package carries), K4 (secrets: a token
or a code is never logged, never stored in clear, never shown twice), R7 (one-time, time-limited, the device is next to
the person).

S1  public_url and pairing_ca are validated; grants need both.
K1  The pairing package (version 2) is DHP2. plus base64url of: version, url, grant id, code, CA certificate (DER), check.
    It matches an independent encoder. Tampering, truncation, the old prefix, wrong alphabet, whitespace inside, padding,
    and a certificate that is not exactly one DER sequence of at most 2048 bytes: all rejected.
C1  There is no endpoint that hands out the CA: the package carries it, so no unverified TLS connection is ever needed.
G1  Creating, listing and cancelling grants needs an admin session, a CSRF token and the same origin.
G2  A grant has validated scopes (never admin:config), configured display targets (never "*"), a lifetime of 1..300 s.
G3  The grant is shown once, inside the package. The database keeps a salted hash, never the code or the package.
G4  Without a CA or a public URL there is no grant (a device could not pin the Agent). At most 8 grants wait at once.
G5  The list shows state and counters, never a secret. Cancelling works once and only for a pending grant.
R1  A good redemption returns the Agent id, a token, the CA, the scopes and targets, once. The token works.
R2  Unknown id, wrong code, expired, used, cancelled, burned, wrong device: one and the same 403 answer.
R3  Five wrong codes burn the grant; the sixth, even with the right code, fails. Counters survive a restart.
R4  A grant is used once, also when twenty threads redeem it at the same moment. A used grant stays used after restart.
R5  A paired device id needs replace in the grant; without it the grant is not consumed. With it, the old token dies.
R6  Expiry is absolute: exactly at expires_at the grant is gone, and moving the clock back gives nothing back.
R7  Rate limits per source and in total; a broken limiter refuses, it never lets a request through.
R8  Malformed bodies are rejected before any lookup and cost no attempt. Plain http is refused.
R9  No secret in logs, error text, the list, the device record or the database. The compare is constant-time even
    for an id that does not exist. Redemption needs no credential and ignores a junk Authorization header.
"""
from __future__ import annotations

import base64
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
import hashlib
import json
import logging
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import serialization
from fastapi.testclient import TestClient
from pydantic import ValidationError
import pytest

from deskhub_agent.app import create_app
from deskhub_agent.errors import AgentError
from deskhub_agent.management_models import DeviceConfiguration
from deskhub_agent.models import Settings, stamp
from deskhub_agent.storage import Store

ROOT = Path(__file__).resolve().parents[2]
CA = ROOT / "tools/tests/fixtures/ca.pem"
ORIGIN = "https://testserver"
URL = "https://10.10.10.5:8765"
ALPHABET = set("0123456789ABCDEFGHJKMNPQRSTVWXYZ")
SCOPES = ["read:status", "control:displays", "device:sync"]
DEVICE = "deskhub-aabbccddeeff"
CA_DER = x509.load_pem_x509_certificate(CA.read_bytes()).public_bytes(serialization.Encoding.DER)
FINGERPRINT = hashlib.sha256(CA_DER).digest()
TINY = bytes([0x30, 0x03, 0x02, 0x01, 0x05])  # the smallest DER sequence these tests use


def fake_der(total: int) -> bytes:
    """A DER SEQUENCE of exactly `total` bytes: a valid header with the shortest length form, zero bytes inside."""
    content = total - 2
    if content < 128:
        return bytes([0x30, content]) + bytes(content)
    content = total - 3
    if content < 256:
        return bytes([0x30, 0x81, content]) + bytes(content)
    content = total - 4
    return bytes([0x30, 0x82, content >> 8, content & 255]) + bytes(content)


# ---- an independent encoder and decoder of the package, the oracle of the format ----
def oracle_encode(url: str, grant_id: str, code: str, der: bytes, version: int = 2, ca_len: int | None = None) -> str:
    raw = url.encode()
    body = bytes([version, len(raw)]) + raw + grant_id.encode() + code.encode() + (len(der) if ca_len is None else ca_len).to_bytes(2, "big") + der
    return "DHP2." + base64.urlsafe_b64encode(body + hashlib.sha256(body).digest()[:4]).rstrip(b"=").decode()


def oracle_decode(text: str) -> dict:
    assert text.startswith("DHP2.")
    payload = text[5:]
    raw = base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4))
    body, check = raw[:-4], raw[-4:]
    assert hashlib.sha256(body).digest()[:4] == check
    assert body[0] == 2
    size = body[1]
    rest = body[2 + size:]
    length = int.from_bytes(rest[18:20], "big")
    assert len(rest) == 20 + length
    return {"url": body[2:2 + size].decode(), "grant_id": rest[:8].decode(), "code": rest[8:18].decode(), "ca_der": rest[20:]}


class Clock:
    def __init__(self):
        self.now = datetime(2026, 10, 28, 12, 0, 0, tzinfo=timezone.utc)

    def __call__(self):
        return self.now

    def advance(self, seconds: float):
        self.now += timedelta(seconds=seconds)


class World:
    def __init__(self, client, app, store, settings, clock, headers):
        self.client, self.app, self.store, self.settings, self.clock, self.headers = client, app, store, settings, clock, headers

    def create(self, **changes):
        body = {"scopes": SCOPES, "targets": ["monitor-desk-01"]}
        body.update(changes)
        return self.client.post("/api/v1/admin/pairing-grants", json=body, headers=self.headers)

    def grant(self, **changes):
        response = self.create(**changes)
        assert response.status_code == 201, response.text
        data = response.json()
        return data, oracle_decode(data["package"])

    def redeem(self, grant_id, code, device_id=DEVICE, client=None, headers=None, **extra):
        return (client or self.client).post("/api/v1/pairing/redeem", json={"grant_id": grant_id, "code": code, "device_id": device_id, **extra},
                                            headers=headers or {})

    def listing(self):
        response = self.client.get("/api/v1/admin/pairing-grants")
        assert response.status_code == 200, response.text
        return {item["grant_id"]: item for item in response.json()["grants"]}

    def wrong(self, grant_id, code, times=1):
        wrong = "".join("0" if c != "0" else "1" for c in code)
        return [self.redeem(grant_id, wrong) for _ in range(times)]


@pytest.fixture
def clock(monkeypatch):
    value = Clock()
    monkeypatch.setattr("deskhub_agent.pairing.utcnow", value)
    return value


@pytest.fixture
def factory(settings, clock, tmp_path):
    stores = []

    @contextmanager
    def build(**overrides):
        values = {"pairing_ca": str(CA), "public_url": URL, "allowed_networks": ["0.0.0.0/0", "127.0.0.0/8"]}
        values.update(overrides)
        for key, value in values.items():
            setattr(settings, key, value)
        store = Store(settings.db_path)
        stores.append(store)
        admin_token = store.pair("browser-admin", ["admin:config"], ["*"])
        app = create_app(settings, store=store)
        with TestClient(app, base_url=ORIGIN) as client:
            session = client.post("/api/v1/admin/session", json={"token": admin_token}, headers={"Origin": ORIGIN})
            assert session.status_code == 200, session.text
            headers = {"Origin": ORIGIN, "X-CSRF-Token": session.json()["csrf_token"]}
            yield World(client, app, store, settings, clock, headers)

    yield build
    for store in stores:
        store.close()


@pytest.fixture
def world(factory):
    with factory() as value:
        yield value


def dump(store) -> str:
    return "\n".join(store.connection.iterdump())


# ---------------------------------------------------------------- S1 settings
@pytest.mark.parametrize("url", ["https://10.10.10.5:8765", "https://desk-mac.local:8765", "https://[::1]:8765", "https://desk.example", "https://10.0.0.1"])
def test_public_url_accepts_an_https_origin(url):
    assert Settings(agent_id="agent-x", public_url=url).public_url == url


@pytest.mark.parametrize("url", ["http://10.10.10.5:8765", "https://x/api", "https://x/", "https://x?y=1", "https://user:pw@x", "https://x#f", "",
                                 "ftp://x", "https://", "https://x:99999", "https://x:0", "https://" + "a" * 130, "https://x\n", "https://x y", "//x"])
def test_public_url_rejects_everything_else(url):
    with pytest.raises(ValidationError):
        Settings(agent_id="agent-x", public_url=url)


def test_pairing_is_off_by_default():
    settings = Settings(agent_id="agent-x")
    assert settings.public_url is None and settings.pairing_ca is None


# ---------------------------------------------------------------- K1 package
def test_package_matches_the_independent_encoder(clock):
    from deskhub_agent.pairing import decode_package, encode_package
    text = encode_package(URL, "ABCD2345", "KM7QWXYZ09", CA_DER)
    assert text == oracle_encode(URL, "ABCD2345", "KM7QWXYZ09", CA_DER)
    assert text.startswith("DHP2.") and 1000 <= len(text) <= 1200
    assert decode_package(text) == {"url": URL, "grant_id": "ABCD2345", "code": "KM7QWXYZ09", "ca_der": CA_DER}
    assert decode_package("  " + text + "\r\n") == decode_package(text)  # copy and paste adds white space around it


def test_package_decoder_rejects_every_single_character_change(clock):
    from deskhub_agent.pairing import decode_package
    text = oracle_encode(URL, "ABCD2345", "KM7QWXYZ09", CA_DER)
    for index in range(5, len(text)):
        changed = text[:index] + ("A" if text[index] != "A" else "B") + text[index + 1:]
        with pytest.raises(ValueError):
            decode_package(changed)


@pytest.mark.parametrize("size", [300, 301])
def test_the_unused_bits_of_the_last_character_must_be_zero(clock, size):
    from deskhub_agent.pairing import decode_package
    text = oracle_encode(URL, "ABCD2345", "KM7QWXYZ09", fake_der(size))
    table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    raw_length = len(base64.urlsafe_b64decode(text[5:] + "=" * (-len(text[5:]) % 4)))
    spare = {1: 4, 2: 2}[raw_length % 3]                          # the last character carries two or four spare bits
    last = table.index(text[-1])
    assert last % (1 << spare) == 0
    for extra in range(1, 1 << spare):                            # the same bytes, spelled with other spare bits
        with pytest.raises(ValueError):
            decode_package(text[:-1] + table[last + extra])


@pytest.mark.parametrize("make", [
    lambda t: t[:-1], lambda t: t[:-4], lambda t: t + "A", lambda t: t + "AAAA", lambda t: "DHP1." + t[5:], lambda t: "dhp2." + t[5:],
    lambda t: t[5:], lambda t: "DHP2.", lambda t: "", lambda t: t[:20] + "\n" + t[20:], lambda t: t[:20] + " " + t[20:],
    lambda t: t.replace("-", "+").replace("_", "/") if ("-" in t or "_" in t) else t + "+", lambda t: t + "=", lambda t: "DHP2." + t[5:].lower(),
    lambda t: "DHP2." + "A" * 120, lambda t: "DHP2." + t[5:300],
])
def test_package_decoder_rejects_malformed_text(clock, make):
    from deskhub_agent.pairing import decode_package
    text = oracle_encode(URL, "ABCD2345", "KM7QWXYZ09", CA_DER)
    with pytest.raises(ValueError):
        decode_package(make(text))


@pytest.mark.parametrize("url", ["http://10.10.10.5:8765", "https://x/api", "https://u:p@x", "https://x?q=1", "https://x#f", "", "https://" + "a" * 130])
def test_package_decoder_rejects_a_bad_url_even_with_a_valid_check(clock, url):
    from deskhub_agent.pairing import decode_package
    with pytest.raises(ValueError):
        decode_package(oracle_encode(url, "ABCD2345", "KM7QWXYZ09", CA_DER))


@pytest.mark.parametrize("version", [0, 1, 3, 255])
def test_package_decoder_rejects_another_version(clock, version):
    from deskhub_agent.pairing import decode_package
    with pytest.raises(ValueError):
        decode_package(oracle_encode(URL, "ABCD2345", "KM7QWXYZ09", CA_DER, version=version))


BAD_DER = {
    "empty": b"", "not a sequence": b"\x31\x03\x02\x01\x05", "an integer": b"\x02\x01\x05", "length longer than the data": bytes([0x30, 0x09, 0x02, 0x01, 0x05]),
    "bytes after the sequence": TINY + b"\x00\x00", "length shorter than the data": bytes([0x30, 0x01, 0x02, 0x01, 0x05]),
    "indefinite length": bytes([0x30, 0x80, 0x02, 0x01, 0x05, 0x00, 0x00]), "a long form for a short length": bytes([0x30, 0x81, 0x03, 0x02, 0x01, 0x05]),
    "a two byte length for a one byte length": bytes([0x30, 0x82, 0x00, 0xC8]) + bytes(200), "three length bytes": bytes([0x30, 0x83, 0x00, 0x00, 0x03, 0x02, 0x01, 0x05]),
    "only a tag": b"\x30", "tag and a half length": b"\x30\x82\x01", "above 2048 bytes": fake_der(2049), "far above 2048 bytes": fake_der(5000),
}


@pytest.mark.parametrize("name", list(BAD_DER))
def test_package_decoder_and_encoder_refuse_a_certificate_that_is_not_one_der_sequence(clock, name):
    from deskhub_agent.pairing import decode_package, encode_package
    with pytest.raises(ValueError):
        decode_package(oracle_encode(URL, "ABCD2345", "KM7QWXYZ09", BAD_DER[name], ca_len=len(BAD_DER[name]) if len(BAD_DER[name]) < 65536 else 65535))
    with pytest.raises(ValueError):
        encode_package(URL, "ABCD2345", "KM7QWXYZ09", BAD_DER[name])


@pytest.mark.parametrize("stated", [0, 1, 4, 6, 100, 65535])
def test_package_decoder_refuses_a_stated_certificate_length_that_is_wrong(clock, stated):
    from deskhub_agent.pairing import decode_package
    with pytest.raises(ValueError):
        decode_package(oracle_encode(URL, "ABCD2345", "KM7QWXYZ09", TINY, ca_len=stated))


def test_package_decoder_refuses_bytes_after_the_stated_certificate_length(clock):
    from deskhub_agent.pairing import decode_package
    # the first five bytes are a good certificate of exactly the stated length; two more bytes follow it inside the package
    with pytest.raises(ValueError):
        decode_package(oracle_encode(URL, "ABCD2345", "KM7QWXYZ09", TINY + b"\x00\x00", ca_len=len(TINY)))


@pytest.mark.parametrize("size", [5, 129, 131, 200, 258, 260, 764, 1500, 2048])
def test_every_der_length_form_up_to_2048_bytes_is_accepted(clock, size):
    from deskhub_agent.pairing import decode_package, encode_package
    der = fake_der(size)
    text = encode_package(URL, "ABCD2345", "KM7QWXYZ09", der)
    assert text == oracle_encode(URL, "ABCD2345", "KM7QWXYZ09", der)
    assert decode_package(text)["ca_der"] == der


@pytest.mark.parametrize("grant_id,code", [
    ("ABCD234", "KM7QWXYZ09"), ("ABCD23456", "KM7QWXYZ09"), ("ABCD2345", "KM7QWXYZ0"),
    ("ABCD2345", "KM7QWXYZ090"), ("abcd2345", "KM7QWXYZ09"), ("ABCD2345", "km7qwxyz09"),
    ("ABCI2345", "KM7QWXYZ09"), ("ABCD2345", "KM7QWXYZ0L"), ("ABCD2345", "KM7QWXYZ0O"),
    ("ABCD2345", "KM7QWXYZ0U"),
])
def test_package_encoder_refuses_bad_fields(clock, grant_id, code):
    from deskhub_agent.pairing import encode_package
    with pytest.raises(ValueError):
        encode_package(URL, grant_id, code, CA_DER)


def test_the_certificate_of_the_package_is_the_configured_ca(clock):
    from deskhub_agent.pairing import ca_der_of, ca_fingerprint
    assert ca_der_of(CA.read_text()) == CA_DER and ca_fingerprint(CA.read_text()) == FINGERPRINT
    for bad in ("", "garbage", "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n"):
        with pytest.raises(ValueError):
            ca_der_of(bad)
        with pytest.raises(ValueError):
            ca_fingerprint(bad)


# ---------------------------------------------------------------- G1 access
def test_grants_need_an_admin_session_csrf_and_origin(factory):
    with factory() as w:
        anonymous = TestClient(w.app, base_url=ORIGIN)
        body = {"scopes": SCOPES, "targets": ["monitor-desk-01"]}
        assert anonymous.post("/api/v1/admin/pairing-grants", json=body, headers={"Origin": ORIGIN}).status_code == 401
        assert anonymous.get("/api/v1/admin/pairing-grants").status_code == 401
        assert anonymous.delete("/api/v1/admin/pairing-grants/ABCD2345", headers={"Origin": ORIGIN}).status_code == 401
        assert w.client.post("/api/v1/admin/pairing-grants", json=body, headers={"Origin": ORIGIN}).status_code == 403            # no CSRF
        assert w.client.post("/api/v1/admin/pairing-grants", json=body, headers={**w.headers, "X-CSRF-Token": "wrong"}).status_code == 403
        assert w.client.post("/api/v1/admin/pairing-grants", json=body, headers={**w.headers, "Origin": "https://evil.example"}).status_code == 403
        assert w.client.get("/api/v1/admin/pairing-grants").json() == {"grants": []}                                                 # nothing was created


def test_a_device_credential_cannot_manage_grants(factory):
    with factory() as w:
        token = w.store.pair("terminal-one", SCOPES, ["monitor-desk-01"])
        device = TestClient(w.app, base_url=ORIGIN)
        response = device.post("/api/v1/admin/pairing-grants", json={"scopes": SCOPES, "targets": ["monitor-desk-01"]},
                               headers={"Authorization": "Bearer " + token, "Origin": ORIGIN})
        assert response.status_code in {401, 403}
        assert device.get("/api/v1/admin/pairing-grants", headers={"Authorization": "Bearer " + token}).status_code in {401, 403}


# ---------------------------------------------------------------- G2 validation and G3 secrets
def test_a_grant_is_shown_once_inside_the_package(world):
    data, package = world.grant()
    assert set(data) == {"grant_id", "package", "expires_at", "ca_fingerprint", "url", "scopes", "targets", "replace", "device_id"}
    assert package["url"] == URL == data["url"] and package["grant_id"] == data["grant_id"]
    assert package["ca_der"] == CA_DER and data["ca_fingerprint"] == FINGERPRINT.hex()   # the fingerprint is only for a person to compare
    assert data["expires_at"] == stamp(world.clock.now + timedelta(seconds=300))
    assert data["scopes"] == SCOPES and data["targets"] == ["monitor-desk-01"] and data["replace"] is False and data["device_id"] is None
    assert package["code"] not in json.dumps({key: value for key, value in data.items() if key != "package"})
    assert set(package["code"]) <= ALPHABET and set(package["grant_id"]) <= ALPHABET


def test_secrets_are_never_stored_in_clear(world):
    data, package = world.grant()
    text = dump(world.store)
    assert package["code"] not in text and data["package"] not in text and package["code"].lower() not in text.lower()
    assert data["grant_id"] in text
    row = world.store.connection.execute("SELECT * FROM pairing_grants WHERE grant_id=?", (data["grant_id"],)).fetchone()
    assert len(row["code_hash"]) == 64 and len(row["salt"]) >= 16 and row["code_hash"] != hashlib.sha256(package["code"].encode()).hexdigest()


def test_codes_and_ids_are_random_and_use_the_clear_alphabet(world):
    seen_codes, seen_ids = set(), set()
    for _ in range(40):
        data, package = world.grant()
        assert set(package["code"]) <= ALPHABET and len(package["code"]) == 10
        assert set(package["grant_id"]) <= ALPHABET and len(package["grant_id"]) == 8
        seen_codes.add(package["code"])
        seen_ids.add(package["grant_id"])
        assert world.client.delete("/api/v1/admin/pairing-grants/" + data["grant_id"], headers=world.headers).status_code == 200
    assert len(seen_codes) == 40 and len(seen_ids) == 40


@pytest.mark.parametrize("changes", [
    {"scopes": []}, {"scopes": ["admin:config"]}, {"scopes": ["read:status", "admin:config"]}, {"scopes": ["root"]}, {"scopes": ["read:status", "read:status"]},
    {"scopes": "read:status"}, {"scopes": None}, {"scopes": ["control:displays"], "targets": []},
    {"targets": ["*"]}, {"targets": ["monitor-unknown"]}, {"targets": ["monitor-desk-01", "monitor-desk-01"]}, {"targets": "monitor-desk-01"},
    {"targets": ["bad id"]}, {"targets": ["a"] * 5},
    {"ttl_seconds": 0}, {"ttl_seconds": -1}, {"ttl_seconds": 301}, {"ttl_seconds": 3600}, {"ttl_seconds": True}, {"ttl_seconds": "60"}, {"ttl_seconds": 60.5},
    {"replace": "yes"}, {"replace": 1}, {"device_id": "bad id"}, {"device_id": ""}, {"device_id": "x" * 97}, {"device_id": 5},
    {"extra": 1}, {"code": "KM7QWXYZ09"},
])
def test_grant_requests_are_validated(world, changes):
    assert world.create(**changes).status_code == 400
    assert world.listing() == {}


def test_valid_variants_are_accepted(world):
    for changes in ({"scopes": ["device:sync"], "targets": []}, {"scopes": ["read:status"], "targets": []}, {"ttl_seconds": 1}, {"ttl_seconds": 300},
                    {"replace": True}, {"device_id": DEVICE}, {"scopes": SCOPES}):
        response = world.create(**changes)
        assert response.status_code == 201, (changes, response.text)
        assert world.client.delete("/api/v1/admin/pairing-grants/" + response.json()["grant_id"], headers=world.headers).status_code == 200
    data, _ = world.grant(ttl_seconds=60)
    assert data["expires_at"] == stamp(world.clock.now + timedelta(seconds=60))


# ---------------------------------------------------------------- G4 availability
@pytest.mark.parametrize("overrides", [{"pairing_ca": None}, {"public_url": None}, {"pairing_ca": "/nonexistent/ca.pem"}])
def test_no_grant_without_a_ca_and_a_public_url(factory, overrides):
    with factory(**overrides) as w:
        response = w.create()
        assert response.status_code == 409 and response.json()["error"]["code"] == "PAIRING_NOT_CONFIGURED"


def test_a_private_key_or_garbage_is_not_a_ca(factory, tmp_path):
    for name, content in (("key.pem", "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n"), ("junk.pem", "not a certificate"), ("empty.pem", "")):
        path = tmp_path / name
        path.write_text(content)
        with factory(pairing_ca=str(path), db_path=str(tmp_path / (name + ".sqlite3"))) as w:
            assert w.create().status_code == 409


def test_a_ca_too_large_for_the_package_gives_no_grant(factory, tmp_path):
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "big test ca")])
    now = datetime(2026, 1, 1, tzinfo=timezone.utc)
    builder = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key()).serial_number(1)
               .not_valid_before(now).not_valid_after(now + timedelta(days=30)).add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True))
    big = builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(f"host{i:03d}.example.test") for i in range(120)]), critical=False).sign(key, hashes.SHA256())
    der = big.public_bytes(serialization.Encoding.DER)
    assert len(der) > 2048                                       # the test certificate really is too large
    path = tmp_path / "big.pem"
    path.write_bytes(big.public_bytes(serialization.Encoding.PEM))
    with factory(pairing_ca=str(path), db_path=str(tmp_path / "big.sqlite3")) as w:
        response = w.create()
        assert response.status_code == 409 and response.json()["error"]["code"] == "PAIRING_NOT_CONFIGURED"
        assert w.listing() == {}                                 # nothing was stored for a package that cannot exist


def test_at_most_eight_grants_wait_at_once(world):
    ids = [world.grant()[0]["grant_id"] for _ in range(8)]
    refused = world.create()
    assert refused.status_code == 429 and refused.json()["error"]["code"] == "TOO_MANY_GRANTS"
    world.client.delete("/api/v1/admin/pairing-grants/" + ids[0], headers=world.headers)
    assert world.create().status_code == 201                    # a cancelled grant frees its place
    world.clock.advance(301)
    assert world.create().status_code == 201                    # expired grants do not count


# ---------------------------------------------------------------- G5 list and cancel
def test_listing_shows_state_and_never_a_secret(world):
    pending, p = world.grant()
    cancelled, c = world.grant()
    used, u = world.grant()
    burned, b = world.grant()
    expired, e = world.grant(ttl_seconds=1)
    world.client.delete("/api/v1/admin/pairing-grants/" + cancelled["grant_id"], headers=world.headers)
    assert world.redeem(used["grant_id"], u["code"]).status_code == 200
    world.wrong(burned["grant_id"], b["code"], times=5)
    world.clock.advance(2)
    items = world.listing()
    assert {key: item["state"] for key, item in items.items()} == {
        pending["grant_id"]: "pending", cancelled["grant_id"]: "cancelled", used["grant_id"]: "used", burned["grant_id"]: "burned", expired["grant_id"]: "expired"}
    assert items[used["grant_id"]]["redeemed_device_id"] == DEVICE and items[used["grant_id"]]["redeemed_at"]
    assert items[pending["grant_id"]]["attempts_left"] == 5 and items[burned["grant_id"]]["attempts_left"] == 0
    text = json.dumps(items)
    for secret in (p["code"], c["code"], u["code"], b["code"], e["code"], pending["package"], "salt", "code_hash", "token"):
        assert secret not in text
    assert set(items[pending["grant_id"]]) == {"grant_id", "state", "created_at", "expires_at", "scopes", "targets", "replace", "device_id",
                                               "redeemed_device_id", "redeemed_at", "attempts_left"}


def test_cancelling_works_once_and_only_for_a_pending_grant(world):
    data, package = world.grant()
    assert world.client.delete("/api/v1/admin/pairing-grants/" + data["grant_id"]).status_code == 403          # CSRF is needed
    assert world.client.delete("/api/v1/admin/pairing-grants/" + data["grant_id"], headers=world.headers).json()["state"] == "cancelled"
    assert world.client.delete("/api/v1/admin/pairing-grants/" + data["grant_id"], headers=world.headers).status_code == 409
    assert world.client.delete("/api/v1/admin/pairing-grants/ZZZZ9999", headers=world.headers).status_code == 404
    assert world.client.delete("/api/v1/admin/pairing-grants/bad id", headers=world.headers).status_code in {400, 404}
    assert world.redeem(data["grant_id"], package["code"]).status_code == 403
    used, u = world.grant()
    assert world.redeem(used["grant_id"], u["code"]).status_code == 200
    assert world.client.delete("/api/v1/admin/pairing-grants/" + used["grant_id"], headers=world.headers).status_code == 409


# ---------------------------------------------------------------- R1 success
def test_a_good_redemption_returns_everything_once_and_the_token_works(world):
    data, package = world.grant(targets=["monitor-desk-01"])
    response = world.redeem(data["grant_id"], package["code"])
    assert response.status_code == 200, response.text
    body = response.json()
    assert set(body) == {"agent_id", "token", "ca_pem", "scopes", "targets"}
    assert body["agent_id"] == world.settings.agent_id and body["ca_pem"] == CA.read_text()
    assert body["scopes"] == SCOPES and body["targets"] == ["monitor-desk-01"] and len(body["token"]) >= 32
    token = body["token"]
    assert world.client.get("/api/v1/status", headers={"Authorization": "Bearer " + token}).status_code == 200
    report = {"firmware_version": "0.3.0", "board_model": "ESP32-S3-Touch-AMOLED-2.16", "boot_id": "boot-one", "uptime_seconds": 1, "free_heap_bytes": 1,
              "applied_revision": 0, "configuration": DeviceConfiguration().model_dump(), "config_error": None,
              "power": {"vbus": "present", "battery": "absent", "charging": "idle", "fault": False},
              "capabilities": {"management": True, "signed_ota": False, "layouts": ["desk", "clock", "focus"], "update_key_fingerprint": None}, "update": None}
    assert world.client.post("/api/v1/device/sync", json=report, headers={"Authorization": "Bearer " + token}).status_code == 200
    record = world.client.get("/api/v1/admin/devices/" + DEVICE).json()
    assert record["scopes"] == SCOPES and record["targets"] == ["monitor-desk-01"]
    assert "token" not in json.dumps(record)
    assert world.listing()[data["grant_id"]]["state"] == "used"
    assert world.store.connection.execute("SELECT token_hash FROM devices WHERE device_id=?", (DEVICE,)).fetchone()[0] == hashlib.sha256(token.encode()).hexdigest()
    assert token not in dump(world.store)


def test_a_device_gets_only_the_granted_scopes(world):
    data, package = world.grant(scopes=["read:status"], targets=[])
    token = world.redeem(data["grant_id"], package["code"]).json()["token"]
    assert world.client.get("/api/v1/status", headers={"Authorization": "Bearer " + token}).status_code == 200
    assert world.client.post("/api/v1/device/sync", json={}, headers={"Authorization": "Bearer " + token}).status_code in {400, 403}
    device = TestClient(world.app, base_url=ORIGIN)                                              # no admin cookie on this one
    assert device.get("/api/v1/admin/devices", headers={"Authorization": "Bearer " + token}).status_code in {401, 403}   # never admin:config


def test_redemption_needs_no_credential_and_ignores_a_junk_header(world):
    data, package = world.grant()
    assert world.redeem(data["grant_id"], package["code"], headers={"Authorization": "Bearer not-a-token-at-all-xxxxxxxxxx"}).status_code == 200


# ---------------------------------------------------------------- R2 uniform rejection
def test_every_refusal_looks_the_same(world):
    answers = []

    def note(response):
        assert response.status_code == 403, response.text
        body = response.json()["error"]
        answers.append((response.status_code, body["code"], body["message"]))

    live, lv = world.grant()
    note(world.redeem("ZZZZ9999", lv["code"]))                                  # unknown id
    note(world.wrong(live["grant_id"], lv["code"])[0])                          # wrong code
    expired, ev = world.grant(ttl_seconds=1)
    used, uv = world.grant()
    cancelled, cv = world.grant()
    burned, bv = world.grant()
    pinned, pv = world.grant(device_id="deskhub-111111111111")
    assert world.redeem(used["grant_id"], uv["code"]).status_code == 200
    world.client.delete("/api/v1/admin/pairing-grants/" + cancelled["grant_id"], headers=world.headers)
    world.wrong(burned["grant_id"], bv["code"], times=5)
    world.clock.advance(2)
    world.app.state.rates.buckets.clear()                                       # the limiter has its own test below
    note(world.redeem(expired["grant_id"], ev["code"], device_id="deskhub-222222222222"))
    note(world.redeem(used["grant_id"], uv["code"], device_id="deskhub-333333333333"))
    note(world.redeem(cancelled["grant_id"], cv["code"], device_id="deskhub-444444444444"))
    note(world.redeem(burned["grant_id"], bv["code"], device_id="deskhub-555555555555"))
    note(world.redeem(pinned["grant_id"], pv["code"], device_id="deskhub-666666666666"))   # the right code, the wrong device
    assert len(set(answers)) == 1
    assert answers[0][1] == "PAIRING_REJECTED"
    assert world.store.connection.execute("SELECT count(*) FROM devices WHERE device_id LIKE 'deskhub-%'").fetchone()[0] == 1   # only the used one


# ---------------------------------------------------------------- R3 attempts
def test_five_wrong_codes_burn_the_grant(world):
    data, package = world.grant()
    for expected in (4, 3, 2, 1, 0):
        assert world.wrong(data["grant_id"], package["code"])[0].status_code == 403
        assert world.listing()[data["grant_id"]]["attempts_left"] == expected
    assert world.listing()[data["grant_id"]]["state"] == "burned"
    assert world.redeem(data["grant_id"], package["code"]).status_code == 403                  # the sixth, with the right code
    assert world.listing()[data["grant_id"]]["state"] == "burned"
    assert world.store.connection.execute("SELECT count(*) FROM devices WHERE device_id=?", (DEVICE,)).fetchone()[0] == 0


def test_a_right_code_after_four_wrong_ones_still_works(world):
    data, package = world.grant()
    world.wrong(data["grant_id"], package["code"], times=4)
    assert world.redeem(data["grant_id"], package["code"]).status_code == 200


def test_counters_and_states_survive_a_restart(factory):
    from deskhub_agent.pairing import Pairing
    with factory() as w:
        data, package = w.grant()
        used, uv = w.grant()
        w.wrong(data["grant_id"], package["code"], times=3)
        assert w.redeem(used["grant_id"], uv["code"]).status_code == 200
        restarted = {item["grant_id"]: item for item in Pairing(w.settings, w.store).list()}
        assert restarted[data["grant_id"]]["attempts_left"] == 2 and restarted[data["grant_id"]]["state"] == "pending"
        assert restarted[used["grant_id"]]["state"] == "used"
        again = Pairing(w.settings, w.store)
        with pytest.raises(AgentError) as error:
            again.redeem(used["grant_id"], uv["code"], "deskhub-777777777777")
        assert error.value.code == "PAIRING_REJECTED" and error.value.status == 403


def test_an_unknown_id_creates_nothing(world):
    before = dump(world.store)
    for _ in range(5):
        assert world.redeem("ZZZZ9999", "0123456789").status_code == 403
    assert dump(world.store) == before


def test_a_device_pinned_in_the_grant_must_match_and_a_mismatch_costs_an_attempt(world):
    data, package = world.grant(device_id=DEVICE)
    assert world.redeem(data["grant_id"], package["code"], device_id="deskhub-111111111111").status_code == 403
    assert world.listing()[data["grant_id"]]["attempts_left"] == 4
    assert world.redeem(data["grant_id"], package["code"], device_id=DEVICE).status_code == 200


# ---------------------------------------------------------------- R4 single use
def test_a_grant_is_used_once(world):
    data, package = world.grant()
    assert world.redeem(data["grant_id"], package["code"]).status_code == 200
    assert world.redeem(data["grant_id"], package["code"]).status_code == 403
    assert world.redeem(data["grant_id"], package["code"], device_id="deskhub-111111111111").status_code == 403


def test_twenty_threads_redeem_a_grant_exactly_once(world):
    data, package = world.grant(replace=True)
    pairing = world.app.state.pairing

    def attempt(index):
        try:
            return pairing.redeem(data["grant_id"], package["code"], f"deskhub-{index:012d}")
        except AgentError as error:
            return error.code

    with ThreadPoolExecutor(max_workers=20) as pool:
        results = list(pool.map(attempt, range(20)))
    wins = [item for item in results if isinstance(item, dict)]
    assert len(wins) == 1 and results.count("PAIRING_REJECTED") == 19
    assert world.store.connection.execute("SELECT count(*) FROM devices WHERE device_id LIKE 'deskhub-0%'").fetchone()[0] == 1


# ---------------------------------------------------------------- R5 replace
def test_a_paired_device_id_needs_replace_and_the_grant_is_not_consumed(world):
    old_token = world.store.pair(DEVICE, SCOPES, ["monitor-desk-01"])
    data, package = world.grant()
    assert world.redeem(data["grant_id"], package["code"]).status_code == 403
    item = world.listing()[data["grant_id"]]
    assert item["state"] == "pending" and item["attempts_left"] == 5                    # not consumed, no attempt lost
    assert world.store.authenticate(old_token)["device_id"] == DEVICE                    # the old token still works
    assert world.redeem(data["grant_id"], package["code"], device_id="deskhub-888888888888").status_code == 200


def test_replace_gives_a_new_token_and_kills_the_old_one(world):
    old_token = world.store.pair(DEVICE, SCOPES, ["monitor-desk-01"])
    data, package = world.grant(replace=True)
    new_token = world.redeem(data["grant_id"], package["code"]).json()["token"]
    assert new_token != old_token
    with pytest.raises(AgentError):
        world.store.authenticate(old_token)
    assert world.store.authenticate(new_token)["device_id"] == DEVICE


def test_a_revoked_device_id_can_be_paired_again_without_replace(world):
    world.store.pair(DEVICE, SCOPES, ["monitor-desk-01"])
    world.store.revoke(DEVICE)
    data, package = world.grant()
    assert world.redeem(data["grant_id"], package["code"]).status_code == 200


# ---------------------------------------------------------------- R6 expiry
def test_expiry_is_absolute_and_exact(world):
    data, package = world.grant(ttl_seconds=60)
    world.clock.advance(59.999)
    inside, ip = world.grant(ttl_seconds=60)
    world.clock.advance(0.001)                                                           # exactly at expires_at of the first one
    assert world.redeem(data["grant_id"], package["code"]).status_code == 403
    assert world.listing()[data["grant_id"]]["state"] == "expired"
    world.clock.advance(-1000)                                                           # moving the clock back gives nothing back
    assert world.redeem(data["grant_id"], package["code"]).status_code == 403
    assert world.listing()[inside["grant_id"]]["state"] == "pending"
    assert world.redeem(inside["grant_id"], ip["code"]).status_code == 200               # still inside its own lifetime


def test_an_expired_grant_costs_no_attempt(world):
    data, package = world.grant(ttl_seconds=1)
    world.clock.advance(5)
    world.wrong(data["grant_id"], package["code"], times=3)
    assert world.listing()[data["grant_id"]]["attempts_left"] == 5


# ---------------------------------------------------------------- R7 limits
def test_one_source_is_limited_and_another_is_not(world):
    data, package = world.grant()
    busy = TestClient(world.app, base_url=ORIGIN, client=("203.0.113.9", 50000))
    other = TestClient(world.app, base_url=ORIGIN, client=("203.0.113.10", 50000))
    statuses = [world.redeem("ZZZZ9999", "0123456789", client=busy).status_code for _ in range(12)]
    assert statuses[:10] == [403] * 10 and statuses[10:] == [429, 429]
    assert world.redeem(data["grant_id"], package["code"], client=other).status_code == 200


def test_all_sources_together_are_limited_too(world):
    statuses = []
    for index in range(35):
        client = TestClient(world.app, base_url=ORIGIN, client=(f"203.0.113.{index + 1}", 50000))
        statuses.append(world.redeem("ZZZZ9999", "0123456789", client=client).status_code)
    assert statuses[:30] == [403] * 30 and set(statuses[30:]) == {429}


def test_a_broken_limiter_refuses_and_touches_nothing(world):
    data, package = world.grant()
    original = world.app.state.rates.check

    def broken(key, maximum=120):
        if key.startswith("pairing"):
            raise RuntimeError("limiter fault")
        return original(key, maximum)

    world.app.state.rates.check = broken
    try:
        response = world.redeem(data["grant_id"], package["code"])
    finally:
        world.app.state.rates.check = original
    assert response.status_code == 503 and response.json()["error"]["code"] == "PAIRING_UNAVAILABLE"
    item = world.listing()[data["grant_id"]]
    assert item["state"] == "pending" and item["attempts_left"] == 5
    assert world.store.connection.execute("SELECT count(*) FROM devices WHERE device_id=?", (DEVICE,)).fetchone()[0] == 0


# ---------------------------------------------------------------- R8 malformed requests and plain http
@pytest.mark.parametrize("body", [
    {}, {"grant_id": "ABCD2345"}, {"code": "0123456789"}, {"device_id": DEVICE},
    {"grant_id": "ABCD234", "code": "0123456789", "device_id": DEVICE}, {"grant_id": "ABCD23456", "code": "0123456789", "device_id": DEVICE},
    {"grant_id": "abcd2345", "code": "0123456789", "device_id": DEVICE}, {"grant_id": "ABCD2345", "code": "012345678", "device_id": DEVICE},
    {"grant_id": "ABCD2345", "code": "01234567890", "device_id": DEVICE}, {"grant_id": "ABCD2345", "code": "012345678a", "device_id": DEVICE},
    {"grant_id": "ABCD2345", "code": "01234567IL", "device_id": DEVICE}, {"grant_id": "ABCD2345", "code": "0123456789", "device_id": "bad id"},
    {"grant_id": "ABCD2345", "code": "0123456789", "device_id": ""}, {"grant_id": "ABCD2345", "code": "0123456789", "device_id": "x" * 97},
    {"grant_id": 12345678, "code": "0123456789", "device_id": DEVICE}, {"grant_id": "ABCD2345", "code": None, "device_id": DEVICE},
    {"grant_id": "ABCD2345", "code": "0123456789", "device_id": DEVICE, "replace": True}, {"grant_id": "ABCD2345", "code": ["0123456789"], "device_id": DEVICE},
])
def test_malformed_bodies_are_refused_before_any_lookup_and_cost_nothing(world, body):
    data, package = world.grant()
    before = dump(world.store)
    response = world.client.post("/api/v1/pairing/redeem", json=body)
    assert response.status_code == 400
    assert dump(world.store) == before
    assert world.listing()[data["grant_id"]]["attempts_left"] == 5


def test_an_oversized_body_is_refused(world):
    response = world.client.post("/api/v1/pairing/redeem", content=json.dumps({"grant_id": "ABCD2345", "code": "0123456789", "device_id": "a" * 9000}),
                                 headers={"Content-Type": "application/json"})
    assert response.status_code in {400, 413}


def test_plain_http_is_refused_and_costs_nothing(factory):
    with factory() as w:
        data, package = w.grant()
        plain = TestClient(w.app, base_url="http://testserver")
        response = plain.post("/api/v1/pairing/redeem", json={"grant_id": data["grant_id"], "code": package["code"], "device_id": DEVICE})
        assert response.status_code == 400 and response.json()["error"]["code"] == "HTTPS_REQUIRED"
        assert w.listing()[data["grant_id"]]["state"] == "pending" and w.listing()[data["grant_id"]]["attempts_left"] == 5


# ---------------------------------------------------------------- R9 secrets
def test_no_secret_reaches_the_logs_or_an_error(world, caplog):
    caplog.set_level(logging.DEBUG)
    data, package = world.grant()
    wrong = world.wrong(data["grant_id"], package["code"], times=2)
    good = world.redeem(data["grant_id"], package["code"])
    token = good.json()["token"]
    assert all(package["code"] not in item.text and item.json()["error"]["code"] == "PAIRING_REJECTED" for item in wrong)
    for secret in (package["code"], token, data["package"]):
        assert secret not in caplog.text
    again = world.redeem(data["grant_id"], package["code"], device_id="deskhub-999999999999")
    assert package["code"] not in again.text and token not in again.text


def test_the_token_is_never_shown_again(world):
    data, package = world.grant()
    token = world.redeem(data["grant_id"], package["code"]).json()["token"]
    for path in ("/api/v1/admin/devices", "/api/v1/admin/devices/" + DEVICE, "/api/v1/admin/pairing-grants", "/api/v1/admin/overview"):
        assert token not in world.client.get(path).text


def test_the_compare_is_constant_time_even_for_an_id_that_does_not_exist(world, monkeypatch):
    import hmac
    calls = []
    real = hmac.compare_digest

    def counting(a, b):
        calls.append(1)
        return real(a, b)

    monkeypatch.setattr("deskhub_agent.pairing.hmac.compare_digest", counting)
    world.redeem("ZZZZ9999", "0123456789")
    assert calls, "an unknown id must still go through the same comparison"
    known, kv = world.grant()
    before = len(calls)
    world.wrong(known["grant_id"], kv["code"])
    assert len(calls) > before


# ---------------------------------------------------------------- C1: nothing hands out the CA
# The package carries the CA, so the device never needs an unverified TLS connection to fetch it. The endpoint that did
# that was removed; nothing should answer there, and nothing about it should be in the contract.
def test_there_is_no_endpoint_that_hands_out_the_ca(world):
    for method in ("get", "post", "put"):
        assert getattr(world.client, method)("/api/v1/pairing/ca").status_code in {404, 405}
    assert not any("pairing/ca" in path for path in world.app.openapi()["paths"])
    assert world.client.get("/api/v1/pairing/ca").text.find("BEGIN CERTIFICATE") == -1


def test_the_only_unauthenticated_pairing_route_is_the_redemption(world):
    paths = {path: sorted(methods) for path, methods in world.app.openapi()["paths"].items() if "pairing" in path and "admin" not in path}
    assert paths == {"/api/v1/pairing/redeem": ["post"]}
