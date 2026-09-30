"""Sicherheit (ServerWatch_Multi + Serverwatch): Authentifizierung, CSRF, Eingabepruefung.

Die Steuer-Endpunkte druecken physisch den Power-/Reset-Knopf eines Servers. Jeder Aufruf
kann einen laufenden Server hart ausschalten oder neu starten.
"""
import pytest
import requests

from conftest import known_bug
from simnet import POWER_BUTTON_PIN, RESET_BUTTON_PIN


@pytest.fixture
def node(swarm):
    n = swarm.add("Alpha", power=False)
    swarm.set_server("refused")
    swarm.wait_ready(n)
    return n


def no_press(swarm, node, ms=3000):
    swarm.sleep(ms)
    return not node.pulses(POWER_BUTTON_PIN) and not node.pulses(RESET_BUTTON_PIN)


@known_bug("no-auth")
def test_dashboard_control_requires_authentication(swarm, node):
    r = node.post("/control", json={"target": "local", "action": "power"})
    assert r.status_code in (401, 403)
    assert no_press(swarm, node)


@known_bug("no-node-auth")
def test_node_control_endpoint_requires_shared_secret(swarm, node):
    """/api/control ist fuer ESP-zu-ESP-Befehle gedacht, nimmt aber Befehle von jedem an."""
    r = node.post("/api/control", json={"action": "power"})
    assert r.status_code in (401, 403)
    assert no_press(swarm, node)


@known_bug("csrf")
def test_cross_site_text_plain_post_is_rejected(swarm, node):
    """Eine fremde Webseite kann per <form enctype="text/plain"> oder fetch(mode:'no-cors')
    ohne CORS-Preflight POSTen; WebServer legt den Body trotzdem als arg("plain") ab."""
    r = node.post("/api/control", data='{"action":"power"}',
                  headers={"Content-Type": "text/plain", "Origin": "http://evil.example"})
    assert r.status_code in (400, 401, 403, 415)
    assert no_press(swarm, node)


def test_power_press_refused_while_server_online(swarm):
    """Regression #11: Ein kurzer Power-Druck bei laufendem Server loest das Herunterfahren aus;
    die API muss 'power' (Starten) bei laufendem Server ablehnen."""
    n = swarm.add("Alpha", power=True)
    swarm.set_server("up")
    swarm.wait_ready(n)
    swarm.sleep(4000)
    assert n.status()["local"]["serverOnline"] is True
    r = n.post("/control", json={"target": "local", "action": "power"})
    assert r.status_code == 409 or r.json().get("success") is False
    assert no_press(swarm, n)


def test_unknown_action_is_rejected(swarm, node):
    r = node.post("/api/control", json={"action": "selfdestruct"})
    assert r.status_code == 400


def test_malformed_json_is_rejected(swarm, node):
    r = node.post("/api/control", data="{kaputt", headers={"Content-Type": "application/json"})
    assert r.status_code == 400


def test_dashboard_control_unknown_action_reports_error(swarm, node):
    r = node.post("/control", json={"target": "local", "action": "reboot-now"})
    assert r.status_code == 400 or r.json().get("success") is False


@known_bug("ws-origin")
def test_websocket_rejects_foreign_origin(swarm, node):
    """Cross-Site WebSocket Hijacking: jede Webseite kann den Status aller Server mitlesen."""
    from websockets.exceptions import InvalidStatus
    with pytest.raises((InvalidStatus, ConnectionError, OSError)):
        with node.websocket(additional_headers={"Origin": "http://evil.example"}) as ws:
            ws.recv(timeout=swarm.real_s(3000))


# ------------------------------------------------------------------ Einzel-Version
@known_bug("csrf")
def test_single_poweron_not_triggerable_via_get(swarm):
    """GET /poweron reicht: <img src="http://esp/poweron"> auf irgendeiner Webseite oder
    Link-Prefetching im Browser druecken den Power-Knopf."""
    n = swarm.add("Solo", sketch="Serverwatch", power=False)
    swarm.set_server("refused")
    swarm.wait_ready(n)
    try:
        n.get("/poweron", timeout_ms=5000)
    except requests.RequestException:
        pass
    assert no_press(swarm, n)


@known_bug("no-auth")
def test_single_poweron_requires_authentication(swarm):
    n = swarm.add("Solo", sketch="Serverwatch", power=False)
    swarm.set_server("refused")
    swarm.wait_ready(n)
    r = n.post("/poweron", timeout_ms=5000)
    assert r.status_code in (401, 403, 405)
    assert no_press(swarm, n)


def test_single_poweron_refused_while_server_online(swarm):
    """Regression #11 fuer die Einzel-Version."""
    n = swarm.add("Solo", sketch="Serverwatch", power=True)
    swarm.set_server("up")
    swarm.wait_ready(n)
    assert swarm.wait_for(lambda: n.get("/status").json()["online"], 8000)
    r = n.get("/poweron", timeout_ms=5000)
    assert r.status_code == 409
    assert no_press(swarm, n)
