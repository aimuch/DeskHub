#!/bin/bash
# Espressif's esptool, the standalone macOS arm64 build, for the App's firmware page (docs/desktop-app-design.md 13.10).
# Downloaded once into desktop/build/cache and checked against the SHA-256 that GitHub publishes for the release asset.
#   desktop/scripts/fetch_esptool.sh   -> prints the folder that holds esptool and LICENSE
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="5.6.0"
SHA256="08a58829a3f14bb08f2e39f4cad48dcb14b7000384268e7f06689836332d6e25"
URL="https://github.com/espressif/esptool/releases/download/v$VERSION/esptool-v$VERSION-macos-arm64.tar.gz"
CACHE="$HERE/build/cache/esptool-$VERSION"
if [ ! -x "$CACHE/esptool-macos-arm64/esptool" ]; then
  mkdir -p "$CACHE"
  curl -sSfL -o "$CACHE/esptool.tar.gz" "$URL"
  echo "$SHA256  $CACHE/esptool.tar.gz" | shasum -a 256 -c - >&2
  tar -xzf "$CACHE/esptool.tar.gz" -C "$CACHE" esptool-macos-arm64/esptool esptool-macos-arm64/LICENSE
  rm -f "$CACHE/esptool.tar.gz"
fi
GOT="$("$CACHE/esptool-macos-arm64/esptool" version | tail -1)"
[ "$GOT" = "$VERSION" ] || { echo "esptool says $GOT, not $VERSION" >&2; exit 1; }
echo "$CACHE/esptool-macos-arm64"
