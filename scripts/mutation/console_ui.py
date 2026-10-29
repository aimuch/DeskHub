#!/usr/bin/env python3
"""Mutation check for the web console: break the front end on purpose, the browser test must go red.

Usage: scripts/mutation/console_ui.py [name-part ...]
Works on a copy in a temporary directory: the copy of agent/ is served by the Agent that the test starts
(PYTHONPATH), and the report and screenshots go to a temporary folder. The working tree is never touched.
Each run starts a real Agent and a real Chrome, so it takes about a minute and a half. Exit 0 only if the baseline
is green and every mutation is caught.
"""
import os, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
W = "agent/src/deskhub_agent/web/"
MUTATIONS = [
    ("first-time session sees the console", [(W + "app.js", "if (session.setup_required) needsPassword(session);\n    else await enter(session);", "await enter(session);")]),
    ("password write without CSRF", [(W + "lib/api.js", "(path === '/session' || path === '/login'))) init.headers.set", "(path === '/session' || path === '/login' || path === '/password'))) init.headers.set")]),
    ("a pending configuration looks applied", [(W + "lib/copy.js", "pending: ['等待设备确认', 'warn']", "pending: ['设备已应用', 'ok']")]),
    ("a stale session is not noticed", [(W + "lib/api.js", "&& error.code === 'UNAUTHORIZED') onUnauthorized()", "&& false) onUnauthorized()")]),
    ("the password tab is never the default", [(W + "app.js", "pickTab(passwordSet ? 'password' : 'token');", "pickTab('token');")]),
    ("the reason for signing out is dropped", [(W + "app.js", "  showError(passwordSet ? 'password-error' : 'token-error', message);\n", "")]),
    ("a changed password keeps the old key", [(W + "pages/access.js", "      state.csrf = session.csrf_token;\n      current.value", "      current.value")]),
    ("any appearance value is accepted", [(W + "lib/prefs.js", "const allowed = (name, value) => CHOICES[name].some(([id]) => id === value);", "const allowed = () => true;")]),
    ("an unknown appearance item is accepted", [(W + "lib/prefs.js", "if (Object.keys(data).some(key => !known.has(key))) return null;", "")]),
    ("stale power numbers stay on the screen", [(W + "pages/common.js", "if (device.power_state === 'stale') return { value: '数据已过期', sub: '设备已经超过 30 秒没有上报', fresh: false };", "")]),
    ("the power page shows old readings", [(W + "pages/power.js", "reading('电池电压', fresh ? volts(power.battery_mv) : NOT_REPORTED,", "reading('电池电压', volts(power.battery_mv),")]),
    ("a revoke needs no confirmation", [(W + "pages/access.js", "  if (!confirmed) return;\n  try {\n    await ctx.api.request(`/devices/", "  try {\n    await ctx.api.request(`/devices/")]),
    ("the setup button is always on", [(W + "app.js", "$('setup-submit').disabled = !list.slice(0, 2).every(([, pass]) => pass);", "$('setup-submit').disabled = false;")]),
    ("a command filter shows everything", [(W + "pages/commands.js", "['failed', '失败', item => item.state === 'failed']", "['failed', '失败', () => true]")]),
]


def run(root, out):
    env = {**os.environ, "PYTHONPATH": str(root / "agent/src"), "E2E_OUT": str(out), "PYTHONDONTWRITEBYTECODE": "1"}
    return subprocess.run(["node", str(ROOT / "scripts/e2e/console.e2e.mjs")], env=env, cwd=ROOT, capture_output=True, text=True, timeout=600).returncode


def main():
    tmp = Path(tempfile.mkdtemp(prefix="console_mut_"))
    out = tmp / "out"
    shutil.copytree(ROOT / "agent", tmp / "agent", ignore=shutil.ignore_patterns("__pycache__", "*.sqlite3*", "tests"))
    base = run(tmp, out)
    print("baseline exit", base, flush=True)
    if base:
        return 1
    bad = ran = 0
    only = sys.argv[1:]
    for name, edits in MUTATIONS:
        if only and not any(part in name for part in only):
            continue
        ran += 1
        saved, ok = {}, True
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
            code = run(tmp, out)
            print(("SURVIVED " if code == 0 else "caught   ") + name, flush=True)
            bad += code == 0
        for rel, text in saved.items():
            (tmp / rel).write_text(text, encoding="utf-8")
    shutil.rmtree(tmp, ignore_errors=True)
    print("mutations:", ran, "not caught:", bad, flush=True)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
