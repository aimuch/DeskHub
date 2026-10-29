#!/usr/bin/env python3
"""Mutation check for hotspot pairing: break the code on purpose, test_pairing_grants.py must go red.

Usage: scripts/mutation/pairing_agent.py [name-part ...]
Works on a copy in a temporary directory. The working tree is never touched.
Exit 0 only if the baseline is green and every mutation is caught.
"""
import shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VENV_PYTHON = ROOT / ".venv/bin/python"
PYTHON = str(VENV_PYTHON) if VENV_PYTHON.exists() else sys.executable
P = "agent/src/deskhub_agent/pairing.py"
M = [
    ("grants never burn", [(P, "ATTEMPTS = 5", "ATTEMPTS = 500")]),
    ("burn one attempt late", [(P, "CASE WHEN attempts_left-1<=0", "CASE WHEN attempts_left-1<0")]),
    ("wrong codes cost nothing", [(P, "SET attempts_left=attempts_left-1, state", "SET attempts_left=attempts_left, state")]),
    ("expiry not persisted", [(P, "WHERE state='pending' AND expires_at <= ?", "WHERE state='pending' AND 0 AND expires_at <= ?")]),
    ("expiry not exact", [(P, "AND expires_at <= ?", "AND expires_at < ?")]),
    ("no compare for an unknown id", [(P, "matched = hmac.compare_digest(", "matched = row is not None and hmac.compare_digest(")]),
    ("pinned device not enforced", [(P, "if not matched or wrong_device:", "if not matched:")]),
    ("replace rule dropped", [(P, 'if existing and not existing["revoked"] and not replace:', "if False:")]),
    ("grant stays reusable", [(P, "SET state='used', redeemed_device_id=?", "SET state='pending', redeemed_device_id=?")]),
    ("admin scope can be granted", [(P, 'scopes: list[Literal["read:status", "control:displays", "device:sync"]]', 'scopes: list[Literal["read:status", "control:displays", "device:sync", "admin:config"]]'),
                                    (P, 'GRANT_SCOPES = ("read:status", "control:displays", "device:sync")', 'GRANT_SCOPES = ("read:status", "control:displays", "device:sync", "admin:config")')]),
    ("unknown display accepted", [(P, "or not set(targets) <= known:", "or False:")]),
    ("pending cap removed", [(P, "if waiting >= MAX_PENDING:", "if False:")]),
    ("lifetime limit raised", [(P, "MAX_TTL_SECONDS = 300", "MAX_TTL_SECONDS = 3600")]),
    ("plain http accepted", [(P, 'if request.url.scheme != "https":', "if False:")]),
    ("a broken limiter lets the request through", [(P, 'raise AgentError("PAIRING_UNAVAILABLE", "Pairing is unavailable", 503) from error', "pass")]),
    ("package check ignored", [(P, "if not hmac.compare_digest(hashlib.sha256(body).digest()[:4], check):", "if False:")]),
    ("non-canonical package accepted", [(P, 'if base64.urlsafe_b64encode(raw).rstrip(b"=").decode() != payload:', "if False:")]),
    ("code kept in clear", [(P, "(grant_id, salt, _hash(salt, code), json.dumps(scopes)", "(grant_id, salt, code, json.dumps(scopes)")]),
    ("a gone grant answers differently", [(P, 'if row is None or row["state"] != "pending":\n                raise _reject()', 'if row is None or row["state"] != "pending":\n                raise AgentError("GRANT_GONE", "gone", 410)')]),
    ("the fingerprint is of the wrong kind", [(P, '"ca_fingerprint": hashlib.sha256(ca_der).hexdigest()', '"ca_fingerprint": hashlib.sha1(ca_der).hexdigest()')]),
    ("the package carries the wrong bytes", [(P, 'encode_package(url, grant_id, code, ca_der), "expires_at"', 'encode_package(url, grant_id, code, ca_der[:-1] + b"\\x00"), "expires_at"')]),
    ("an oversized CA is packed", [(P, 'if not der_sequence_ok(der):\n            raise AgentError("PAIRING_NOT_CONFIGURED"', 'if False:\n            raise AgentError("PAIRING_NOT_CONFIGURED"')]),
    ("the DER tag is not checked", [(P, "or der[0] != 0x30:", ":")]),
    ("a non-minimal long length is accepted", [(P, "        if length < 0x80:\n            return False  # not minimal", "        if False:\n            return False  # not minimal")]),
    ("bytes after the DER are accepted", [(P, "return header + length == len(der)", "return header + length <= len(der)")]),
    ("the stated certificate length is ignored", [(P, "if len(rest) != 20 + ca_length:", "if False:")]),
    ("the decoder accepts any DER", [(P, 'if not der_sequence_ok(ca_der):\n        raise ValueError("the certificate in the package is not one DER sequence")', 'if False:\n        raise ValueError("the certificate in the package is not one DER sequence")')]),
    ("the CA endpoint is back", [(P, '    @app.post("/api/v1/pairing/redeem")', '    @app.get("/api/v1/pairing/ca")\n    def pairing_ca() -> dict:\n        return {"ca_pem": pairing._configuration()[2]}\n\n    @app.post("/api/v1/pairing/redeem")')]),
    ("grant id too short", [(P, "GRANT_ID_LENGTH, CODE_LENGTH = 8, 10", "GRANT_ID_LENGTH, CODE_LENGTH = 6, 10")]),
    ("any grant can be cancelled", [(P, 'if row["state"] != "pending":\n                raise AgentError("GRANT_NOT_PENDING"', 'if False:\n                raise AgentError("GRANT_NOT_PENDING"')]),
    ("the list leaks the salt", [(P, '"attempts_left": row["attempts_left"]} for row in rows]', '"attempts_left": row["attempts_left"], "salt": row["salt"]} for row in rows]')]),
    ("global limit removed", [(P, 'rates.check("pairing:*", 30)', "pass")]),
    ("per source limit removed", [(P, 'rates.check("pairing:" + (request.client.host if request.client else "unknown"), 10)', "pass")]),
    ("unexpired check skipped on redeem", [(P, "            self._expire(utcnow())\n            row = self.store.connection.execute(\"SELECT * FROM pairing_grants WHERE grant_id=?\", (grant_id,)).fetchone()\n            # Always", "            row = self.store.connection.execute(\"SELECT * FROM pairing_grants WHERE grant_id=?\", (grant_id,)).fetchone()\n            # Always")]),
    ("replaced device keeps the old token", [(P, "token = self.store.pair(device_id, scopes, targets, replace=replace)", "token = self.store.pair(device_id, scopes, targets, replace=False)")]),
]


