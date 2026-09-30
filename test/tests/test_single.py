"""Einzel-Version (Serverwatch.ino): Grundfunktion und Robustheit."""
import time

import requests

from conftest import known_bug
from simnet import POWER_BUTTON_PIN


def solo(swarm, **kw):
    n = swarm.add("Solo", sketch="Serverwatch", **kw)
    swarm.wait_ready(n)
    return n


def test_single_status_reports_online(swarm):
    n = solo(swarm, power=True)
    swarm.set_server("up")
    s = swarm.wait_for(lambda: (lambda j: j if j["online"] else None)(n.get("/status").json()), 8000)
    assert s == {"online": True, "reachable": True, "power": True}


def test_single_status_reports_offline_when_port_closed(swarm):
    n = solo(swarm, power=True)
    swarm.set_server("refused")
    s = swarm.wait_for(lambda: (lambda j: j if j["reachable"] is False else None)(n.get("/status").json()), 8000)
    assert s and s["online"] is False


def test_single_page_contains_config(swarm):
    n = solo(swarm)
    html = n.get("/").text
    assert "Solo" in html and "192.168.178.1:80" in html


def test_single_poweron_presses_button(swarm):
    swarm.set_server("refused")  # Server aus: Starten ist erlaubt
    n = solo(swarm, power=False)
    r = n.post("/poweron", json={}, timeout_ms=5000)
    assert r.json() == {"success": True}
    (start, dur), = n.pulses(POWER_BUTTON_PIN)
    assert 700 <= dur <= 1500


def test_single_status_responsive_when_server_powered_off(swarm):
    """Regression #1: /status liefert den gecachten Wert, statt pro Anfrage zu verbinden."""
    n = solo(swarm, power=False)
    swarm.set_server("blackhole")  # Server wird ausgeschaltet
    t0 = time.monotonic()
    try:
        n.get("/status", timeout_ms=3000)
        ok = True
    except requests.RequestException:
        ok = False
    assert ok and time.monotonic() - t0 < swarm.real_s(3000)


def test_single_wifi_reconnects_after_router_outage(swarm):
    n = solo(swarm)
    n.set_ap(False)
    assert swarm.wait_for(lambda: not n.link_up(), 5000)
    swarm.sleep(20000)
    n.set_ap(True)
    assert swarm.wait_for(lambda: n.link_up() and n.get("/", 2000).ok, 30000)
