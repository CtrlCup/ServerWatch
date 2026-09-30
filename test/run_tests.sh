#!/usr/bin/env bash
# Fuehrt die komplette ServerWatch-Testsuite aus.
#   test/run_tests.sh                 # Simulation + Dashboard + Firmware-Build (falls pio da ist)
#   test/run_tests.sh -m "not firmware"  # ohne Firmware-Build
#   test/run_tests.sh -k swarm        # nur Schwarm-Tests
# Weitere Argumente gehen direkt an pytest.
set -euo pipefail
cd "$(dirname "$0")"

python3 -c "import pytest, requests, websockets" 2>/dev/null || {
    echo "Python-Abhaengigkeiten fehlen: pip install -r test/requirements.txt" >&2
    exit 1
}
if command -v npm >/dev/null && [ ! -d js/node_modules/jsdom ]; then
    (cd js && npm ci --no-fund --no-audit >/dev/null)
fi
python3 sim/build.py
SIM_SKIP_BUILD=1 exec python3 -m pytest "$@"
