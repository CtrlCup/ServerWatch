"""Schwarm-Funktion (ServerWatch_Multi): Erkennung, Datenabgleich, Fernsteuerung, Live-Updates.

Zeitkonstanten des Sketches: Scan alle 10 s (mDNS-Query blockiert 3 s), Status alle 3 s,
entfernte ESPs werden nach 60 s ohne Antwort entfernt.
"""
import json

from conftest import known_bug
from simnet import POWER_BUTTON_PIN, RESET_BUTTON_PIN, remote_entry, ws_messages

DISCOVERY_MS = 40000   # 3 Scan-Runden + Reserve
PROPAGATE_MS = 25000   # Scan (10 s) + Status/Push (3 s) + Reserve


def start_apart(swarm, *names, gap_ms=5000, **kw):
    """Startet Knoten zeitversetzt, sodass ihre Scan-Phasen nicht zusammenfallen."""
    nodes = []
    for i, n in enumerate(names):
        if i:
            swarm.sleep(gap_ms)
        nodes.append(swarm.add(n, **kw))
    swarm.wait_ready(*nodes)
    return nodes


def sees(a, b):
    return remote_entry(a.status(), b)[1]


# ------------------------------------------------------------------ Erkennung
def test_discovery_nodes_booted_apart(swarm):
    a, b = start_apart(swarm, "Alpha", "Beta")
    assert swarm.wait_for(lambda: sees(a, b), DISCOVERY_MS), "Alpha erkennt Beta nicht"
    assert swarm.wait_for(lambda: sees(b, a), DISCOVERY_MS), "Beta erkennt Alpha nicht"


@known_bug("scan-collision")
def test_discovery_nodes_booted_together(swarm):
    """Typischer Fall nach Stromausfall: alle ESPs booten gleichzeitig.
    Beide scannen dann im selben 10-s-Takt; waehrend A auf die HTTP-Antwort von B wartet,
    steckt B in seiner eigenen blockierenden mDNS-Query bzw. HTTP-Anfrage an A."""
    a = swarm.add("Alpha")
    b = swarm.add("Beta")
    swarm.wait_ready(a, b)
    assert swarm.wait_for(lambda: sees(a, b), DISCOVERY_MS), "Alpha erkennt Beta nicht"
    assert swarm.wait_for(lambda: sees(b, a), DISCOVERY_MS), "Beta erkennt Alpha nicht"


@known_bug("scan-collision")
def test_three_nodes_full_mesh(swarm):
    """Auch zeitversetzt gestartet kollidieren bei 3 Knoten zwei Scan-Phasen (Abstand < ~3 s)."""
    nodes = start_apart(swarm, "Alpha", "Beta", "Gamma", gap_ms=3500)
    for x in nodes:
        for y in nodes:
            if x is not y:
                assert swarm.wait_for(lambda: sees(x, y), DISCOVERY_MS), f"{x.name} erkennt {y.name} nicht"


@known_bug("duplicate-hostname")
def test_nodes_with_same_server_name_see_each_other(swarm):
    """Hostname = 'ServerWatch-' + serverName. Zwei Server mit gleichem Namen (z. B. beide
    Standardwert 'Heimserver') fuehren zu mDNS-Konflikt und gegenseitigem Ausblenden."""
    a, b = start_apart(swarm, "Heimserver", "Heimserver")
    assert swarm.wait_for(lambda: sees(a, b), DISCOVERY_MS), "Knoten 1 sieht Knoten 2 nicht"
    assert swarm.wait_for(lambda: sees(b, a), DISCOVERY_MS), "Knoten 2 sieht Knoten 1 nicht"


# ------------------------------------------------------------------ Datenabgleich
def test_remote_status_is_synced(swarm):
    a, b = start_apart(swarm, "Alpha", "Beta")
    entry = swarm.wait_for(lambda: sees(a, b), DISCOVERY_MS)
    assert entry, "Alpha erkennt Beta nicht"
    assert entry["serverName"] == "Beta"
    assert entry["serverOnline"] is True
    assert entry["serverPower"] is True
    assert entry["espReachable"] is True
    assert entry["pingTime"] > 0


@known_bug("remote-info-incomplete")
def test_remote_server_ip_is_synced(swarm):
    """Das Dashboard zeigt fuer entfernte Server 'Remote' statt der ueberwachten IP."""
    a, b = start_apart(swarm, "Alpha", "Beta")
    entry = swarm.wait_for(lambda: sees(a, b), DISCOVERY_MS)
    assert entry, "Alpha erkennt Beta nicht"
    assert entry["serverIP"] == "192.168.178.1"


def test_remote_change_propagates_to_api(swarm):
    a, b = start_apart(swarm, "Alpha", "Beta")
    assert swarm.wait_for(lambda: sees(a, b), DISCOVERY_MS)
    b.set_power(False)
    assert swarm.wait_for(lambda: sees(a, b)["serverPower"] is False, PROPAGATE_MS), \
        "Stromausfall bei Beta kommt bei Alpha nicht an"
    assert sees(a, b)["serverOnline"] is False
    b.set_power(True)
    assert swarm.wait_for(lambda: sees(a, b)["serverOnline"] is True, PROPAGATE_MS), \
        "Wiederkehr von Beta kommt bei Alpha nicht an"


