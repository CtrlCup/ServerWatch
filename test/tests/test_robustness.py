"""Robustheit von ServerWatch_Multi: Erreichbarkeit, Blockaden, WLAN-Wiederverbindung, Selbstheilung.

Hintergrund zum gemeldeten Fehler "ESP nach einer Weile nicht mehr erreichbar, erst Strom
trennen hilft": siehe die Tests zu blockierendem Server-Check, WLAN und Watchdog.
"""
import socket
import time

import requests

from conftest import known_bug
from simnet import ws_messages


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


@known_bug("boot-wifi-hang")
def test_boot_recovers_after_initial_auth_failures(swarm):
    """setupWiFi() wartet endlos auf WL_CONNECTED. Scheitern die ersten beiden Versuche mit
    einem nicht-'reconnectable' Grund (z. B. 202 AUTH_FAIL), versucht der Core es nie wieder,
    und loop() mit seinem Reconnect-Fallback wird nie erreicht -> nur Stromtrennen hilft."""
    a = swarm.add("Alpha", ap_up=False)
    a.set_ap(False, drop_reason=202, attempt_reason=202)
    swarm.sleep(15000)
    a.set_ap(True)
    swarm.wait_ready(a, timeout_ms=60000)


@known_bug("no-watchdog")
def test_node_recovers_from_unknown_hang(swarm):
    """Simulierter Haenger im loop() (SIM_HANG_AT_MS). Ohne Watchdog bleibt der ESP fuer
    immer unerreichbar. Erwartung: Task-/Loop-Watchdog loest einen Neustart aus."""
    a = swarm.add("Alpha", env={"SIM_HANG_AT_MS": "15000"})
    swarm.wait_ready(a)
    assert swarm.wait_for(lambda: any(e == "sim_hang" for _, e in a.events()), 20000)
    assert swarm.wait_for(lambda: a.boots() >= 2, 30000), "Kein Neustart nach Haenger"
    swarm.wait_ready(a, timeout_ms=30000)


@known_bug("ws-stale-clients")
def test_websocket_accepts_new_client_when_stale_clients_exist(swarm):
    """Die WebSocket-Lib hat 5 Slots. Tote Verbindungen (Handy im Standby, Tab eingefroren)
    blockieren Slots, weil kein Heartbeat konfiguriert ist; neue Dashboards bekommen dann
    keine Live-Updates mehr."""
    a = swarm.add("Alpha")
    swarm.wait_ready(a)
    zombies = []
    for _ in range(5):
        s = socket.create_connection((a.ip, 81 + swarm.port_offset))
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  b"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
        zombies.append(s)  # liest nie, antwortet nie auf Pings
    swarm.sleep(3000)

    def fresh_client_gets_update():
        with a.websocket(timeout_ms=3000) as ws:
            return ws_messages(ws, swarm.real_s(4000))

    try:
        assert swarm.wait_for(fresh_client_gets_update, 60000, poll_ms=2000), \
            "Neuer WebSocket-Client bekommt keine Verbindung/Updates"
    finally:
        for s in zombies:
            s.close()
