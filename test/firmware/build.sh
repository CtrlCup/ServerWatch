#!/usr/bin/env bash
# Kompiliert beide Sketches mit PlatformIO fuer den ESP32-C3 (echte Toolchain).
# Nutzung: test/firmware/build.sh [board]   (Standard: esp32-c3-devkitm-1)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BOARD="${1:-esp32-c3-devkitm-1}"
PIO="${PIO:-$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")}"
OUT="$ROOT/test/.build/firmware"

build() {
    local sketch="$1" libs="$2" dir="$OUT/${1%.ino}"
    mkdir -p "$dir/src"
    cp "$ROOT/$sketch" "$dir/src/$sketch"
    # Header neben den Sketches mitnehmen (dashboard_html.h, lokale secrets.h falls vorhanden)
    find "$ROOT" -maxdepth 1 -name "*.h" -exec cp {} "$dir/src/" \;
    cat > "$dir/platformio.ini" <<INI
[env:fw]
platform = espressif32
board = $BOARD
framework = arduino
build_flags = -Wall -Wextra
lib_deps = $libs
INI
    echo "=== $sketch ($BOARD) ==="
    "$PIO" run -d "$dir" -e fw
}

build Serverwatch.ino ""
build ServerWatch_Multi.ino "bblanchon/ArduinoJson @ ^6.21.3, links2004/WebSockets @ ^2.4.1"
echo "Firmware-Build OK"
