"""The administrator password for the web console (docs/rules.md R8, decision D10).

The password is the only way into the console once it is set. The module keeps three promises:
  - Only a slow hash (scrypt, own salt) is stored. The password itself is never written, returned or logged.
  - Five wrong tries lock the account for 15 minutes. The count and the lock live in the database, so a restart does not
    clear them.
  - Anything unclear (a store that fails, a lock record that cannot be read) means "no". It never means "yes".
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
import functools
import hashlib
import hmac
import logging
import re
import secrets
import threading
import unicodedata

from .errors import AgentError
from .models import stamp, utcnow

logger = logging.getLogger("deskhub.auth")

MIN_LENGTH = 12
MAX_LENGTH = 128
MAX_FAILURES = 5
LOCK_MINUTES = 15
# OWASP first choice for scrypt: N=2^17, r=8, p=1 (128 MiB, a few tenths of a second). Tests lower N only.
SCRYPT_N = 2 ** 17
SCRYPT_R = 8
SCRYPT_P = 1
SALT_BYTES = 16
HASH_BYTES = 32

BANNED_PARTS = ("password", "deskhub", "admin", "qwerty", "12345678", "iloveyou", "letmein", "welcome", "changeme")
COMMON = frozenset("""
    123456 1234567 12345678 123456789 1234567890 111111 000000 654321 666666 121212 112233 abc123 qwerty qwertyuiop
    asdfgh asdfghjkl zxcvbn zxcvbnm password passw0rd p4ssword letmein welcome monkey dragon master login princess
    sunshine football baseball iloveyou trustno1 superman batman shadow michael hello freedom whatever starwars
    secret administrator
""".split())
SEQUENCE_RUN = 8
TIMESTAMP = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")

SCHEMA = """
    CREATE TABLE IF NOT EXISTS admin_auth (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        generation INTEGER NOT NULL,
        salt BLOB, hash BLOB, n INTEGER, r INTEGER, p INTEGER,
        failures INTEGER NOT NULL DEFAULT 0,
        locked_until TEXT,
        updated_at TEXT NOT NULL
    );
