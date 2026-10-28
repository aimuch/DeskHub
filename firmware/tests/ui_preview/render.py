#!/usr/bin/env python3
"""Build the real device UI against native LVGL, run its checks, and save simulated screenshots.

    python3 firmware/tests/ui_preview/render.py

Output: firmware/tests/ui_preview/out/*.png and report.json. Everything is SIMULATED: the pass proves layout,
routing and state handling, not the panel, the touch controller, the power chip or the network.
"""
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
OUT = HERE / "out"


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=kw.pop("check", True), **kw)


def main() -> int:
    run(["cmake", "-S", HERE, "-B", BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", BUILD, "--parallel", "6"])
    run(["ctest", "--test-dir", BUILD, "--output-on-failure"])  # the UI checks and the touch-orientation regression
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    result = run([BUILD / "deskhub_ui_preview", OUT], check=False)
    converter = shutil.which("sips")
    if converter:
        for image in sorted(OUT.glob("*.ppm")):
            run([converter, "-s", "format", "png", image, "--out", image.with_suffix(".png")], stdout=subprocess.DEVNULL)
            image.unlink()
    print(f"screenshots and report.json: {OUT}")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
