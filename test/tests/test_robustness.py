"""Robustheit von ServerWatch_Multi: Erreichbarkeit, Blockaden, WLAN-Wiederverbindung, Selbstheilung.

Hintergrund zum gemeldeten Fehler "ESP nach einer Weile nicht mehr erreichbar, erst Strom
trennen hilft": siehe die Tests zu blockierendem Server-Check, WLAN und Watchdog.
"""
import os
import re
import socket
import time

import pytest
import requests

from conftest import known_bug
from simnet import ROOT, ws_messages


def probe(swarm, node, duration_ms, every_ms=2000, timeout_ms=3000, path="/api/status"):
    """Fragt node wiederholt ab; liefert Liste der fehlgeschlagenen Zeitpunkte."""
    failures = []
    end = time.monotonic() + swarm.real_s(duration_ms)
    while time.monotonic() < end:
        t = time.monotonic()
        try:
            node.get(path, timeout_ms)
        except requests.RequestException as e:
            failures.append(type(e).__name__)
        rest = swarm.real_s(every_ms) - (time.monotonic() - t)
        if rest > 0:
            time.sleep(rest)
    return failures


def test_ui_responsive_when_server_online(swarm):
    a = swarm.add("Alpha")
    swarm.wait_ready(a)
    assert probe(swarm, a, 15000) == []


def test_ui_responsive_when_server_powered_off(swarm):
    """Regression #1: Ist der ueberwachte Server AUS (kein TCP-RST), darf der Status-Check das
    Webinterface nicht blockieren (frueher setTimeout(1000) = 1000 s in arduino-esp32 2.x)."""
    a = swarm.add("Alpha", power=False)
    swarm.set_server("refused")
    swarm.wait_ready(a)
    swarm.set_server("blackhole")  # Server wird ausgeschaltet
    failures = probe(swarm, a, 30000)
    assert failures == [], f"{len(failures)} Anfragen ohne Antwort (Timeout 3 s)"


def test_power_button_usable_when_server_powered_off(swarm):
    a = swarm.add("Alpha", power=False)
    swarm.set_server("refused")
    swarm.wait_ready(a)
    swarm.set_server("blackhole")  # Server wird ausgeschaltet
    swarm.sleep(4000)  # naechster Status-Check laeuft
    t0 = time.monotonic()
    r = a.post("/control", json={"target": "local", "action": "power"}, timeout_ms=5000)
    assert r.status_code == 200
    assert (time.monotonic() - t0) < swarm.real_s(3000)


def test_loop_never_blocks_longer_than_one_second(swarm):
    """Regression #2: mDNS-Query (3 s) und HTTP-Abfragen anderer ESPs duerfen loop() (Webserver,
    WebSocket) nicht blockieren."""
    a = swarm.add("Alpha")
    swarm.sleep(5000)
    b = swarm.add("Beta")
    swarm.wait_ready(a, b)
    swarm.sleep(30000)
    m = a.metrics()
    assert m["max_loop_ms"] < 1000, f"loop() blockierte bis zu {m['max_loop_ms']:.0f} ms"


def test_wifi_reconnects_after_router_outage(swarm):
    a = swarm.add("Alpha")
    swarm.wait_ready(a)
    a.set_ap(False, drop_reason=200)  # Beacon-Timeout, z. B. Fritzbox-Neustart
    assert swarm.wait_for(lambda: not a.link_up(), 5000)
    swarm.sleep(30000)
    a.set_ap(True)
    assert swarm.wait_for(lambda: a.link_up() and a.get("/api/status", 2000).ok, 30000), \
        "Nach Router-Neustart nicht wieder erreichbar"


def test_wifi_reconnects_after_deauth_with_nonreconnectable_reason(swarm):
    """Grund 202 (AUTH_FAIL) ist im Core nicht 'reconnectable'; der Fallback im loop()
    (alle 10 s disconnect()+begin()) muss die Verbindung trotzdem wiederherstellen."""
    a = swarm.add("Alpha")
    swarm.wait_ready(a)
    a.set_ap(False, drop_reason=202, attempt_reason=202)
    assert swarm.wait_for(lambda: not a.link_up(), 5000)
    swarm.sleep(15000)
    a.set_ap(True)
    assert swarm.wait_for(lambda: a.link_up() and a.get("/api/status", 2000).ok, 30000)