"""


class PolicyError(ValueError):
    """The password is refused. The message names the rule, never the password."""

    def __init__(self, reason: str, message: str):
        super().__init__(message)
        self.reason, self.message = reason, message


def prepare(password: str) -> str:
    """The form that is hashed: the same text in every Unicode spelling of it."""
    return unicodedata.normalize("NFKC", password)


def check_password(password, token_hashes=None) -> str:
    """Raise PolicyError when the password is refused. Return the form that is hashed."""
    if not isinstance(password, str):
        raise PolicyError("type", "The password must be text")
    password = prepare(password)
    if len(password) < MIN_LENGTH:
        raise PolicyError("too_short", f"Use at least {MIN_LENGTH} characters")
    if len(password) > MAX_LENGTH:
        raise PolicyError("too_long", f"Use at most {MAX_LENGTH} characters")
    if any(unicodedata.category(char) in {"Cc", "Cs", "Co", "Cn"} for char in password):
        raise PolicyError("characters", "Do not use control characters")
    if not password.strip():
        raise PolicyError("blank", "The password cannot be only white space")
    lowered = password.lower()
    if len(set(lowered)) <= 3:
        raise PolicyError("repeated", "Use more different characters")
    if _longest_run(lowered) >= SEQUENCE_RUN:
        raise PolicyError("sequence", "Do not use a long run such as abcdefgh or 12345678")
    if any(part in lowered for part in BANNED_PARTS) or lowered.rstrip("0123456789!@#$%^&*._-") in COMMON:
        raise PolicyError("common", "This password is too common or too easy to guess")
    if token_hashes and hashlib.sha256(password.encode("utf-8")).hexdigest() in set(token_hashes):
        raise PolicyError("token", "The password cannot be the management token")
    return password


def _longest_run(text: str) -> int:
    """Longest stretch whose characters step by +1 or by -1 in code point order."""
    best = run = 1
    direction = 0
    for before, after in zip(text, text[1:]):
        step = ord(after) - ord(before)
        if step in (1, -1) and (direction in (0, step)):
            run, direction = run + 1, step
        elif step in (1, -1):
            run, direction = 2, step
        else:
            run, direction = 1, 0
        best = max(best, run)
    return best


def _octets(prepared: str) -> bytes:
    return prepared.encode("utf-8", "surrogatepass")   # never raises: login input is arbitrary text


def _scrypt(prepared: str, salt: bytes, n: int, r: int, p: int) -> bytes:
    return hashlib.scrypt(_octets(prepared), salt=salt, n=n, r=r, p=p, dklen=HASH_BYTES, maxmem=2 * 128 * n * r)


def _unavailable() -> AgentError:
    return AgentError("AUTH_UNAVAILABLE", "Sign-in is not available now; try again later", 503, True)


def _fail_closed(function):
    """A store or hashing fault answers "unavailable". It never answers "allowed"."""
    @functools.wraps(function)
    def guarded(*args, **kwargs):
        try:
            return function(*args, **kwargs)
        except (AgentError, PolicyError):
            raise
        except Exception as error:
            logger.error("administrator password store failed: %s", type(error).__name__)
            raise _unavailable() from None
    return guarded


class AdminAuth:
    DUMMY_SALT = bytes(SALT_BYTES)

    def __init__(self, store, clock=utcnow):
        self.store, self.clock = store, clock
        self._attempt = threading.Lock()   # one hash at a time: 128 MiB each, and the count stays exact
        with store.lock:
            store.connection.executescript(SCHEMA)
            store.connection.execute("INSERT OR IGNORE INTO admin_auth(id, generation, updated_at) VALUES(1, 0, ?)", (stamp(),))

    # ---- reading ------------------------------------------------------------------------------------------------
    def _read(self):
        with self.store.lock:
            row = self.store.connection.execute("SELECT * FROM admin_auth WHERE id=1").fetchone()
        if row is None:
            raise RuntimeError("the administrator record is missing")
        return row

    @_fail_closed
    def password_set(self) -> bool:
        return self._read()["hash"] is not None

    @_fail_closed
    def generation(self) -> int:
        """Counts every set, change and reset. A session from an older count is finished."""
        return self._read()["generation"]

    # ---- signing in ---------------------------------------------------------------------------------------------
    @_fail_closed
    def verify(self, password: str) -> int:
        """Return the generation the password was checked against. Raise LOGIN_FAILED or ACCOUNT_LOCKED."""
        with self._attempt:
            row = self._read()
            now = self.clock()
            failures = row["failures"]
            if row["locked_until"] is not None:
                until = self._parse(row["locked_until"])
                if until is None or until > now:    # a record that cannot be read counts as locked
                    raise AgentError("ACCOUNT_LOCKED", f"Too many failed attempts; try again in {LOCK_MINUTES} minutes", 429, True)
                failures = 0                          # the lock ran out: a new window starts
            if row["hash"] is None:
                _scrypt(prepare(password), self.DUMMY_SALT, SCRYPT_N, SCRYPT_R, SCRYPT_P)   # the same time as a real check
                raise self._refused()
            if self._matches(password, row):
                if failures or row["locked_until"] is not None:
                    self._record(row["generation"], 0, None)
                return row["generation"]
            failures += 1
            self._record(row["generation"], failures, stamp(now + timedelta(minutes=LOCK_MINUTES)) if failures >= MAX_FAILURES else None)
            raise self._refused()

    @staticmethod
    def _refused() -> AgentError:
        return AgentError("LOGIN_FAILED", "The password is wrong", 401)

    @staticmethod
    def _parse(value: str):
        if not isinstance(value, str) or not TIMESTAMP.match(value):
            return None
        try:
            return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(timezone.utc)
        except ValueError:
            return None

    @staticmethod
    def _matches(password: str, row) -> bool:
        n, r, p = row["n"], row["r"], row["p"]
        if not (2 ** 4 <= n <= 2 ** 20 and n & (n - 1) == 0 and 1 <= r <= 32 and 1 <= p <= 16) or len(row["salt"]) != SALT_BYTES:
            raise RuntimeError("the stored hash parameters are not usable")
        return hmac.compare_digest(_scrypt(prepare(password), row["salt"], n, r, p), row["hash"])

    def _record(self, generation: int, failures: int, locked_until) -> None:
        with self.store.lock:
            self.store.connection.execute("UPDATE admin_auth SET failures=?, locked_until=? WHERE id=1 AND generation=?",
                                          (failures, locked_until, generation))

    # ---- setting, changing, resetting ---------------------------------------------------------------------------
    @_fail_closed
    def set_password(self, password: str, *, token_hashes=(), only_if_unset=False) -> int:
        """Store a new password and start a new generation. `only_if_unset` is the first-time setup: it never replaces one."""
        prepared = check_password(password, token_hashes)
        with self._attempt:
            salt = secrets.token_bytes(SALT_BYTES)
            digest = _scrypt(prepared, salt, SCRYPT_N, SCRYPT_R, SCRYPT_P)
            with self.store.lock:
                changed = self.store.connection.execute(
                    "UPDATE admin_auth SET generation=generation+1, salt=?, hash=?, n=?, r=?, p=?, failures=0, locked_until=NULL, updated_at=? "
                    "WHERE id=1" + (" AND hash IS NULL" if only_if_unset else ""),
                    (salt, digest, SCRYPT_N, SCRYPT_R, SCRYPT_P, stamp())).rowcount
                if not changed:
                    raise AgentError("PASSWORD_ALREADY_SET", "An administrator password is already set", 409)
                return self.store.connection.execute("SELECT generation FROM admin_auth WHERE id=1").fetchone()[0]

    @_fail_closed
    def change_password(self, current: str, password: str, *, token_hashes=()) -> int:
        """A change needs the current password. A wrong one counts as a failed sign-in."""
        prepared = check_password(password, token_hashes)
        if prepared == prepare(current):
            raise PolicyError("same", "The new password must differ from the current one")
        self.verify(current)
        return self.set_password(password, token_hashes=token_hashes)

    def reset(self) -> None:
        """Forget the password and the lock; end every password session. Used by `deskhubctl admin-password-reset`."""
        with self.store.lock:
            self.store.connection.execute(
                "UPDATE admin_auth SET generation=generation+1, salt=NULL, hash=NULL, n=NULL, r=NULL, p=NULL, failures=0, locked_until=NULL, updated_at=? WHERE id=1",
                (stamp(),))