def run(root):
    r = subprocess.run([PYTHON, "-m", "pytest", "agent/tests/test_pairing_grants.py", "-q", "-x", "-p", "no:cacheprovider"],
                       cwd=root, capture_output=True, text=True, env={"PYTHONPATH": str(root / "agent/src"), "PATH": "/usr/bin:/bin", "PYTHONDONTWRITEBYTECODE": "1"})  # no stale bytecode: it is keyed by the second
    return r.returncode


def main():
    tmp = Path(tempfile.mkdtemp(prefix="pair_mut_"))
    shutil.copytree(ROOT / "agent", tmp / "agent", ignore=shutil.ignore_patterns("__pycache__", "*.sqlite3*"))
    shutil.copytree(ROOT / "tools/tests/fixtures", tmp / "tools/tests/fixtures")
    base = run(tmp)
    print("baseline exit", base, flush=True)
    if base:
        return 1
    bad = ran = 0
    only = sys.argv[1:]
    for name, edits in M:
        if only and not any(o in name for o in only):
            continue
        ran += 1
        saved = {}
        ok = True
        for rel, old, new in edits:
            path = tmp / rel
            saved.setdefault(rel, path.read_text(encoding="utf-8"))
            current = path.read_text(encoding="utf-8")
            if old not in current:
                ok = False
                break
            path.write_text(current.replace(old, new, 1), encoding="utf-8")
        if not ok:
            print(f"SKIP     {name}: pattern not found", flush=True)
            bad += 1
        else:
            code = run(tmp)
            print(("SURVIVED " if code == 0 else "caught   ") + name, flush=True)
            bad += code == 0
        for rel, text in saved.items():
            (tmp / rel).write_text(text, encoding="utf-8")
    shutil.rmtree(tmp, ignore_errors=True)
    print("mutations:", ran, "not caught:", bad, flush=True)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