def test_wifi_reconnects_while_server_is_off(swarm):
    """Kombination aus Router-Neustart und ausgeschaltetem Server."""
    a = swarm.add("Alpha", power=False)
    swarm.set_server("refused")
    swarm.wait_ready(a)
    a.set_ap(False)
    swarm.sleep(20000)
    a.set_ap(True)
    assert swarm.wait_for(lambda: a.link_up(), 30000), "WLAN kam nicht zurueck"


def test_boot_while_router_is_starting(swarm):
    """Nach Stromausfall bootet der ESP schneller als die Fritzbox (AP noch nicht sichtbar)."""
    a = swarm.add("Alpha", ap_up=False)
    swarm.sleep(20000)
    a.set_ap(True)
    swarm.wait_ready(a, timeout_ms=30000)


@pytest.mark.parametrize("sketch", ["ServerWatch_Multi", "Serverwatch"])
def test_boot_recovers_after_initial_auth_failures(swarm, sketch):
    """Regression #5: Scheitern die ersten Verbindungsversuche beim Boot mit einem nicht
    'reconnectable' Grund (z. B. 202 AUTH_FAIL), darf setup() nicht endlos warten."""
    a = swarm.add("Alpha", sketch=sketch, ap_up=False)
    a.set_ap(False, drop_reason=202, attempt_reason=202)
    swarm.sleep(15000)
    a.set_ap(True)
    swarm.wait_ready(a, timeout_ms=60000)


@pytest.mark.parametrize("sketch", ["ServerWatch_Multi", "Serverwatch"])
def test_node_recovers_from_unknown_hang(swarm, sketch):
    """Regression #4: Simulierter Haenger im loop() (SIM_HANG_AT_MS). Der Watchdog muss einen
    Neustart ausloesen, sonst bliebe der ESP bis zum Stromtrennen unerreichbar."""
    a = swarm.add("Alpha", sketch=sketch, env={"SIM_HANG_AT_MS": "15000"})
    swarm.wait_ready(a)
    assert swarm.wait_for(lambda: any(e == "sim_hang" for _, e in a.events()), 20000)
    assert swarm.wait_for(lambda: a.boots() >= 2, 30000), "Kein Neustart nach Haenger"
    swarm.wait_ready(a, timeout_ms=30000)


