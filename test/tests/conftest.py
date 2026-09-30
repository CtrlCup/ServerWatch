import os
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))
from simnet import ROOT, Swarm  # noqa: E402

# Bekannte, als GitHub-Issue erfasste Fehler. Tests fuer diese Fehler sind als
# xfail(strict=True) markiert: Die Suite bleibt gruen, solange der Fehler besteht.
# Ist ein Fehler behoben, schlaegt der Test mit XPASS fehl -> Eintrag hier und den
# Marker am Test entfernen. Schluessel -> Issue-Nummer auf GitHub (CtrlCup/ServerWatch).
KNOWN_BUGS = {
    "dashboard-xss": 7,
    "no-auth": 8,
    "csrf": 9,
    "no-node-auth": 10,
    "power-guard": 11,
    "shutdown-not-implemented": 12,
    "control-always-success": 13,
    "duplicate-hostname": 14,
    "offline-remote-state": 15,
    "remote-info-incomplete": 16,
    "ws-origin": 18,
}


def known_bug(key):
    issue = KNOWN_BUGS.get(key)
    ref = f"#{issue}" if issue else key
    return pytest.mark.xfail(strict=True, reason=f"Bekannter Fehler {ref} ({key})")


@pytest.fixture(scope="session", autouse=True)
def build_sim():
    if os.environ.get("SIM_SKIP_BUILD") != "1":
        subprocess.run([sys.executable, os.path.join(ROOT, "test", "sim", "build.py")], check=True,
                       stdout=subprocess.DEVNULL)


@pytest.fixture
def swarm(tmp_path, request):
    s = Swarm(tmp_path / "state", scale=float(os.environ.get("SIM_TIME_SCALE", "5")))
    yield s
    s.shutdown()
    # Bei Fehlschlag die seriellen Logs der Knoten anhaengen
    rep = getattr(request.node, "rep_call", None)
    if rep is not None and rep.failed:
        for n in s.nodes:
            print(f"\n===== serial {n.name} ({n.ip}) =====\n{n.serial()[-4000:]}")


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    setattr(item, "rep_" + rep.when, rep)
