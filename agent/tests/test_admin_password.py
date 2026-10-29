"""Failure modes of the administrator password login (docs/rules.md R8, decision D10), written before the code.

A  Policy: at least 12 characters, at most 128; common passwords, one repeated character, very few distinct characters, a long
   run of a sequence, control characters, only white space, the token (the "口令") itself: all refused.
H  Hashing: slow hash (scrypt), an own salt for every password, nothing of the password in the database, the cost parameters
   stored with the hash.
S  Setup: the first browser login with the token is a setup session: nothing but "set the password" works until it is set.
   A script (no Origin header, from this computer) with the token is not a page and keeps its full access.
T  After the password is set, the browser no longer takes the token. Scripts still do.
L  Password login: needs the same Origin; a wrong password and "no password set" look the same; five failures lock the
   account for 15 minutes; the lock survives a restart; a failing lock store or a broken lock record never lets anyone in;
   concurrent failures cannot get past the counter.
C  Change: needs the current password (and a wrong one counts as a failure); the new one must pass the policy and differ;
   every other session ends at once, this one goes on; CSRF and Origin are required.
R  Reset: only on the computer, with the command line. It ends every session. The web has no reset, and a token cannot set
   or change the password.
N  No secret anywhere: not in a response, not in an error, not in the log, not in the database file.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from datetime import timedelta
import hashlib
import json
import logging
import time

import pytest
from fastapi.testclient import TestClient

from deskhub_agent import admin_auth
from deskhub_agent.admin_auth import AdminAuth, PolicyError, check_password
from deskhub_agent.app import create_app
from deskhub_agent.cli import main
from deskhub_agent.errors import AgentError
from deskhub_agent.models import utcnow
from deskhub_agent.storage import Store

ORIGIN = "http://testserver"
GOOD = "q7Z!mK2#pL9v"
OTHER = "n4W$xT8&cB5r"
WEAK = "password-123456"


@pytest.fixture
def stack(settings, monkeypatch):
    monkeypatch.setattr(admin_auth, "SCRYPT_N", 2 ** 4)   # cheap in tests; the production cost is checked on its own (H1)
    store = Store(settings.db_path)
    token = store.pair("browser-admin", ["admin:config"], ["*"])
    app = create_app(settings, store=store)
    with TestClient(app) as client:
        yield client, app, store, token
    store.close()


def error_code(response):
    return response.json()["error"]["code"]


def token_login(client, token, *, origin=True):
    return client.post("/api/v1/admin/session", json={"token": token}, headers={"Origin": ORIGIN} if origin else {})


def password_login(client, password, *, origin=True):
    return client.post("/api/v1/admin/login", json={"password": password}, headers={"Origin": ORIGIN} if origin else {})


def write_headers(csrf):
    return {"Origin": ORIGIN, "X-CSRF-Token": csrf}


def setup(client, token, password=GOOD):
    """The first browser login with the token, then the password. Returns the csrf token of the session."""
    login = token_login(client, token)
    assert login.status_code == 200 and login.json()["setup_required"] is True, login.text
    csrf = login.json()["csrf_token"]
    done = client.put("/api/v1/admin/password", json={"password": password}, headers=write_headers(csrf))
    assert done.status_code == 200, done.text
    assert done.json()["csrf_token"] != csrf and done.json()["setup_required"] is False   # a new key for the new rights
    return done.json()["csrf_token"]


def second_client(app):
    return TestClient(app)   # its own cookie jar: a second browser


PAGES = ["/api/v1/admin/overview", "/api/v1/admin/devices", "/api/v1/admin/firmware", "/api/v1/admin/host",
         "/api/v1/admin/displays", "/api/v1/admin/commands"]


# ---- A: policy -------------------------------------------------------------------------------------------------------
def test_a1_length_limits():
    check_password(GOOD)                                  # 12 characters
    with pytest.raises(PolicyError):
        check_password(GOOD[:-1])                         # 11
    check_password("a" * 60 + "Q7z!" * 17)                # 128 characters
    with pytest.raises(PolicyError):
        check_password("a" * 60 + "Q7z!" * 17 + "x")      # 129
    for value in ("", " ", None, 123456789012, b"q7Z!mK2#pL9v", ["q7Z!mK2#pL9v"]):
        with pytest.raises(PolicyError):
            check_password(value)


def test_a2_common_and_patterned_passwords_are_refused():
    weak = ["password1234", "Password1234", "PASSWORD1234!", "123456789012", "qwertyuiop12", "deskhub12345", "DeskHub-Admin1",
            "administrator", "iloveyou1234", "letmein123456", "aaaaaaaaaaaa", "abababababab", "abcdefghijkl", "1234567890ab",
            "zyxwvutsrqpo", "111111111111", "000000000000",
            "Sunshine1234", "baseball2024!",                  # a common word with digits behind it
            "abcabcabcabc",                                    # only three different characters
            "abcdefgh7Q!z"]                                    # a run of eight is the limit
    for value in weak:
        with pytest.raises(PolicyError):
            check_password(value)
    check_password("密码很长很长很长很长很长很长")          # a long passphrase in any script is fine
    check_password("correct horse battery staple")        # spaces are fine
    check_password("abcdefgZ7!kq")                        # a run of seven is still fine


def test_a3_control_characters_and_white_space_only_are_refused():
    for value in ("q7Z!mK2#pL9v\x00", "q7Z!mK2\n#pL9v", "q7Z!mK2#pL9v\x7f", " " * 20, "\t" * 20,
                  "\u1680\u2028\u2029 " * 3, "q7Z!mK2#pL9v\ud800"):   # four kinds of white space; a lone surrogate
        with pytest.raises(PolicyError):
            check_password(value)


def test_a4_the_token_is_not_a_password(stack):
    client, app, store, token = stack
    hashes = [hashlib.sha256(token.encode()).hexdigest()]
    with pytest.raises(PolicyError):
        check_password(token, token_hashes=hashes)
    check_password(GOOD, token_hashes=hashes)
    login = token_login(client, token)
    csrf = login.json()["csrf_token"]
    refused = client.put("/api/v1/admin/password", json={"password": token}, headers=write_headers(csrf))
    assert refused.status_code == 400 and error_code(refused) == "WEAK_PASSWORD"
    assert token not in refused.text


# ---- H: hashing ------------------------------------------------------------------------------------------------------
def test_h1_the_production_cost_is_a_slow_hash():
    assert admin_auth.SCRYPT_N >= 2 ** 14 and admin_auth.SCRYPT_R == 8 and admin_auth.SCRYPT_P == 1
    assert admin_auth.MAX_FAILURES == 5 and admin_auth.LOCK_MINUTES == 15 and admin_auth.MIN_LENGTH == 12


def test_h2_salt_hash_and_parameters(stack, settings):
    client, app, store, token = stack
    auth: AdminAuth = app.state.management.auth
    auth.set_password(GOOD, token_hashes=[])
    first = store.connection.execute("SELECT * FROM admin_auth WHERE id=1").fetchone()
    auth.set_password(OTHER, token_hashes=[])
    second = store.connection.execute("SELECT * FROM admin_auth WHERE id=1").fetchone()
    assert len(first["salt"]) == 16 and len(first["hash"]) == 32 and first["salt"] != second["salt"]
    assert (first["n"], first["r"], first["p"]) == (2 ** 4, 8, 1)
    assert second["generation"] > first["generation"]
    for row in (first, second):
        dump = repr(tuple(row)).encode()
        assert GOOD.encode() not in dump and OTHER.encode() not in dump
    store.connection.commit()
    raw = open(settings.db_path, "rb").read()
    assert GOOD.encode() not in raw and OTHER.encode() not in raw
    auth.verify(OTHER)
    for wrong in (GOOD, OTHER + "x", OTHER[:-1], ""):
        with pytest.raises(AgentError) as caught:
            auth.verify(wrong)
        assert caught.value.code == "LOGIN_FAILED"


def test_h3_one_password_in_every_unicode_spelling(stack):
    client, app, store, token = stack
    auth = app.state.management.auth
    wide = "".join(chr(ord(char) + 0xFEE0) if "!" <= char <= "~" else char for char in GOOD)   # the full-width form of the same text
    assert wide != GOOD
    auth.set_password(wide, token_hashes=[])
    auth.verify(GOOD)
    auth.set_password(GOOD, token_hashes=[])
    auth.verify(wide)


def test_h4_the_stored_cost_is_used_not_the_current_one(stack, monkeypatch):
    client, app, store, token = stack
    auth = app.state.management.auth
    auth.set_password(GOOD, token_hashes=[])                    # N = 2**4
    monkeypatch.setattr(admin_auth, "SCRYPT_N", 2 ** 5)         # a later release raises the cost
    auth.verify(GOOD)                                           # the old hash still opens with its own cost


# ---- S: setup --------------------------------------------------------------------------------------------------------
def test_s1_the_first_browser_login_can_only_set_the_password(stack):
    client, app, store, token = stack
    login = token_login(client, token)
    assert login.status_code == 200 and login.json()["setup_required"] is True
    csrf = login.json()["csrf_token"]
    assert client.get("/api/v1/admin/session").json()["setup_required"] is True
    for path in PAGES:
        blocked = client.get(path)
        assert blocked.status_code == 403 and error_code(blocked) == "PASSWORD_REQUIRED", path
    for method, path in (("post", "/api/v1/admin/devices/browser-admin/revoke"), ("put", "/api/v1/admin/devices/browser-admin/configuration")):
        blocked = getattr(client, method)(path, headers=write_headers(csrf))
        assert blocked.status_code in {400, 403}
        assert blocked.status_code == 403 and error_code(blocked) == "PASSWORD_REQUIRED", path
    upload = client.post("/api/v1/admin/firmware", content=b"PK", headers={**write_headers(csrf), "Content-Type": "application/zip"})
    assert upload.status_code == 403 and error_code(upload) == "PASSWORD_REQUIRED"
    assert client.put("/api/v1/admin/password", json={"password": GOOD}, headers=write_headers(csrf)).status_code == 200
    for path in PAGES:
        assert client.get(path).status_code == 200, path
    assert client.get("/api/v1/admin/session").json()["setup_required"] is False
    assert len(app.state.management.sessions) == 1               # the first-time session was replaced, not kept beside it


def test_s2_setting_the_password_needs_csrf_and_origin(stack):
    client, app, store, token = stack
    csrf = token_login(client, token).json()["csrf_token"]
    body = {"password": GOOD}
    assert client.put("/api/v1/admin/password", json=body, headers={"Origin": ORIGIN}).status_code == 403
    assert client.put("/api/v1/admin/password", json=body, headers={"X-CSRF-Token": csrf}).status_code == 403
    assert client.put("/api/v1/admin/password", json=body, headers={"Origin": "https://attacker.invalid", "X-CSRF-Token": csrf}).status_code == 403
    assert client.put("/api/v1/admin/password", json=body, headers={"Origin": ORIGIN, "X-CSRF-Token": "wrong"}).status_code == 403
    assert client.get("/api/v1/admin/auth").json() == {"password_set": False}
    assert client.put("/api/v1/admin/password", json=body, headers=write_headers(csrf)).status_code == 200
    assert client.get("/api/v1/admin/auth").json() == {"password_set": True}


def test_s3_weak_passwords_do_not_end_the_setup(stack):
    client, app, store, token = stack
    csrf = token_login(client, token).json()["csrf_token"]
    for weak in ("short", "password1234", "aaaaaaaaaaaa"):
        refused = client.put("/api/v1/admin/password", json={"password": weak}, headers=write_headers(csrf))
        assert refused.status_code == 400 and error_code(refused) == "WEAK_PASSWORD" and weak not in refused.text
    assert client.get("/api/v1/admin/auth").json() == {"password_set": False}
    assert client.get(PAGES[0]).status_code == 403
    assert client.put("/api/v1/admin/password", json={"password": GOOD}, headers=write_headers(csrf)).status_code == 200


def test_s4_a_script_with_the_token_is_not_a_page(stack):
    client, app, store, token = stack
    login = token_login(client, token, origin=False)       # no Origin header: a command line or a script on this computer
    assert login.status_code == 200 and login.json()["setup_required"] is False
    for path in PAGES:
        assert client.get(path).status_code == 200, path
    assert client.put("/api/v1/admin/password", json={"password": GOOD}, headers=write_headers(login.json()["csrf_token"])).status_code == 403


def test_s5_the_setup_session_ends_when_the_token_is_revoked(stack):
    client, app, store, token = stack
    token_login(client, token)
    store.revoke("browser-admin")
    assert client.get("/api/v1/admin/session").status_code == 401


def test_s6_setting_it_twice_from_the_setup_session_is_refused(stack):
    client, app, store, token = stack
    csrf = setup(client, token)
    again = client.put("/api/v1/admin/password", json={"password": OTHER}, headers=write_headers(csrf))
    assert again.status_code == 400 and error_code(again) == "CURRENT_PASSWORD_REQUIRED"   # now it is a change, and a change needs the current one


def test_s7_the_first_time_setup_never_replaces_a_password(stack):
    client, app, store, token = stack
    auth = app.state.management.auth
    first = auth.set_password(GOOD, token_hashes=[], only_if_unset=True)
    with pytest.raises(AgentError) as caught:
        auth.set_password(OTHER, token_hashes=[], only_if_unset=True)
    assert caught.value.code == "PASSWORD_ALREADY_SET" and caught.value.status == 409
    assert auth.generation() == first
    auth.verify(GOOD)


# ---- T: the token after the password ------------------------------------------------------------------------------------
def test_t1_browsers_no_longer_take_the_token_scripts_do(stack):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    refused = token_login(other, token)
    assert refused.status_code == 403 and error_code(refused) == "TOKEN_LOGIN_DISABLED"
    assert not other.cookies.get("deskhub_session")
    script = token_login(other, token, origin=False)
    assert script.status_code == 200 and script.json()["setup_required"] is False
    assert other.get(PAGES[0]).status_code == 200


def test_t2_the_token_can_still_be_revoked_by_itself(stack):
    client, app, store, token = stack
    setup(client, token)
    store.revoke("browser-admin")
    assert client.get(PAGES[0]).status_code == 200          # the password session is the account, not the token
    other = second_client(app)
    assert token_login(other, token, origin=False).status_code == 401
    assert password_login(other, GOOD).status_code == 200


# ---- L: password login -------------------------------------------------------------------------------------------------
def test_l1_login_with_the_password(stack):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    ok = password_login(other, GOOD)
    assert ok.status_code == 200 and ok.json()["setup_required"] is False and ok.json()["csrf_token"]
    cookie = ok.headers["set-cookie"]
    assert "HttpOnly" in cookie and "SameSite=strict" in cookie and "Max-Age=1800" in cookie
    assert other.get(PAGES[0]).status_code == 200
    assert other.delete("/api/v1/admin/session", headers=write_headers(ok.json()["csrf_token"])).status_code == 200
    assert other.get(PAGES[0]).status_code == 401


def test_l2_a_wrong_password_and_no_password_look_the_same(stack):
    client, app, store, token = stack
    before = password_login(client, GOOD)                    # nothing is set yet
    assert before.status_code == 401 and error_code(before) == "LOGIN_FAILED"
    setup(client, token)
    other = second_client(app)
    wrong = password_login(other, OTHER)
    assert wrong.status_code == 401 and error_code(wrong) == "LOGIN_FAILED"
    assert before.json()["error"]["message"] == wrong.json()["error"]["message"]
    assert not other.cookies.get("deskhub_session")


def test_l3_the_password_login_needs_the_same_origin(stack):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    assert password_login(other, GOOD, origin=False).status_code == 403
    assert other.post("/api/v1/admin/login", json={"password": GOOD}, headers={"Origin": "https://attacker.invalid"}).status_code == 403
    assert not other.cookies.get("deskhub_session")


def test_l4_five_failures_lock_the_account_for_fifteen_minutes(stack):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    for attempt in range(5):
        assert password_login(other, OTHER).status_code == 401, attempt
    locked = password_login(other, GOOD)                    # the right password does not help while it is locked
    assert locked.status_code == 429 and error_code(locked) == "ACCOUNT_LOCKED"
    assert not other.cookies.get("deskhub_session")
    auth = app.state.management.auth
    auth.clock = lambda: utcnow() + timedelta(minutes=14, seconds=50)
    assert password_login(other, GOOD).status_code == 429
    auth.clock = lambda: utcnow() + timedelta(minutes=15, seconds=1)
    assert password_login(other, OTHER).status_code == 401      # the lock is over and the count starts again: one, not six
    assert password_login(other, GOOD).status_code == 200


def test_l5_a_success_clears_the_count(stack):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    for attempt in range(4):
        assert password_login(other, OTHER).status_code == 401
    assert password_login(other, GOOD).status_code == 200       # the count of four is gone ...
    for attempt in range(2):
        assert password_login(other, OTHER).status_code == 401   # ... so these two do not make the fifth
    assert password_login(other, GOOD).status_code == 200


def test_l6_the_lock_survives_a_restart(stack, settings):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    for _ in range(5):
        password_login(other, OTHER)
    fresh = TestClient(create_app(settings, store=store))
    locked = password_login(fresh, GOOD)
    assert locked.status_code == 429 and error_code(locked) == "ACCOUNT_LOCKED"


def test_l7_a_per_address_limit_comes_before_the_hashing(stack, monkeypatch):
    client, app, store, token = stack
    other = second_client(app)
    codes = [password_login(other, OTHER).status_code for _ in range(10)]
    assert codes == [401] * 10
    calls = []
    monkeypatch.setattr(app.state.management.auth, "verify", lambda password: calls.append(password))
    limited = password_login(other, OTHER)
    assert limited.status_code == 429 and error_code(limited) == "RATE_LIMITED" and calls == []


def test_l11_a_missing_password_costs_the_same_time(stack, monkeypatch):
    client, app, store, token = stack
    calls = []
    real = admin_auth._scrypt
    monkeypatch.setattr(admin_auth, "_scrypt", lambda *args: (calls.append(args), real(*args))[1])
    with pytest.raises(AgentError) as caught:
        app.state.management.auth.verify(GOOD)
    assert caught.value.code == "LOGIN_FAILED" and len(calls) == 1   # the hash is computed although there is nothing to compare


def test_l8_a_broken_lock_store_lets_nobody_in(stack, monkeypatch):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    auth = app.state.management.auth
    original = auth._read

    def broken():
        raise RuntimeError("the lock record cannot be read")

    monkeypatch.setattr(auth, "_read", broken)
    response = password_login(other, GOOD)
    assert response.status_code in {429, 500, 503} and not other.cookies.get("deskhub_session")
    monkeypatch.setattr(auth, "_read", original)
    assert password_login(other, GOOD).status_code == 200


def test_l9_a_broken_lock_record_counts_as_locked(stack):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    for bad in ("garbage", "", "9999", "2026-13-45T99:99:99Z", "2000-01-01", "2000-01-01T00:00:00+00:00"):   # the last two are real dates, not our format
        with store.lock:
            store.connection.execute("UPDATE admin_auth SET locked_until=? WHERE id=1", (bad,))
            store.connection.commit()
        response = password_login(other, GOOD)
        if bad == "":
            assert response.status_code in {200, 429}
            continue
        assert response.status_code == 429 and error_code(response) == "ACCOUNT_LOCKED", bad


def test_l10_concurrent_failures_cannot_get_past_the_counter(stack, monkeypatch):
    client, app, store, token = stack
    setup(client, token)
    auth: AdminAuth = app.state.management.auth
    real = admin_auth._scrypt
    monkeypatch.setattr(admin_auth, "_scrypt", lambda *args: (time.sleep(0.005), real(*args))[1])   # a wide window for a race

    def attempt(_):
        try:
            auth.verify(OTHER)
        except AgentError as error:
            return error.code
        return "OK"

    with ThreadPoolExecutor(max_workers=12) as pool:
        codes = list(pool.map(attempt, range(24)))
    assert codes.count("OK") == 0
    assert codes.count("LOGIN_FAILED") == 5 and codes.count("ACCOUNT_LOCKED") == 19


def test_l12_a_session_ends_after_thirty_minutes(stack):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    password_login(other, GOOD)
    for value in app.state.management.sessions.values():
        value["deadline"] -= 1790                                           # 29 minutes 50 seconds in
    assert other.get(PAGES[0]).status_code == 200
    for value in app.state.management.sessions.values():
        value["deadline"] -= 20
    assert other.get(PAGES[0]).status_code == 401


# ---- C: change -----------------------------------------------------------------------------------------------------------
def test_c1_change_needs_the_current_password_and_ends_the_other_sessions(stack):
    client, app, store, token = stack
    setup(client, token)
    a, b = second_client(app), second_client(app)
    csrf_a = password_login(a, GOOD).json()["csrf_token"]
    password_login(b, GOOD)
    assert b.get(PAGES[0]).status_code == 200
    change = {"password": OTHER, "current_password": GOOD}
    assert a.put("/api/v1/admin/password", json={"password": OTHER}, headers=write_headers(csrf_a)).status_code == 400
    wrong = a.put("/api/v1/admin/password", json={**change, "current_password": "n4W$xT8&cB5q"}, headers=write_headers(csrf_a))
    assert wrong.status_code == 401 and error_code(wrong) == "LOGIN_FAILED"
    assert a.put("/api/v1/admin/password", json=change, headers=write_headers(csrf_a)).status_code == 200
    assert b.get(PAGES[0]).status_code == 401                # every other session ended at once
    assert a.get(PAGES[0]).status_code == 200                # this one goes on
    assert client.get(PAGES[0]).status_code == 401           # the setup session of the beginning too
    fresh = second_client(app)
    assert password_login(fresh, GOOD).status_code == 401
    assert password_login(fresh, OTHER).status_code == 200


def test_c2_the_new_password_must_pass_the_policy_and_differ(stack):
    client, app, store, token = stack
    setup(client, token)
    a = second_client(app)
    csrf = password_login(a, GOOD).json()["csrf_token"]
    for weak in ("short", "password1234"):
        refused = a.put("/api/v1/admin/password", json={"password": weak, "current_password": GOOD}, headers=write_headers(csrf))
        assert refused.status_code == 400 and error_code(refused) == "WEAK_PASSWORD"
    same = a.put("/api/v1/admin/password", json={"password": GOOD, "current_password": GOOD}, headers=write_headers(csrf))
    assert same.status_code == 400 and error_code(same) == "WEAK_PASSWORD"
    assert a.get(PAGES[0]).status_code == 200


def test_c3_wrong_current_passwords_count_toward_the_lock(stack):
    client, app, store, token = stack
    setup(client, token)
    a = second_client(app)
    csrf = password_login(a, GOOD).json()["csrf_token"]
    body = {"password": OTHER, "current_password": "n4W$xT8&cB5q"}
    codes = [a.put("/api/v1/admin/password", json=body, headers=write_headers(csrf)).status_code for _ in range(6)]
    assert codes == [401, 401, 401, 401, 401, 429]
    assert password_login(second_client(app), GOOD).status_code == 429


def test_c5_the_token_is_not_a_new_password_either(stack):
    client, app, store, token = stack
    setup(client, token)
    a = second_client(app)
    csrf = password_login(a, GOOD).json()["csrf_token"]
    refused = a.put("/api/v1/admin/password", json={"password": token, "current_password": GOOD}, headers=write_headers(csrf))
    assert refused.status_code == 400 and error_code(refused) == "WEAK_PASSWORD" and token not in refused.text


def test_c6_a_weak_new_password_never_costs_an_attempt(stack):
    client, app, store, token = stack
    setup(client, token)
    a = second_client(app)
    csrf = password_login(a, GOOD).json()["csrf_token"]
    body = {"password": "short", "current_password": "n4W$xT8&cB5q"}      # both are wrong
    codes = [a.put("/api/v1/admin/password", json=body, headers=write_headers(csrf)).status_code for _ in range(6)]
    assert codes == [400] * 6
    assert password_login(second_client(app), GOOD).status_code == 200      # the account is not locked


def test_c4_change_needs_csrf_and_origin(stack):
    client, app, store, token = stack
    setup(client, token)
    a = second_client(app)
    csrf = password_login(a, GOOD).json()["csrf_token"]
    body = {"password": OTHER, "current_password": GOOD}
    assert a.put("/api/v1/admin/password", json=body, headers={"Origin": ORIGIN}).status_code == 403
    assert a.put("/api/v1/admin/password", json=body, headers={"X-CSRF-Token": csrf}).status_code == 403
    assert password_login(second_client(app), GOOD).status_code == 200


# ---- R: reset ------------------------------------------------------------------------------------------------------------
def test_r1_the_web_has_no_reset_and_a_token_cannot_set_the_password(stack):
    client, app, store, token = stack
    setup(client, token)
    for path in ("/api/v1/admin/password/reset", "/api/v1/admin/password-reset", "/api/v1/admin/reset"):
        assert client.post(path, headers=write_headers("x")).status_code in {404, 405}
    script = second_client(app)
    login = token_login(script, token, origin=False)
    for body in ({"password": OTHER}, {"password": OTHER, "current_password": GOOD}):
        assert script.put("/api/v1/admin/password", json=body, headers=write_headers(login.json()["csrf_token"])).status_code == 403
    assert password_login(second_client(app), GOOD).status_code == 200     # nothing changed


def test_r2_the_command_line_reset_ends_everything(stack, settings, tmp_path, capsys):
    client, app, store, token = stack
    setup(client, token)
    a = second_client(app)
    password_login(a, GOOD)
    config = tmp_path / "agent.json"
    config.write_text(settings.model_dump_json())
    config.chmod(0o600)
    assert main(["--config", str(config), "admin-password-reset"]) == 0
    out = capsys.readouterr()
    assert json.loads(out.out) == {"reset": True} and GOOD not in out.out + out.err
    assert a.get(PAGES[0]).status_code == 401 and client.get(PAGES[0]).status_code == 401
    fresh = second_client(app)
    assert password_login(fresh, GOOD).status_code == 401
    assert fresh.get("/api/v1/admin/auth").json() == {"password_set": False}
    again = token_login(fresh, token)                         # back to the first login: the token, then a new password
    assert again.status_code == 200 and again.json()["setup_required"] is True
    assert fresh.put("/api/v1/admin/password", json={"password": OTHER}, headers=write_headers(again.json()["csrf_token"])).status_code == 200


def test_r3_a_reset_also_clears_a_lock(stack):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    for _ in range(5):
        password_login(other, OTHER)
    app.state.management.auth.reset()
    row = store.connection.execute("SELECT failures, locked_until FROM admin_auth WHERE id=1").fetchone()
    assert (row["failures"], row["locked_until"]) == (0, None)
    csrf = token_login(other, token).json()["csrf_token"]
    assert other.put("/api/v1/admin/password", json={"password": GOOD}, headers=write_headers(csrf)).status_code == 200
    assert password_login(second_client(app), GOOD).status_code == 200


# ---- N: no secret anywhere -------------------------------------------------------------------------------------------
def test_n1_no_response_error_or_log_has_the_password(stack, caplog, settings):
    client, app, store, token = stack
    caplog.set_level(logging.DEBUG)
    secrets_seen = [GOOD, OTHER]
    responses = [token_login(client, token)]
    csrf = responses[0].json()["csrf_token"]
    responses.append(client.put("/api/v1/admin/password", json={"password": WEAK}, headers=write_headers(csrf)))   # too weak
    responses.append(client.put("/api/v1/admin/password", json={"password": 1234567890123}, headers=write_headers(csrf)))         # wrong type
    responses.append(client.put("/api/v1/admin/password", json={"password": GOOD, "extra": OTHER}, headers=write_headers(csrf)))   # unknown member
    responses.append(client.put("/api/v1/admin/password", content=b'{"password": "' + GOOD.encode() + b'"', headers={**write_headers(csrf), "Content-Type": "application/json"}))
    responses.append(client.put("/api/v1/admin/password", json={"password": GOOD}, headers=write_headers(csrf)))
    other = second_client(app)
    responses.append(password_login(other, OTHER))
    responses.append(other.post("/api/v1/admin/login", json={"password": [OTHER]}, headers={"Origin": ORIGIN}))
    responses.append(other.post("/api/v1/admin/login", json={"pass": OTHER}, headers={"Origin": ORIGIN}))
    responses.append(password_login(other, GOOD))
    for response in responses:
        for secret in secrets_seen + [WEAK]:
            assert secret not in response.text and secret not in json.dumps(dict(response.headers)), response.request.url
    logged = "\n".join(record.getMessage() for record in caplog.records)
    for secret in secrets_seen + [WEAK, "1234567890123"]:
        assert secret not in logged
    store.connection.commit()
    assert GOOD.encode() not in open(settings.db_path, "rb").read()


def test_n2_the_session_shape_is_the_same_as_the_token_session(stack):
    client, app, store, token = stack
    setup(client, token)
    other = second_client(app)
    shown = password_login(other, GOOD).json()
    assert set(shown) == {"admin_id", "csrf_token", "expires_at", "setup_required"}
    assert shown["admin_id"] == "admin"
