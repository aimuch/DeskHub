#!/bin/bash
# Both install packages of the App: a .dmg (open it, drag DeskHub onto Applications) and a .zip (unzip: DeskHub.app, drag
# it into Applications or Launchpad). Used by the GitHub workflow and by hand.
#   desktop/scripts/make_release.sh   -> desktop/build/DeskHub-<version>.dmg and DeskHub-<version>.zip
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DMG="$("$HERE/scripts/make_dmg.sh")"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$HERE/Resources/Info.plist")"
ZIP="$HERE/build/DeskHub-$VERSION.zip"
rm -f "$ZIP"
# ditto keeps the bundle as macOS expects it (signature, permissions); --keepParent puts DeskHub.app at the top.
ditto -c -k --norsrc --keepParent "$HERE/build/DeskHub.app" "$ZIP"
echo "$DMG"
echo "$ZIP"
