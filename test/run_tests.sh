#!/usr/bin/env bash
# Fuehrt die komplette ServerWatch-Testsuite aus.
#   test/run_tests.sh                 # Simulation + Dashboard + Firmware-Build (falls pio da ist)
#   test/run_tests.sh -m "not firmware"  # ohne Firmware-Build
#   test/run_tests.sh -k swarm        # nur Schwarm-Tests
# Weitere Argumente gehen direkt an pytest.
set -euo pipefail
cd "$(dirname "$0")"

# Optionale venv unter test/.venv bevorzugen (Ubuntu/Debian sperren systemweites pip, PEP 668)
PY=python3
[ -x .venv/bin/python ] && PY=.venv/bin/python
"$PY" -c "import pytest, requests, websockets" 2>/dev/null || {
    echo "Python-Abhaengigkeiten fehlen: python3 -m venv test/.venv && test/.venv/bin/pip install -r test/requirements.txt" >&2
    exit 1
}
if command -v npm >/dev/null && [ ! -d js/node_modules/jsdom ]; then
    (cd js && npm ci --no-fund --no-audit >/dev/null)
fi
"$PY" sim/build.py
SIM_SKIP_BUILD=1 exec "$PY" -m pytest "$@"
