"""Kompiliert beide Sketches mit der echten ESP32-C3-Toolchain (PlatformIO)."""
import os
import shutil
import subprocess

import pytest

from simnet import ROOT

PIO = shutil.which("pio") or os.path.expanduser("~/.platformio/penv/bin/pio")


@pytest.mark.firmware
@pytest.mark.skipif(not os.path.exists(PIO), reason="PlatformIO (pio) nicht installiert")
def test_firmware_compiles_for_esp32c3():
    r = subprocess.run([os.path.join(ROOT, "test", "firmware", "build.sh")], capture_output=True, text=True,
                       env={**os.environ, "PIO": PIO}, timeout=1800)
    assert r.returncode == 0, r.stdout[-4000:] + r.stderr[-4000:]
    assert "Firmware-Build OK" in r.stdout