def test_websocket_accepts_new_client_when_stale_clients_exist(swarm):
    """Regression #17: Die WebSocket-Lib hat 5 Slots. Tote Verbindungen (Handy im Standby, Tab
    eingefroren) muessen per Heartbeat getrennt werden, damit neue Dashboards Updates bekommen."""
    a = swarm.add("Alpha")
    swarm.wait_ready(a)
    zombies = []
    token = a.ws_token()
    for _ in range(5):
        s = socket.create_connection((a.ip, 81 + swarm.port_offset))
        s.sendall(f"GET /?t={token} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
        zombies.append(s)  # liest nie, antwortet nie auf Pings
    swarm.sleep(3000)

    def fresh_client_gets_update():
        try:
            with a.websocket(timeout_ms=3000) as ws:
                return ws_messages(ws, swarm.real_s(4000))
        except Exception:  # abgewiesen, solange alle Slots belegt sind
            return None

    try:
        assert swarm.wait_for(fresh_client_gets_update, 60000, poll_ms=2000), \
            "Neuer WebSocket-Client bekommt keine Verbindung/Updates"
    finally:
        for s in zombies:
            s.close()


def test_no_watchdog_reset_in_normal_operation(swarm):
    """Der Watchdog darf im Normalbetrieb nie ausloesen, auch nicht bei ausgeschaltetem Server
    und mit einem zweiten ESP im Netz."""
    a = swarm.add("Alpha", power=False)
    swarm.sleep(5000)
    b = swarm.add("Beta")
    swarm.set_server("blackhole")
    swarm.wait_ready(a, b)
    swarm.sleep(60000)
    assert a.boots() == 1 and b.boots() == 1, [e for _, e in a.events() + b.events() if e.startswith("restart")]


def test_node_restarts_after_long_wifi_outage(swarm):
    """Faellt das WLAN laenger als wifiRestartTimeout (5 min) aus, startet der ESP neu, als
    letzte Rueckfallebene, falls der WLAN-Treiber haengt."""
    a = swarm.add("Alpha")
    swarm.wait_ready(a)
    a.set_ap(False)
    assert swarm.wait_for(lambda: a.boots() >= 2, 330000, poll_ms=5000), "Kein Neustart nach 5 min ohne WLAN"
    a.set_ap(True)
    swarm.wait_ready(a, timeout_ms=30000)


def test_without_power_sense_server_counts_online_when_reachable(swarm):
    """Issue #19: Ohne Spannungsabgriff (usePowerSense=false) gilt der Server als online,
    sobald er erreichbar ist; 'Starten' wird dann abgelehnt, 'Herunterfahren' ist moeglich."""
    swarm.set_server("up")
    a = swarm.add("Alpha", power=False, cfg={"usePowerSense": "false"})
    swarm.wait_ready(a)
    local = swarm.wait_for(lambda: a.status()["local"] if a.status()["local"]["serverOnline"] else None, 10000)
    assert local, "Erreichbarer Server wird ohne Spannungssensor nicht als online erkannt"
    assert local["serverPower"] is None
    assert a.post("/control", json={"target": "local", "action": "power"}).status_code == 409
    assert a.post("/control", json={"target": "local", "action": "shutdown"}).status_code == 200


def test_single_without_power_sense(swarm):
    swarm.set_server("up")
    n = swarm.add("Solo", sketch="Serverwatch", power=False, cfg={"usePowerSense": "false"})
    swarm.wait_ready(n)
    s = swarm.wait_for(lambda: (lambda j: j if j["online"] else None)(n.get("/status").json()), 10000)
    assert s == {"online": True, "reachable": True, "power": None}


SKETCHES = ["ServerWatch_Multi", "Serverwatch"]
DIAG_KEYS = {"uptime_ms", "reset_reason", "free_heap", "min_free_heap", "rssi", "wifi_disconnects",
             "last_disconnect_reason", "max_loop_ms", "version"}


def sketch_version(sketch):
    with open(os.path.join(ROOT, sketch + ".ino")) as f:
        return re.search(r'firmwareVersion\s*=\s*"([^"]+)"', f.read()).group(1)


@pytest.mark.parametrize("sketch", SKETCHES)
def test_diag_reports_plausible_values_and_wifi_drop(swarm, sketch):
    """Issue #23: /api/diag liefert Diagnosewerte; eine WLAN-Trennung (Grund 15) wird gezaehlt und
    bleibt auch nach dem eigenen disconnect() des Reconnect-Fallbacks (Grund 8, ignoriert) stehen."""
    a = swarm.add("Alpha", sketch=sketch)
    swarm.wait_ready(a)
    swarm.sleep(2000)
    d = a.get("/api/diag").json()
    assert set(d) == DIAG_KEYS
    assert all(isinstance(d[k], int) and not isinstance(d[k], bool) for k in DIAG_KEYS - {"version", "max_loop_ms"})
    assert isinstance(d["max_loop_ms"], (int, float)) and 0 <= d["max_loop_ms"] < 1000
    assert d["uptime_ms"] > 0 and d["reset_reason"] == 1 and d["free_heap"] > 0 and d["rssi"] < 0
    assert (d["wifi_disconnects"], d["last_disconnect_reason"]) == (0, 0)
    assert d["version"] == sketch_version(sketch)

    a.set_ap(False, drop_reason=15, attempt_reason=15)
    assert swarm.wait_for(lambda: not a.link_up(), 5000)
    swarm.sleep(15000)  # laenger als 10 s: Fallback ruft WiFi.disconnect() (Grund 8)
    a.set_ap(True)
    assert swarm.wait_for(lambda: a.link_up() and a.get("/api/diag", 2000).ok, 30000)
    d = a.get("/api/diag").json()
    assert d["wifi_disconnects"] >= 1
    assert d["last_disconnect_reason"] == 15


@pytest.mark.parametrize("sketch", SKETCHES)
def test_reduce_tx_power_option(swarm, sketch):
    """Issue #23: reduceTxPower senkt die Sendeleistung nach WiFi.begin() und meldet das seriell."""
    default = swarm.add("Alpha", sketch=sketch)
    reduced = swarm.add("Beta", sketch=sketch, cfg={"reduceTxPower": "true"})
    swarm.wait_ready(default, reduced)
    assert "Sendeleistung" in reduced.serial()
    assert "Sendeleistung" not in default.serial()
