#!/usr/bin/env python3
"""Mutation check for the administrator password: break the code on purpose, test_admin_password.py must go red.

Usage: scripts/mutation/admin_password.py [name-part ...]
Works on a copy in a temporary directory. The working tree is never touched.
Exit 0 only if the baseline is green and every mutation is caught.
"""
import shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VENV_PYTHON = ROOT / ".venv/bin/python"
PYTHON = str(VENV_PYTHON) if VENV_PYTHON.exists() else sys.executable
A = "agent/src/deskhub_agent/admin_auth.py"
M = "agent/src/deskhub_agent/management.py"
C = "agent/src/deskhub_agent/cli.py"
MUTATIONS = [
    # ---- the rules for a password
    ("minimum length 8", [(A, "MIN_LENGTH = 12", "MIN_LENGTH = 8")]),
    ("no maximum length", [(A, "MAX_LENGTH = 128", "MAX_LENGTH = 100000")]),
    ("common words allowed", [(A, "if any(part in lowered for part in BANNED_PARTS) or lowered.rstrip(\"0123456789!@#$%^&*._-\") in COMMON:", "if False:")]),
    ("common list without digits behind", [(A, " or lowered.rstrip(\"0123456789!@#$%^&*._-\") in COMMON:", ":")]),
    ("banned parts dropped", [(A, "if any(part in lowered for part in BANNED_PARTS) or", "if")]),
    ("repeated characters allowed", [(A, "if len(set(lowered)) <= 3:", "if False:")]),
    ("three different characters allowed", [(A, "if len(set(lowered)) <= 3:", "if len(set(lowered)) <= 2:")]),
    ("four different characters refused", [(A, "if len(set(lowered)) <= 3:", "if len(set(lowered)) <= 4:")]),
    ("sequences allowed", [(A, "if _longest_run(lowered) >= SEQUENCE_RUN:", "if False:")]),
    ("sequence limit nine", [(A, "SEQUENCE_RUN = 8", "SEQUENCE_RUN = 9")]),
    ("sequence limit seven", [(A, "SEQUENCE_RUN = 8", "SEQUENCE_RUN = 7")]),
    ("descending sequences allowed", [(A, "if step in (1, -1) and (direction in (0, step)):", "if step == 1 and (direction in (0, step)):")]),
    ("control characters allowed", [(A, 'if any(unicodedata.category(char) in {"Cc", "Cs", "Co", "Cn"} for char in password):', "if False:")]),
    ("lone surrogates allowed", [(A, '{"Cc", "Cs", "Co", "Cn"}', '{"Cc", "Co", "Cn"}')]),
    ("white space only allowed", [(A, "if not password.strip():", "if False:")]),
    ("the token can be the password", [(A, "if token_hashes and hashlib.sha256(password.encode(\"utf-8\")).hexdigest() in set(token_hashes):", "if False:")]),
    ("non-text accepted", [(A, "if not isinstance(password, str):", "if False:")]),
    ("spellings of one text differ", [(A, "return unicodedata.normalize(\"NFKC\", password)", "return password")]),
    # ---- the hash
    ("fixed salt", [(A, "salt = secrets.token_bytes(SALT_BYTES)", "salt = bytes(SALT_BYTES)")]),
    ("short salt", [(A, "SALT_BYTES = 16", "SALT_BYTES = 8")]),
    ("weak cost", [(A, "SCRYPT_N = 2 ** 17", "SCRYPT_N = 2 ** 8")]),
    ("weak block size", [(A, "SCRYPT_R = 8", "SCRYPT_R = 1")]),
    ("the current cost instead of the stored one", [(A, "_scrypt(prepare(password), row[\"salt\"], n, r, p)", "_scrypt(prepare(password), row[\"salt\"], SCRYPT_N, r, p)")]),
    ("any password matches", [(A, "return hmac.compare_digest(_scrypt(prepare(password), row[\"salt\"], n, r, p), row[\"hash\"])", "return True")]),
    ("no hash when nothing is set", [(A, "_scrypt(prepare(password), self.DUMMY_SALT, SCRYPT_N, SCRYPT_R, SCRYPT_P)   # the same time as a real check", "pass")]),
    # ---- the lock
    ("failures not counted", [(A, "            failures += 1\n", "            failures += 0\n")]),
    ("lock after six", [(A, "MAX_FAILURES = 5", "MAX_FAILURES = 6")]),
    ("lock after four", [(A, "MAX_FAILURES = 5", "MAX_FAILURES = 4")]),
    ("lock for one minute", [(A, "LOCK_MINUTES = 15", "LOCK_MINUTES = 1")]),
    ("lock never ends", [(A, "if until is None or until > now:", "if True:")]),
    ("lock that cannot be read is free", [(A, "if until is None or until > now:", "if until is not None and until > now:")]),
    ("lock record is not checked for format", [(A, "if not isinstance(value, str) or not TIMESTAMP.match(value):", "if False:")]),
    ("count survives an ended lock", [(A, "                failures = 0                          # the lock ran out: a new window starts\n", "                pass\n")]),
    ("success keeps the count", [(A, "                if failures or row[\"locked_until\"] is not None:\n                    self._record(row[\"generation\"], 0, None)\n", "                pass\n")]),
    ("lock not stored", [(A, "            self.store.connection.execute(\"UPDATE admin_auth SET failures=?, locked_until=? WHERE id=1 AND generation=?\",\n                                          (failures, locked_until, generation))", "            pass")]),
    ("a store fault lets the request in", [(A, "            raise _unavailable() from None\n    return guarded", "            return 0\n    return guarded")]),
    ("attempts run side by side", [(A, "        self._attempt = threading.Lock()", "        self._attempt = threading.Semaphore(100)")]),
    # ---- setting, changing, resetting
    ("set ignores the rules", [(A, "        prepared = check_password(password, token_hashes)\n        with self._attempt:", "        prepared = prepare(password)\n        with self._attempt:")]),
    ("change ignores the rules", [(A, "        prepared = check_password(password, token_hashes)\n        if prepared == prepare(current):", "        prepared = prepare(password)\n        if prepared == prepare(current):")]),
    ("change does not check the current password", [(A, "        self.verify(current)\n        return self.set_password", "        return self.set_password")]),
    ("change may keep the same password", [(A, "        if prepared == prepare(current):\n            raise PolicyError(\"same\", \"The new password must differ from the current one\")\n", "")]),
    ("set does not start a new generation", [(A, "SET generation=generation+1, salt=?, hash=?", "SET generation=generation, salt=?, hash=?")]),
    ("first-time setup may replace", [(A, "(\" AND hash IS NULL\" if only_if_unset else \"\")", "\"\"")]),
    ("reset keeps the generation", [(A, "\"UPDATE admin_auth SET generation=generation+1, salt=NULL", "\"UPDATE admin_auth SET generation=generation, salt=NULL")]),
    ("reset keeps the password", [(A, "SET generation=generation+1, salt=NULL, hash=NULL, n=NULL, r=NULL, p=NULL, failures=0", "SET generation=generation+1, failures=0")]),
    ("reset keeps the lock", [(A, "n=NULL, r=NULL, p=NULL, failures=0, locked_until=NULL, updated_at=? WHERE id=1\",\n                (stamp(),))", "n=NULL, r=NULL, p=NULL, updated_at=? WHERE id=1\",\n                (stamp(),))")]),
    ("the web has a password that is always set", [(A, "return self._read()[\"hash\"] is not None", "return True")]),
    # ---- sessions and routes
    ("a first-time session sees the whole console", [(M, "if kind == \"setup\" and not setup_ok:", "if False:")]),
    ("a browser may use the token after the password", [(M, "        if self.auth.password_set():\n            raise AgentError(\"TOKEN_LOGIN_DISABLED\"", "        if False:\n            raise AgentError(\"TOKEN_LOGIN_DISABLED\"")]),
    ("every token login is a script", [(M, "script = request.headers.get(\"origin\") is None", "script = True")]),
    ("no token login is a script", [(M, "script = request.headers.get(\"origin\") is None", "script = False")]),
    ("a script may set the password", [(M, "        if session[\"kind\"] == \"script\":\n            raise AgentError(\"FORBIDDEN\"", "        if False:\n            raise AgentError(\"FORBIDDEN\"")]),
    ("an old generation still works", [(M, "if kind != \"script\" and session[\"generation\"] != self.auth.generation():", "if False:")]),
    ("a revoked token does not end a token session", [(M, "        if kind != \"password\":   # a token session lives only as long as its token", "        if False:   # a token session lives only as long as its token")]),
    ("a password session needs a token", [(M, "        if kind != \"password\":   # a token session lives only as long as its token", "        if True:   # a token session lives only as long as its token")]),
    ("a write needs no CSRF", [(M, "        if write:\n            self.origin(request)", "        if False:\n            self.origin(request)")]),
    ("password login from any origin", [(M, "        management.origin(request)\n        rates.check(\"password:\"", "        rates.check(\"password:\"")]),
    ("password login not limited per address", [(M, "        rates.check(\"password:\" + caller(request), 10)\n", "")]),
    ("the old session stays after a change", [(M, "        management.end_session(request)\n        return start(request, management.password_session(generation))", "        return start(request, management.password_session(generation))")]),
    ("the setup refuses to look at the token", [(M, "        hashes = management.token_hashes()\n", "        hashes = []\n")]),
    ("the change may use the token", [(M, "management.auth.change_password(value.current_password, value.password, token_hashes=hashes)", "management.auth.change_password(value.current_password, value.password, token_hashes=())")]),
    ("the cookie can be read by a page", [(M, "max_age=1800, httponly=True,", "max_age=1800, httponly=False,")]),
    ("the session never ends", [(M, "\"deadline\": time.monotonic() + 1800,", "\"deadline\": time.monotonic() + 10 ** 9,")]),
    ("the reset command does nothing", [(C, "                    AdminAuth(store).reset()\n", "                    AdminAuth(store)\n")]),
]


def run(root):
    r = subprocess.run([PYTHON, "-m", "pytest", "agent/tests/test_admin_password.py", "-q", "-x", "-p", "no:cacheprovider"],
                       cwd=root, capture_output=True, text=True, env={"PYTHONPATH": str(root / "agent/src"), "PATH": "/usr/bin:/bin", "PYTHONDONTWRITEBYTECODE": "1"})
    return r.returncode


def main():
    tmp = Path(tempfile.mkdtemp(prefix="adminpw_mut_"))
    shutil.copytree(ROOT / "agent", tmp / "agent", ignore=shutil.ignore_patterns("__pycache__", "*.sqlite3*"))
    base = run(tmp)
    print("baseline exit", base, flush=True)
    if base:
        return 1
    bad = ran = 0
    only = sys.argv[1:]
    for name, edits in MUTATIONS:
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
