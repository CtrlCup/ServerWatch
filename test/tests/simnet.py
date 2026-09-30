"""Test-Harness fuer die ServerWatch-Host-Simulation.

Ein `Swarm` startet simulierte ESP32-Knoten (je ein Prozess mit eigener Loopback-IP
127.0.10.x) und steuert deren Umgebung ueber Dateien im Zustandsverzeichnis
(siehe test/sim/fakes/sim_runtime.cpp). Alle Zeitangaben in dieser Datei sind
*simulierte* Millisekunden; `scale` (SIM_TIME_SCALE) beschleunigt die Simulation.
"""
import json
import os
import random
import signal
import subprocess
import time

import requests

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN_DIR = os.environ.get("SIM_BIN_DIR") or os.path.join(ROOT, "test", ".build", "sim")

# GPIO-Belegung aus den Sketches (Standardkonfiguration)
POWER_CHECK_PIN = 4
POWER_BUTTON_PIN = 3
RESET_BUTTON_PIN = 5
SERVER_IP = "192.168.178.1"


class Node:
    def __init__(self, swarm, ip, sketch, name, env):
        self.swarm, self.ip, self.sketch, self.name, self.env = swarm, ip, sketch, name, env
        self.dir = os.path.join(swarm.state, "nodes", ip)
        self.proc = None
        os.makedirs(self.dir, exist_ok=True)

    # ------------------------------------------------------------ Lebenszyklus
    def start(self):
        env = dict(os.environ)
        env.update({
            "SIM_TIME_SCALE": str(self.swarm.scale),
            "SW_IP": self.ip,
            "SW_STATE": self.swarm.state,
            "SW_PORT_OFFSET": str(self.swarm.port_offset),
        })
        env.update(self.env)
        self.logfile = open(os.path.join(self.dir, "serial.log"), "ab")
        self.proc = subprocess.Popen([os.path.join(BIN_DIR, self.sketch)], env=env,
                                     stdout=self.logfile, stderr=subprocess.STDOUT)
        return self

    def kill(self):
        """Simuliert Stromausfall: Prozess weg, Registry-Eintrag bleibt (wie ein stummer Host)."""
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
        self.proc = None

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    # ------------------------------------------------------------ Umgebung
    def _write(self, name, value):
        tmp = os.path.join(self.dir, name + ".tmp")
        with open(tmp, "w") as f:
            f.write(value)
        os.replace(tmp, os.path.join(self.dir, name))

    def set_ap(self, up=True, drop_reason=200, attempt_reason=201):
        """WLAN-Router aus Sicht dieses Knotens an/aus. Gruende: esp_wifi_types.h."""
        self._write("ap", "up" if up else f"down {drop_reason} {attempt_reason}")

    def set_input(self, pin, level):
        self._write(f"gpio_in_{pin}", "1" if level else "0")

    def set_power(self, on):
        self.set_input(POWER_CHECK_PIN, on)

    # ------------------------------------------------------------ Beobachtung
    def read(self, name, default=""):
        try:
            with open(os.path.join(self.dir, name)) as f:
                return f.read()
        except FileNotFoundError:
            return default

    def link_up(self):
        return self.read("link", "0").strip() == "1"

    def metrics(self):
        m = {}
        for line in self.read("metrics").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                m[k] = float(v) if v.replace(".", "", 1).isdigit() else v
        return m

    def events(self):
        out = []
        for line in self.read("events.log").splitlines():
            parts = line.split(" ", 1)
            if len(parts) == 2:
                out.append((float(parts[0]), parts[1]))
        return out

    def boots(self):
        return sum(1 for _, e in self.events() if e.startswith("boot"))

    def pulses(self, pin):
        """Liste (start_ms, dauer_ms) aller HIGH-Pulse auf einem Ausgangspin."""
        res, start = [], None
        for t, e in self.events():
            if e == f"gpio {pin} 1" and start is None:
                start = t
            elif e == f"gpio {pin} 0" and start is not None:
                res.append((start, t - start))
                start = None
        return res

    def serial(self):
        return self.read("serial.log")

    # ------------------------------------------------------------ HTTP / WebSocket
    def url(self, path="/", port=80):
        return f"http://{self.ip}:{port + self.swarm.port_offset}{path}"

    def request(self, method, path, timeout_ms=4000, **kw):
        return requests.request(method, self.url(path), timeout=self.swarm.real_s(timeout_ms), **kw)

    def get(self, path, timeout_ms=4000, **kw):
        return self.request("GET", path, timeout_ms, **kw)

    def post(self, path, timeout_ms=8000, **kw):
        return self.request("POST", path, timeout_ms, **kw)

    def status(self, timeout_ms=4000):
        return self.get("/api/status", timeout_ms).json()["servers"]

    def websocket(self, timeout_ms=4000, **kw):
        from websockets.sync.client import connect
        return connect(f"ws://{self.ip}:{81 + self.swarm.port_offset}/", open_timeout=self.swarm.real_s(timeout_ms), **kw)


