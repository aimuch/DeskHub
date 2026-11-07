#!/bin/bash
# Builds DeskHub.app and puts it in a disk image with a link to /Applications: open the .dmg, drag DeskHub onto
# Applications, and it is in Launchpad too. No Xcode needed.
#   desktop/scripts/make_dmg.sh   -> desktop/build/DeskHub-<version>.dmg
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
"$HERE/scripts/make_app.sh" > /dev/null
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$HERE/Resources/Info.plist")"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/deskhub-dmg.XXXXXX")"
cp -R "$HERE/build/DeskHub.app" "$STAGE/DeskHub.app"
ln -s /Applications "$STAGE/Applications"
DMG="$HERE/build/DeskHub-$VERSION.dmg"
rm -f "$DMG"
hdiutil create -volname "DeskHub $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG" > /dev/null
hdiutil verify "$DMG" > /dev/null
echo "$DMG"
