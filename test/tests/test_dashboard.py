"""Dashboard (im ESP eingebettetes HTML/JS von ServerWatch_Multi), gerendert in jsdom.

Die Seite und die Statusdaten kommen live aus simulierten Knoten; nur WebSocket/fetch im
Browser werden gestubbt (test/js/render_dashboard.mjs).
"""
import json
import os
import shutil
import subprocess

import pytest

from conftest import known_bug
from simnet import ROOT, remote_entry

JS_DIR = os.path.join(ROOT, "test", "js")
DISCOVERY_MS = 40000


@pytest.fixture(scope="module", autouse=True)
def jsdom_available():
    if not shutil.which("node"):
        pytest.skip("node nicht installiert")
    if not os.path.isdir(os.path.join(JS_DIR, "node_modules", "jsdom")):
        if not shutil.which("npm"):
            pytest.skip("npm nicht installiert")
        subprocess.run(["npm", "install", "--no-fund", "--no-audit"], cwd=JS_DIR, check=True,
                       stdout=subprocess.DEVNULL)


def render(tmp_path, node, status=None, ws_messages=None, click=None):
    spec = {
        "html": node.get("/").text,
        "url": node.url("/"),
        "statusResponse": status if status is not None else node.get("/api/status").json(),
        "wsMessages": ws_messages or [],
        "click": click or [],
    }
    f = tmp_path / "render.json"
    f.write_text(json.dumps(spec))
    out = subprocess.run(["node", os.path.join(JS_DIR, "render_dashboard.mjs"), str(f)],
                         capture_output=True, text=True, timeout=30, check=True)
    return json.loads(out.stdout)


def discovered_pair(swarm, name_a="Alpha", name_b="Beta", **kw_b):
    a = swarm.add(name_a)
    swarm.sleep(5000)
    b = swarm.add(name_b, **kw_b)
    swarm.wait_ready(a, b)
    assert swarm.wait_for(lambda: remote_entry(a.status(), b)[1], DISCOVERY_MS), "Discovery fehlgeschlagen"
    return a, b


def test_dashboard_renders_cards_from_http_status(swarm, tmp_path):
    a, b = discovered_pair(swarm)
    r = render(tmp_path, a)
    titles = sorted(c["title"] for c in r["cards"])
    assert titles == ["Alpha", "Beta"], r
    assert sorted(c["badge"] for c in r["cards"]) == ["Lokal", "Remote"]
    assert r["errors"] == []


def test_dashboard_renders_cards_from_websocket_update(swarm, tmp_path):
    """Regression #6: WS-Updates muessen {"type":"update","servers":{...}} liefern (nicht doppelt
    verschachtelt), sonst rendert das Dashboard eine einzelne Karte 'Unbekannt'."""
    a, b = discovered_pair(swarm)
    with a.websocket() as ws:
        msg = ws.recv(timeout=swarm.real_s(4000))
    r = render(tmp_path, a, ws_messages=[msg])
    titles = sorted(c["title"] for c in r["cards"])
    assert titles == ["Alpha", "Beta"], f"Nach WebSocket-Update gerendert: {titles}"


def test_dashboard_escapes_remote_server_name(swarm, tmp_path):
    """Regression #7: serverName eines anderen Geraets darf nie als Markup eingefuegt werden."""
    evil = '<img src=x onerror="window.__xss=1">Evil'
    a, b = discovered_pair(swarm, name_b=evil)
    r = render(tmp_path, a)
    assert r["injectedElements"] == 0, "HTML aus serverName wurde als Markup eingefuegt"
    assert any(c["title"] == evil[:32] for c in r["cards"]), [c["title"] for c in r["cards"]]  # als Text, auf 32 Zeichen gekuerzt


def rogue_status(**fields):
    base = {"id": "0a0b0c0d0e0f", "hostname": "serverwatch-evil-0d0e0f", "serverName": "Evil",
            "serverIP": "10.0.0.1", "serverPort": 80, "serverOnline": False, "serverPower": False,
            "powerSense": True, "pingTime": 0, "hasReset": True, "version": "1.0", "uptime": 1, "rssi": -50}
    base.update(fields)
    return base


def test_dashboard_key_cannot_break_out_of_onclick(swarm, tmp_path):
    """Der Schluessel eines Remote-ESP (seine id) darf keinen Code einschleusen, auch wenn
    ein gefaelschtes Geraet eine praeparierte id schickt (Issues #7, #14)."""
    a = swarm.add("Alpha")
    swarm.wait_ready(a)
    evil = swarm.add_rogue("serverwatch-evil", {"/api/localstatus": (200, rogue_status(id="');__xss=1;('"))})
    swarm.sleep(DISCOVERY_MS)
    r = render(tmp_path, a, click=[".card .btn-primary"])
    assert r["xss"] is False, "Code aus der id wurde beim Klick ausgefuehrt"
    assert not remote_entry(a.status(), evil)[1], "ESP mit ungueltiger id wurde uebernommen"


def test_dashboard_header_matches_source():
    """Issue #21: dashboard_html.h wird aus serverwatch_Multi_interface.html erzeugt."""
    r = subprocess.run(["python3", os.path.join(ROOT, "tools", "embed_html.py"), "--check"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_dashboard_marks_unreachable_remote(swarm, tmp_path):
    """Issue #15: Ein ausgefallener ESP wird als nicht erreichbar gezeigt, Buttons sind gesperrt."""
    a, b = discovered_pair(swarm)
    b.kill()
    assert swarm.wait_for(lambda: remote_entry(a.status(), b)[1]["espReachable"] is False, 30000)
    r = render(tmp_path, a)
    card = next(c for c in r["cards"] if c["title"] == "Beta")
    assert any(i.startswith("ESP Status: Nicht erreichbar seit") for i in card["info"]), card["info"]
    assert all(btn["disabled"] for btn in card["buttons"])


def test_dashboard_shows_server_address_and_buttons(swarm, tmp_path):
    swarm.set_server("refused")
    a = swarm.add("Alpha", power=False)
    swarm.wait_ready(a)
    r = render(tmp_path, a)
    (card,) = r["cards"]
    assert "Server: 192.168.178.1:80" in card["info"]
    texts = {b["text"]: b["disabled"] for b in card["buttons"]}
    assert texts == {"Starten": False, "Reset": True, "Herunterfahren": True}


def test_dashboard_warns_about_default_credentials_and_disabled_swarm(swarm, tmp_path):
    a = swarm.add("Alpha", cfg={"webUser": None, "webPassword": None, "swarmKey": None})
    a.env["SWCFG_webPassword"] = "serverwatch"  # Standard-Zugangsdaten fuer die Anfragen des Tests
    swarm.wait_ready(a)
    r = render(tmp_path, a)
    assert "Standard-Passwort" in r["banner"] and "Schwarm deaktiviert" in r["banner"]


def test_dashboard_has_no_banner_when_configured(swarm, tmp_path):
    a = swarm.add("Alpha")
    swarm.wait_ready(a)
    assert render(tmp_path, a)["banner"] == ""
