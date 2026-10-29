#!/usr/bin/env python3
"""Mutation check for `create_tls.py --ca-dir`: break the code on purpose, test_create_tls_reuse.py must go red.

Usage: scripts/mutation/create_tls_reuse.py [name-part ...]
Works on a copy in a temporary directory. The working tree is never touched.
Exit 0 only if the baseline is green and every mutation is caught.
"""
import shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VENV_PYTHON = ROOT / ".venv/bin/python"
PYTHON = str(VENV_PYTHON) if VENV_PYTHON.exists() else sys.executable
T = "tools/create_tls.py"
M = [
    ("a symlink is followed", T, 'os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)', "os.O_RDONLY"),
    ("a readable key is accepted", T, "if secret and info.st_mode & 0o077:", "if False:"),
    ("the key may belong to another certificate", T, "!= certificate.public_key().public_bytes(serialization.Encoding.DER, der):", "!= key.public_key().public_bytes(serialization.Encoding.DER, der):"),
    ("a certificate that is not a CA is accepted", T, "if not is_ca or not may_sign:", "if False:"),
    ("a CA that may not sign is accepted", T, "if not is_ca or not may_sign:", "if not is_ca:"),
    ("an ending CA is accepted", T, "MIN_CA_DAYS = 30 ", "MIN_CA_DAYS = 0 "),
    ("an expired CA is accepted", T, "if certificate.not_valid_after_utc - now < dt.timedelta(days=MIN_CA_DAYS):", "if False:"),
    ("the certificate outlives the CA", T, "min(now + dt.timedelta(days=SERVER_DAYS), ca.not_valid_after_utc)", "now + dt.timedelta(days=SERVER_DAYS)"),
    ("the CA file is not copied as it was", T, '"ca.pem": (ca_pem, 0o644)', '"ca.pem": (ca_pem + b"\\n", 0o644)'),
    ("the CA key is not copied as it was", T, '"ca-key.pem": (ca_key_pem, 0o600)', '"ca-key.pem": (ca_key_pem + b"\\n", 0o600)'),
    ("a linked directory is accepted", T, "if ca_dir.is_symlink() or not ca_dir.is_dir():", "if not ca_dir.is_dir():"),
    ("another issuer name", T, "        ca_name = ca.subject\n", '        ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Other")])\n'),
    ("a huge file is read", T, "or info.st_size > CA_FILE_LIMIT:", ":"),
    ("a certificate that has not started is accepted", T, "if certificate.not_valid_before_utc > now:", "if False:"),
    ("the CA is read after the output is made", T,
     "    reused = load_ca(ca_dir, now) if ca_dir is not None else None   # before anything is created: a refusal writes nothing\n    if output.exists():",
     "    reused = None\n    if output.exists():"),
    ("an encrypted key is not turned into a refusal", T, "except (ValueError, TypeError, UnsupportedAlgorithm):", "except (ValueError, UnsupportedAlgorithm):"),
]


def run(root):
    r = subprocess.run([PYTHON, "-m", "unittest", "tools.tests.test_create_tls_reuse", "-q"], cwd=root, capture_output=True, text=True,
                       env={"PATH": "/usr/bin:/bin", "PYTHONDONTWRITEBYTECODE": "1"})
    return r.returncode


def main():
    tmp = Path(tempfile.mkdtemp(prefix="tls_mut_"))
    shutil.copytree(ROOT / "tools", tmp / "tools", ignore=shutil.ignore_patterns("__pycache__"))
    (tmp / "agent").mkdir()
    shutil.copytree(ROOT / "agent/src", tmp / "agent/src", ignore=shutil.ignore_patterns("__pycache__"))
    base = run(tmp)
    print("baseline exit", base, flush=True)
    if base:
        return 1
    bad = ran = 0
    only = sys.argv[1:]
    for name, rel, old, new in M:
        if only and not any(o in name for o in only):
            continue
        ran += 1
        path = tmp / rel
        saved = path.read_text(encoding="utf-8")
        if old not in saved:
            print(f"SKIP     {name}: pattern not found", flush=True)
            bad += 1
            continue
        path.write_text(saved.replace(old, new, 1), encoding="utf-8")
        code = run(tmp)
        print(("SURVIVED " if code == 0 else "caught   ") + name, flush=True)
        bad += code == 0
        path.write_text(saved, encoding="utf-8")
    shutil.rmtree(tmp, ignore_errors=True)
    print("mutations:", ran, "not caught:", bad, flush=True)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
