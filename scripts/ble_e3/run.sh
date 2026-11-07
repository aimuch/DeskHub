#!/bin/bash
# Builds the E3 test app (docs/desktop-app-design.md 13.5) and runs it through LaunchServices, so that macOS asks for
# Bluetooth for this app and does not kill it. No Xcode needed: swiftc and an ad-hoc signature.
#   scripts/ble_e3/run.sh <output.jsonl> [seconds] [reconnect-at-seconds]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:?output file}"
SECONDS_TOTAL="${2:-600}"
RECONNECT="${3:-300}"
BUILD="${TMPDIR:-/tmp}/deskhub-ble-e3"
APP="$BUILD/DeskHubE3.app"
mkdir -p "$APP/Contents/MacOS"
cp "$HERE/Info.plist" "$APP/Contents/Info.plist"
swiftc -O -o "$APP/Contents/MacOS/DeskHubE3" "$HERE/main.swift" -framework CoreBluetooth
codesign --force --sign - "$APP"
open -W -n "$APP" --args "out=$OUT" "seconds=$SECONDS_TOTAL" "reconnect=$RECONNECT"
cat "$OUT"
