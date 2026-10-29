"""The part the device-side mutation scripts share: copy the firmware tests to a temporary directory, break one line at a
time, build one host test target and run it. A mutation is caught when the build works and the test fails."""
import os, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FW = ROOT / "firmware"


def forget(build, target):
    """Make the build redo the target. Make compares file times to the second, so a source that was restored and broken
    again within one second would be taken as unchanged, and the binary of the mutation before it would run."""
    for path in (build / "CMakeFiles" / f"{target}.dir").rglob("*.o"):
        path.unlink()
    (build / target).unlink(missing_ok=True)


def run(root, target, source="tests", generator=None):
    build = root / "build"
    command = ["cmake", "-S", str(root / source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Debug"] + (["-G", generator] if generator else [])
    cfg = subprocess.run(command, capture_output=True)
    if cfg.returncode:
        return 100
    forget(build, target)
    if subprocess.run(["cmake", "--build", str(build), "--target", target], capture_output=True).returncode:
        return 101
    try:
        # Only the exit code matters. A broken build can print a failure for every step of a long random walk, and
        # capturing that output fills the memory of this script until the system kills it.
        return subprocess.run([str(build / target)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120).returncode
    except subprocess.TimeoutExpired:
        return 124


def check(root, mutations, target, source, generator):
    base = run(root, target, source, generator)
    print("baseline exit", base, flush=True)
    if base:
        return 1
    bad = ran = 0
    only = sys.argv[1:]
    for name, rel, old, new in mutations:
        if only and not any(o in name for o in only):
            continue
        ran += 1
        path = root / rel
        text = path.read_text(encoding="utf-8")
        if old not in text:
            print(f"SKIP     {name}: pattern not found", flush=True)
            bad += 1
            continue
        path.write_text(text.replace(old, new, 1), encoding="utf-8")
        code = run(root, target, source, generator)
        path.write_text(text, encoding="utf-8")
        if code == 0:
            print(f"SURVIVED {name}", flush=True)
            bad += 1
        elif code in (100, 101):
            print(f"INVALID  {name}: does not configure or compile", flush=True)
            bad += 1
        else:
            print(f"caught   {name}", flush=True)
    print("mutations:", ran, "not caught:", bad, flush=True)
    return 1 if bad else 0


def firmware_main(mutations, target, source="tests", generator=None):
    tmp = Path(tempfile.mkdtemp(prefix="fw_mut_"))
    root = tmp / "firmware"
    for d in ("components", "tests", "fonts"):
        shutil.copytree(FW / d, root / d, ignore=shutil.ignore_patterns("build*", "out", "managed_components"))
    shutil.copytree(ROOT / "protocol", tmp / "protocol", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copytree(ROOT / "scripts/ui_preview", tmp / "scripts/ui_preview", ignore=shutil.ignore_patterns("build", "__pycache__"))  # the preview build wants its touch test
    link = root / "managed_components"
    os.symlink(FW / "managed_components", link)
    try:
        return check(root, mutations, target, source, generator)
    finally:
        link.unlink()  # remove the link first: the real directory must never be touched
        shutil.rmtree(tmp, ignore_errors=True)
