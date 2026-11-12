#!/usr/bin/env python3
"""Stack budgets of the tasks that ran out on the device (docs/desktop-app-design.md 13.14 M14).

0.3.32 restarted about 20 s after it booted and rolled back (2026-11-12): the parsed Bluetooth message had grown to 552 bytes,
and write_control kept two of them and a 513-byte buffer on the NimBLE host task's 4 KB stack. This compiles the files again
with -fstack-usage, using the commands of a firmware build, and fails when a frame on those paths is over its budget.

    python3 scripts/check_stack.py firmware/build
"""
import json
import os
import shlex
import subprocess
import sys
import tempfile

# (file, function name part, budget in bytes). The NimBLE host task: 4096 in all, NimBLE's own frames included.
BUDGETS = [
    ("ble_link.cpp", "write_control", 512),
    ("ble_protocol.cpp", "parse_into", 512),
    ("ble_protocol.cpp", "parse_todo(", 512),
    ("ble_protocol.cpp", "parse_usage(", 512),
    ("ble_protocol.cpp", "parse_todo_result(", 512),
    ("ble_link.cpp", "read_info", 2048),
    # the LVGL task: 12 KB in PSRAM; update() runs every tick and calls the pages. 6496 in 0.3.32 after the to-do overlays moved
    # out of it (6816 before); what earlier versions used was not measured. The budget keeps it from growing unseen.
    # 6560 after the undo of "done" (13.15). 3904 once the per-tick Frame moved to static storage (code review, 2026-11-12):
    # the budget follows it down, so the 2.6 KB it gave back stay free for the page code under update().
    ("ui.cpp", "{anonymous}::update()", 4352),
    ("ui.cpp", "todo_overlays", 4096),
]
WANT = {"ble_link.cpp", "ble_protocol.cpp", "ui.cpp"}


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    build = os.path.abspath(sys.argv[1])
    with open(os.path.join(build, "compile_commands.json")) as f:
        commands = json.load(f)
    out = tempfile.mkdtemp(prefix="stack-")
    found = {}
    for e in commands:
        name = os.path.basename(e["file"])
        if name not in WANT or not ("/components/ble_link/" in e["file"] or "/components/ui/" in e["file"]):
            continue
        args = shlex.split(e["command"])
        args[args.index("-o") + 1] = os.path.join(out, name + ".o")
        if "-MF" in args:
            args[args.index("-MF") + 1] = os.devnull
        args.insert(1, "-fstack-usage")
        r = subprocess.run(args, cwd=e["directory"], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"{name}: does not compile\n{r.stderr[-2000:]}")
            return 1
        with open(os.path.join(out, name + ".su")) as su:
            for line in su:
                where, size, _ = line.rstrip("\n").split("\t")
                found.setdefault(name, []).append((where, int(size)))
    bad = 0
    for name, part, budget in BUDGETS:
        sizes = [(w, n) for w, n in found.get(name, []) if part in w]
        if not sizes:
            print(f"MISSING {name} {part}")
            bad += 1
            continue
        worst = max(n for _, n in sizes)
        ok = worst <= budget
        bad += 0 if ok else 1
        print(f"{'ok  ' if ok else 'OVER'} {name:18} {part:28} {worst:5} of {budget}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
