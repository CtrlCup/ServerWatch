"""Einzel-Version (Serverwatch.ino): Grundfunktion und Robustheit."""
import re
import time

import requests

from conftest import known_bug
from simnet import POWER_BUTTON_PIN, expected_hostname


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


# ------------------------------------------------------------ Hostname (RFC 1123) und Host-Allowlist
PROXMOX = "Proxmox Node 2"
GROESSTER = "Größter Server!"


def named(swarm, name, **kw):
    swarm.set_server("refused")
    n = swarm.add(name, sketch="Serverwatch", power=False, cfg={"useLogin": "false"}, **kw)
    swarm.wait_ready(n)
    return n


def serial_hostname(n):
    m = re.search(r"Hostname: (\S+)\s*$", n.serial(), re.M)  # Zeilen tragen ein Zeitstempel-Praefix
    return m.group(1) if m else None


def test_single_hostname_is_sanitized_with_mac_suffix(swarm):
    n = named(swarm, PROXMOX)
    h = expected_hostname(PROXMOX, n.ip)
    assert h == "serverwatch-proxmox-node-2-000a01"
    assert serial_hostname(n) == h
    assert re.fullmatch(r"[a-z0-9-]+", h) and len(h) <= 63


def test_single_hostname_transliterates_umlauts_and_symbols(swarm):
    n = named(swarm, GROESSTER)
    assert serial_hostname(n) == expected_hostname(GROESSTER, n.ip) == "serverwatch-groesster-server-000a01"


def test_single_hostname_is_rfc1123_for_hostile_names(swarm):
    for name in ("A" * 60, "  --x__y--  ", "äöüß"):
        n = named(swarm, name)
        h = serial_hostname(n)
        assert h == expected_hostname(name, n.ip), (name, h)
        assert re.fullmatch(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?", h) and len(h) <= 63 and "--" not in h, h


def test_single_hostname_empty_sanitized_name_uses_mac_only(swarm):
    n = named(swarm, "!!!")
    assert serial_hostname(n) == "serverwatch-" + "".join(f"{int(b):02x}" for b in n.ip.split(".")[1:])


def test_single_allowlist_accepts_sanitized_hostname(swarm):
    n = named(swarm, PROXMOX)
    h = expected_hostname(PROXMOX, n.ip)
    for host in (h, h.upper(), f"{h}.local", f"{h}.LOCAL", f"{h}.fritz.box", f"{h}.FRITZ.BOX", n.ip):
        assert n.get("/status", headers={"Host": host}).status_code == 200, host


def test_single_allowlist_rejects_old_hostname(swarm):
    n = named(swarm, PROXMOX)
    for host in ("ServerWatch-Proxmox Node 2", "ServerWatch-Proxmox-Node-2", "serverwatch-proxmox-node-2",
                 "serverwatch-proxmox-node-2.local", "ServerWatch-Proxmox%20Node%202.local"):
        assert n.get("/status", headers={"Host": host}).status_code == 403, host


def test_single_page_shows_display_name_with_spaces(swarm):
    n = named(swarm, PROXMOX)
    assert PROXMOX in n.get("/").text
    n2 = named(swarm, GROESSTER)
    assert "Größter Server" in n2.get("/").content.decode("utf-8")
