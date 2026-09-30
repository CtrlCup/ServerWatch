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


@known_bug("ws-nested-servers")
def test_dashboard_renders_cards_from_websocket_update(swarm, tmp_path):
    """Die Firmware schickt {"type":"update","servers":{"servers":{...}}} (doppelt verschachtelt).
    Das Dashboard rendert daraus eine einzelne Karte 'Unbekannt', bis der naechste HTTP-Poll
    (alle 5 s) wieder die richtigen Karten zeichnet -> Flackern."""
    a, b = discovered_pair(swarm)
    with a.websocket() as ws:
        msg = ws.recv(timeout=swarm.real_s(4000))
    r = render(tmp_path, a, ws_messages=[msg])
    titles = sorted(c["title"] for c in r["cards"])
    assert titles == ["Alpha", "Beta"], f"Nach WebSocket-Update gerendert: {titles}"


@known_bug("dashboard-xss")
def test_dashboard_escapes_remote_server_name(swarm, tmp_path):
    """serverName eines (beliebigen, per mDNS auftauchenden) Geraets landet ungefiltert in
    innerHTML -> Stored XSS im Dashboard, von dem aus alle Server geschaltet werden koennen."""
    evil = '<img src=x onerror="window.__xss=1">Evil'
    a, b = discovered_pair(swarm, name_b=evil)
    r = render(tmp_path, a)
    assert r["injectedElements"] == 0, "HTML aus serverName wurde als Markup eingefuegt"
    assert any(c["title"] == evil for c in r["cards"]), [c["title"] for c in r["cards"]]


@known_bug("dashboard-xss")
def test_dashboard_hostname_cannot_break_out_of_onclick(swarm, tmp_path):
    """Der mDNS-Hostname dient als Schluessel und wird in onclick="powerAction('<key>',...)"
    eingesetzt. Ein Hostname mit ' bricht aus dem String aus und fuehrt Code aus."""
    evil = "x');window.__xss=1;('"
    a, b = discovered_pair(swarm, name_b=evil, power=False)
    r = render(tmp_path, a, click=[".card .btn-primary"])
    assert r["xss"] is False, "Code aus dem Hostnamen wurde beim Klick ausgefuehrt"