class Swarm:
    def __init__(self, state_dir, scale=5.0):
        self.state = str(state_dir)
        self.scale = float(scale)
        self.port_offset = random.randrange(20000, 40000, 100)
        self.nodes = []
        os.makedirs(os.path.join(self.state, "hosts"), exist_ok=True)
        self.set_server("up")

    # Zeit ------------------------------------------------------------------
    def real_s(self, sim_ms):
        return sim_ms / 1000.0 / self.scale

    def sleep(self, sim_ms):
        time.sleep(self.real_s(sim_ms))

    def wait_for(self, cond, timeout_ms, poll_ms=250, desc="Bedingung"):
        """Wartet (simulierte Zeit) bis cond() truthy ist. Gibt den Wert zurueck oder None."""
        deadline = time.monotonic() + self.real_s(timeout_ms)
        while True:
            try:
                v = cond()
                if v:
                    return v
            except (requests.RequestException, ValueError, KeyError):
                pass
            if time.monotonic() > deadline:
                return None
            self.sleep(poll_ms)

    # Umgebung --------------------------------------------------------------
    def set_server(self, mode, ip=SERVER_IP, port=None):
        """Ueberwachter Server: 'up' (Port offen), 'refused' (Host an, Port zu), 'blackhole' (Host aus)."""
        name = ip if port is None else f"{ip}_{port}"
        with open(os.path.join(self.state, "hosts", name), "w") as f:
            f.write(mode)

    # Knoten ----------------------------------------------------------------
    def add(self, name, sketch="ServerWatch_Multi", power=True, start=True, env=None, cfg=None, ap_up=True):
        ip = f"127.0.10.{len(self.nodes) + 1}"
        e = dict(env or {})
        key = "SWCFG_serverName" if sketch == "ServerWatch_Multi" else "SWCFG_nodeName"
        e.setdefault(key, name)
        for k, v in (cfg or {}).items():
            e["SWCFG_" + k] = v
        node = Node(self, ip, sketch, name, e)
        node.set_power(power)
        node.set_ap(ap_up)
        self.nodes.append(node)
        if start:
            node.start()
        return node

    def wait_ready(self, *nodes, timeout_ms=20000):
        for n in nodes:
            ok = self.wait_for(lambda: n.link_up() and n.get("/", 2000).status_code == 200, timeout_ms)
            assert ok, f"{n.name} ({n.ip}) wurde nicht erreichbar.\n--- serial ---\n{n.serial()[-3000:]}"

    def shutdown(self):
        for n in self.nodes:
            n.kill()


def remote_entry(servers, node):
    """Findet den Eintrag eines entfernten Knotens in /api/status (anhand der ESP-IP)."""
    for key, s in servers.items():
        if not s.get("isLocal") and s.get("espIP") == node.ip:
            return key, s
    return None, None


def ws_messages(ws, duration_s):
    """Sammelt WebSocket-Nachrichten fuer duration_s echte Sekunden."""
    msgs, end = [], time.monotonic() + duration_s
    while time.monotonic() < end:
        try:
            msgs.append(json.loads(ws.recv(timeout=max(0.01, end - time.monotonic()))))
        except TimeoutError:
            break
    return msgs
