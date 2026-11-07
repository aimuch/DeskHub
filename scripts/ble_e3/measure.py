"""Read the probe lines of a test build for a while, over one open serial port.

Usage: probe_measure.py PORT SECONDS OUT.jsonl
Halfway through it changes the theme a few times over USB (a full redraw: the heaviest DMA load the screen makes without a
finger), then sets it back to "ocean". Records DHPROBE lines, DHPROBE_EVENT lines and any ESP-IDF error or warning line.
Nothing else is written to the device.
"""
import json
import sys
import time

sys.path.insert(0, "tools")
import provision  # noqa: E402

port_name, seconds, out_path = sys.argv[1], float(sys.argv[2]), sys.argv[3]
port = provision.open_usb(port_name, 115200)
start = time.monotonic()
themes = [("mint", 0.45), ("amber", 0.55), ("mint", 0.65), ("ocean", 0.75)]
buffer = bytearray()
counts = {"probe": 0, "event": 0, "error": 0, "warning": 0}
with open(out_path, "w") as out:
    def record(kind, text):
        out.write(json.dumps({"t": round(time.monotonic() - start, 1), "kind": kind, "text": text}, ensure_ascii=False) + "\n")
        out.flush()

    while time.monotonic() - start < seconds:
        elapsed = (time.monotonic() - start) / seconds
        while themes and elapsed >= themes[0][1]:
            theme = themes.pop(0)[0]
            port.write(provision.encode_frame({"op": "display_preferences", "theme": theme}))
            port.flush()
            record("theme", theme)
        chunk = port.read(4096)
        if not chunk:
            continue
        buffer.extend(chunk)
        while b"\n" in buffer:
            line, _, rest = buffer.partition(b"\n")
            buffer = bytearray(rest)
            text = line.decode("utf-8", errors="replace").strip()
            if text.startswith("DHPROBE_EVENT"):
                counts["event"] += 1; record("event", text)
            elif text.startswith("DHPROBE "):
                counts["probe"] += 1; record("probe", text[8:])
            elif text.startswith("E (") or " E (" in text[:12]:
                counts["error"] += 1; record("error", text[:300])
            elif text.startswith("W ("):
                counts["warning"] += 1; record("warning", text[:300])
            elif text.startswith("{") and '"ok"' in text:
                record("reply", text[:200])
    port.close()
print(json.dumps(counts))