def test_remote_change_is_pushed_via_websocket(swarm):
    a, b = start_apart(swarm, "Alpha", "Beta")
    assert swarm.wait_for(lambda: sees(a, b), DISCOVERY_MS)
    with a.websocket() as ws:
        first = json.loads(ws.recv(timeout=swarm.real_s(4000)))
        assert first["type"] == "update" and "local" in first["servers"]
        b.set_power(False)
        seen = False
        for msg in ws_messages(ws, swarm.real_s(PROPAGATE_MS)):
            _, e = remote_entry(msg["servers"], b)
            if e and e["serverPower"] is False:
                seen = True
                break
        assert seen, "Kein WebSocket-Update mit geaendertem Beta-Status"


def test_websocket_sends_periodic_updates(swarm):
    (a,) = start_apart(swarm, "Alpha")
    with a.websocket() as ws:
        msgs = ws_messages(ws, swarm.real_s(10000))
    assert len(msgs) >= 3, f"Nur {len(msgs)} Updates in 10 s (erwartet: initial + alle 3 s)"
    assert all(m["type"] == "update" for m in msgs)


@known_bug("offline-remote-state")
def test_offline_remote_is_marked_unreachable(swarm):
    """Faellt ein ESP aus, soll er als 'Nicht erreichbar' angezeigt werden, statt bis zu 60 s
    lang mit veralteten Daten als erreichbar zu gelten und danach kommentarlos zu verschwinden."""
    a, b = start_apart(swarm, "Alpha", "Beta")
    assert swarm.wait_for(lambda: sees(a, b), DISCOVERY_MS)
    b.kill()
    assert swarm.wait_for(lambda: sees(a, b) and sees(a, b)["espReachable"] is False, 30000), \
        "Ausgefallener ESP wird nicht als 'nicht erreichbar' markiert"
    swarm.sleep(60000)
    e = sees(a, b)
    assert e is not None and e["espReachable"] is False, "Ausgefallener ESP verschwindet aus dem Dashboard"


# ------------------------------------------------------------------ Fernsteuerung
def test_remote_power_command(swarm):
    a, b = start_apart(swarm, "Alpha", "Beta", power=False)
    key, _ = swarm.wait_for(lambda: remote_entry(a.status(), b)[1] and remote_entry(a.status(), b), DISCOVERY_MS)
    r = a.post("/control", json={"target": key, "action": "power"}, timeout_ms=15000)
    assert r.status_code == 200 and r.json().get("success") is True
    assert swarm.wait_for(lambda: b.pulses(POWER_BUTTON_PIN), 5000), "Power-Pin bei Beta wurde nicht betaetigt"
    assert not a.pulses(POWER_BUTTON_PIN), "Power-Pin bei Alpha darf nicht betaetigt werden"
    (start, dur), = b.pulses(POWER_BUTTON_PIN)
    assert 700 <= dur <= 1500


def test_remote_reset_command(swarm):
    a, b = start_apart(swarm, "Alpha", "Beta")
    key, _ = swarm.wait_for(lambda: remote_entry(a.status(), b)[1] and remote_entry(a.status(), b), DISCOVERY_MS)
    r = a.post("/control", json={"target": key, "action": "reset"}, timeout_ms=15000)
    assert r.status_code == 200 and r.json().get("success") is True
    assert swarm.wait_for(lambda: b.pulses(RESET_BUTTON_PIN), 5000), "Reset-Pin bei Beta wurde nicht betaetigt"


def test_local_power_command_via_dashboard_endpoint(swarm):
    (a,) = start_apart(swarm, "Alpha", power=False)
    swarm.set_server("blackhole")
    r = a.post("/control", json={"target": "local", "action": "power"}, timeout_ms=60000)
    assert r.status_code == 200
    assert swarm.wait_for(lambda: a.pulses(POWER_BUTTON_PIN), 5000)


@known_bug("shutdown-not-implemented")
def test_remote_shutdown_command(swarm):
    """Der Dashboard-Button 'Herunterfahren' sendet action=shutdown, die Firmware kennt die
    Aktion nicht: es passiert nichts, trotzdem meldet die API Erfolg."""
    a, b = start_apart(swarm, "Alpha", "Beta")
    key, _ = swarm.wait_for(lambda: remote_entry(a.status(), b)[1] and remote_entry(a.status(), b), DISCOVERY_MS)
    r = a.post("/control", json={"target": key, "action": "shutdown"}, timeout_ms=15000)
    assert r.status_code == 200
    assert swarm.wait_for(lambda: b.pulses(POWER_BUTTON_PIN), 10000), "Shutdown hat den Power-Pin nicht betaetigt"


@known_bug("control-always-success")
def test_control_unknown_target_reports_error(swarm):
    (a,) = start_apart(swarm, "Alpha")
    r = a.post("/control", json={"target": "ServerWatch-GibtEsNicht", "action": "power"})
    assert r.status_code >= 400 or r.json().get("success") is False


@known_bug("control-always-success")
def test_control_unreachable_remote_reports_error(swarm):
    a, b = start_apart(swarm, "Alpha", "Beta")
    key, _ = swarm.wait_for(lambda: remote_entry(a.status(), b)[1] and remote_entry(a.status(), b), DISCOVERY_MS)
    b.kill()
    r = a.post("/control", json={"target": key, "action": "power"}, timeout_ms=20000)
    assert r.status_code >= 400 or r.json().get("success") is False
