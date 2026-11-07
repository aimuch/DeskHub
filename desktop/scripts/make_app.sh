#!/bin/bash
# Builds DeskHub.app without Xcode: swift build (release), the bundle by hand, an ad-hoc signature.
#   desktop/scripts/make_app.sh            -> desktop/build/DeskHub.app
# Copy it to /Applications (or drag it there) to keep it. Apple silicon only.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"
swift run core-check
swift build -c release --product DeskHub
APP="$HERE/build/DeskHub.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$(swift build -c release --show-bin-path)/DeskHub" "$APP/Contents/MacOS/DeskHub"
cp "$HERE/Resources/Info.plist" "$APP/Contents/Info.plist"
cp "$HERE/Resources/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
# esptool for the firmware page: a program of its own (GPL-2.0), with its license next to it.
ESPTOOL="$("$HERE/scripts/fetch_esptool.sh")"
mkdir -p "$APP/Contents/Resources/esptool"
cp "$ESPTOOL/esptool" "$ESPTOOL/LICENSE" "$APP/Contents/Resources/esptool/"
codesign --force --sign - "$APP"
codesign --verify "$APP"
echo "$APP"
