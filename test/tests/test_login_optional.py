"""Optionaler Login (useLogin) und Host-Allowlist gegen DNS-Rebinding, fuer beide Sketches.

Spezifikation: useLogin=false (Standard) -> kein Basic Auth; useLogin=true -> Login wie bisher.
In BEIDEN Modi gilt: Host-Allowlist (IP, Hostname, <host>.local, <host>.<localDomain>), Origin-/JSON-Schutz,
keine CORS-Header, WebSocket nur mit Token und eigenem/leerem Origin, Schwarm-Endpunkte unveraendert.
"""
import socket

import pytest

from simnet import POWER_BUTTON_PIN, SWARM_KEY, ws_messages

LOGIN = [pytest.param("false", id="login-off"), pytest.param("true", id="login-on")]
SKETCHES = [pytest.param("ServerWatch_Multi", id="multi"), pytest.param("Serverwatch", id="single")]

READ_PATHS = {
    "ServerWatch_Multi": ["/", "/api/status", "/api/wstoken", "/api/diag"],
    "Serverwatch": ["/", "/status", "/api/diag"],
}
ACTION = {"ServerWatch_Multi": "/control", "Serverwatch": "/poweron"}
BODY = {"ServerWatch_Multi": {"target": "local", "action": "power"}, "Serverwatch": {}}


def make(swarm, sketch, login, cfg=None, power=False, **kw):
    """Startet einen Knoten; Server aus ('refused'), damit der Power-Druck erlaubt ist."""
    swarm.set_server("refused")
    c = {"useLogin": login}
    c.update(cfg or {})
    name = "Alpha" if sketch == "ServerWatch_Multi" else "Solo"
    n = swarm.add(name, sketch=sketch, power=power, cfg=c, **kw)
    swarm.wait_ready(n)
    return n


def hostname(node):
    if node.sketch == "ServerWatch_Multi":
        return node.status()["local"]["hostname"]
    return "ServerWatch-" + node.name


def port(swarm):
    return 80 + swarm.port_offset


def pressed(swarm, node, ms=3000):
    return bool(swarm.wait_for(lambda: node.pulses(POWER_BUTTON_PIN), ms))


def no_press(swarm, node, ms=3000):
    swarm.sleep(ms)
    return not node.pulses(POWER_BUTTON_PIN)


def act(node, **kw):
    return node.post(ACTION[node.sketch], json=BODY[node.sketch], **kw)


def raw_request(node, swarm, request_bytes):
    """Rohe HTTP-Anfrage (fuer Faelle, die requests nicht sauber erzeugt); liefert Statuszeile."""
    s = socket.create_connection((node.ip, port(swarm)), timeout=swarm.real_s(4000))
    try:
        s.sendall(request_bytes)
        data = b""
        while b"\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        return data.split(b"\r\n", 1)[0].decode(errors="replace")
    finally:
        s.close()


# ============================================================== useLogin=false
@pytest.mark.parametrize("sketch", SKETCHES)
def test_login_off_read_endpoints_need_no_auth(swarm, sketch):
    n = make(swarm, sketch, "false")
    for path in READ_PATHS[sketch]:
        assert n.get(path, auth=None).status_code == 200, path


@pytest.mark.parametrize("sketch", SKETCHES)
def test_login_off_action_executes_without_auth(swarm, sketch):
    n = make(swarm, sketch, "false")
    assert act(n, auth=None).status_code == 200
    assert pressed(swarm, n)


@pytest.mark.parametrize("sketch", SKETCHES)
def test_login_off_wrong_authorization_header_is_ignored(swarm, sketch):
    """Auch viele schnelle Anfragen mit falschem/kaputtem Header: nie 401 oder 429."""
    n = make(swarm, sketch, "false")
    path = READ_PATHS[sketch][1]
    for value in ["Basic YWRtaW46ZmFsc2No", "Basic", "Basic !!!", "Bearer xyz", "Basic " + "A" * 300] * 2:
        r = n.get(path, auth=None, headers={"Authorization": value})
        assert r.status_code == 200, value
    r = n.get(path, auth=("admin", "falsch"))
    assert r.status_code == 200
    r = act(n, auth=("admin", "falsch"))
    assert r.status_code == 200


def test_login_off_multi_reports_no_default_credentials(swarm):
    swarm.set_server("refused")
    n = swarm.add("Alpha", cfg={"useLogin": "false", "webUser": None, "webPassword": None}, power=False)
    n.env["SWCFG_webPassword"] = "serverwatch"
    swarm.wait_ready(n)
    j = n.get("/api/status", auth=None).json()
    assert j["defaultCredentials"] is False


def test_login_off_single_has_no_default_password_warning(swarm):
    swarm.set_server("refused")
    n = swarm.add("Solo", sketch="Serverwatch", power=False,
                  cfg={"useLogin": "false", "webUser": None, "webPassword": None})
    swarm.wait_ready(n)
    for path in ("/", "/status"):
        body = n.get(path, auth=None).text
        assert '"defaultCredentials":true' not in body.replace(" ", ""), path
        assert "Standard-Passwort" not in body, path


@pytest.mark.parametrize("sketch", SKETCHES)
def test_sketch_default_is_login_off(swarm, sketch):
    """Ohne SWCFG_useLogin gilt der Sketch-Standard (const bool useLogin = false); reduceTxPower (Standard an) stoert nicht."""
    n = make(swarm, sketch, None)
    assert "SWCFG_useLogin" not in n.env
    for path in READ_PATHS[sketch]:
        assert n.get(path, auth=None).status_code == 200, path
    assert act(n, auth=None).status_code == 200
    assert pressed(swarm, n)


def test_login_off_multi_websocket_needs_token_but_no_login(swarm):
    n = make(swarm, "ServerWatch_Multi", "false")
    token = n.get("/api/wstoken", auth=None).json()["token"]
    assert len(token) == 32
    with n.websocket(path="/?t=" + token) as ws:
        assert ws_messages(ws, swarm.real_s(4000))
    for path in ["/", "/?t=", "/?t=0123456789abcdef0123456789abcdef", "/?t=" + token + "x"]:
        try:
            with n.websocket(path=path) as ws:
                msgs = ws_messages(ws, swarm.real_s(3000))
        except Exception:
            msgs = []
        assert msgs == [], path


# ============================================================== useLogin=true
@pytest.mark.parametrize("sketch", SKETCHES)
def test_login_on_requires_auth_and_throttles(swarm, sketch):
    n = make(swarm, sketch, "true")
    for path in READ_PATHS[sketch]:
        assert n.get(path, auth=None).status_code == 401, path
        swarm.sleep(1200)
    assert n.get(READ_PATHS[sketch][0]).status_code == 200
    assert act(n, auth=None).status_code == 401
    swarm.sleep(1200)
    assert act(n, auth=("admin", "falsch1")).status_code == 401
    assert act(n, auth=("admin", "falsch2")).status_code == 429  # zweiter Fehlversuch < 1 s
    swarm.sleep(1500)
    assert n.get(READ_PATHS[sketch][0]).status_code == 200
    assert no_press(swarm, n)


def test_login_on_multi_default_credentials_only_with_default_password(swarm):
    swarm.set_server("refused")
    n = swarm.add("Alpha", cfg={"useLogin": "true", "webUser": None, "webPassword": None}, power=False)
    n.env["SWCFG_webPassword"] = "serverwatch"
    swarm.wait_ready(n)
    assert n.get("/api/status").json()["defaultCredentials"] is True
    m = make(swarm, "ServerWatch_Multi", "true")
    assert m.get("/api/status").json()["defaultCredentials"] is False


# ============================================================== Host-Allowlist (beide Modi, beide Sketches)
def allowed_hosts(node, swarm, domain="fritz.box"):
    h = hostname(node)
    p = port(swarm)
    hosts = [node.ip, f"{node.ip}:{p}", h, f"{h}:{p}", h.upper(), h.lower(), f"{h}.local", f"{h}.LOCAL", f"{h}.local:{p}"]
    if domain:
        hosts += [f"{h}.{domain}", f"{h}.{domain.upper()}", f"{h}.{domain}:{p}"]
    return hosts


def foreign_hosts(node):
    h = hostname(node)
    hosts = ["evil.com", "evil.com:80", f"{h}.evil.com", f"{h}.fritz.box.evil.com", f"{h}x", f"x{h}", f"x{h}.local",
             f"{h}.local.evil.com", "localhost", "127.0.0.1", "192.168.178.1"]
    return hosts


@pytest.mark.parametrize("sketch", SKETCHES)
@pytest.mark.parametrize("login", LOGIN)
def test_host_allowlist_accepts_own_names(swarm, sketch, login):
    n = make(swarm, sketch, login)
    path = READ_PATHS[sketch][1]
    for host in allowed_hosts(n, swarm):
        # Auth immer mitsenden: im Login-Modus noetig, ohne Login egal
        r = n.get(path, headers={"Host": host})
        assert r.status_code == 200, (host, r.status_code)
        swarm.sleep(50)


@pytest.mark.parametrize("sketch", SKETCHES)
@pytest.mark.parametrize("login", LOGIN)
def test_host_allowlist_rejects_foreign_hosts_on_all_endpoints(swarm, sketch, login):
    n = make(swarm, sketch, login)
    paths = READ_PATHS[sketch] + ["/api/nonce"] if sketch == "ServerWatch_Multi" else READ_PATHS[sketch]
    for host in foreign_hosts(n):
        for path in paths:
            r = n.get(path, headers={"Host": host})
            assert r.status_code == 403, (host, path, r.status_code)
        r = act(n, headers={"Host": host})
        assert r.status_code == 403, (host, "action", r.status_code)
    assert no_press(swarm, n)


@pytest.mark.parametrize("sketch", SKETCHES)
@pytest.mark.parametrize("login", LOGIN)
def test_host_allowlist_rejects_empty_host(swarm, sketch, login):
    n = make(swarm, sketch, login)
    path = READ_PATHS[sketch][1]
    auth = b"Authorization: Basic YWRtaW46dGVzdC1wYXNzd29yZA==\r\n" if login == "true" else b""
    line = raw_request(n, swarm, f"GET {path} HTTP/1.1\r\nHost:\r\n".encode() + auth + b"Connection: close\r\n\r\n")
    assert " 403 " in line, line


@pytest.mark.parametrize("login", LOGIN)
def test_host_allowlist_rejects_rebinding_even_with_valid_credentials_multi(swarm, login):
    """DNS-Rebinding: Host und Origin lauten beide evil; Aktion darf nie ausgefuehrt werden."""
    n = make(swarm, "ServerWatch_Multi", login)
    r = act(n, headers={"Host": "evil.com", "Origin": "http://evil.com"})
    assert r.status_code == 403
    assert no_press(swarm, n)


@pytest.mark.parametrize("sketch", SKETCHES)
def test_host_allowlist_local_domain_empty_means_none(swarm, sketch):
    n = make(swarm, sketch, "false", cfg={"localDomain": ""})
    h = hostname(n)
    path = READ_PATHS[sketch][1]
    assert n.get(path, headers={"Host": f"{h}.local"}).status_code == 200
    assert n.get(path, headers={"Host": h}).status_code == 200
    assert n.get(path, headers={"Host": f"{h}.fritz.box"}).status_code == 403
    assert n.get(path, headers={"Host": f"{h}."}).status_code == 403


@pytest.mark.parametrize("sketch", SKETCHES)
def test_host_allowlist_custom_local_domain(swarm, sketch):
    n = make(swarm, sketch, "false", cfg={"localDomain": "home.lan"})
    h = hostname(n)
    path = READ_PATHS[sketch][1]
    assert n.get(path, headers={"Host": f"{h}.home.lan"}).status_code == 200
    assert n.get(path, headers={"Host": f"{h}.HOME.LAN:80"}).status_code == 200
    assert n.get(path, headers={"Host": f"{h}.fritz.box"}).status_code == 403
    assert n.get(path, headers={"Host": f"{h}.home.lan.evil.com"}).status_code == 403


def test_single_hostname_is_case_insensitive(swarm):
    n = make(swarm, "Serverwatch", "false")
    path = "/status"
    for host in ("ServerWatch-Solo", "serverwatch-solo", "SERVERWATCH-SOLO.local", "serverwatch-solo.fritz.box"):
        assert n.get(path, headers={"Host": host}).status_code == 200, host
    assert n.get(path, headers={"Host": "ServerWatch-Solox"}).status_code == 403


# ============================================================== Origin-/JSON-Schutz (beide Modi)
@pytest.mark.parametrize("sketch", SKETCHES)
@pytest.mark.parametrize("login", LOGIN)
def test_action_rejects_non_json_content_type(swarm, sketch, login):
    n = make(swarm, sketch, login)
    for ctype, body in [("text/plain", "{}"), ("application/x-www-form-urlencoded", "target=local&action=power")]:
        r = n.post(ACTION[sketch], data=body, headers={"Content-Type": ctype})
        assert r.status_code == 415, ctype
    assert no_press(swarm, n)


@pytest.mark.parametrize("sketch", SKETCHES)
@pytest.mark.parametrize("login", LOGIN)
def test_action_rejects_foreign_origin(swarm, sketch, login):
    n = make(swarm, sketch, login)
    h = hostname(n)
    for origin in ["http://evil.com", "null", f"https://{n.ip}", f"http://{n.ip}/x", f"http://{h}.evil.com",
                   f"http://{h}.fritz.box.evil.com"]:
        r = act(n, headers={"Origin": origin})
        assert r.status_code == 403, origin
    assert no_press(swarm, n)


@pytest.mark.parametrize("sketch", SKETCHES)
@pytest.mark.parametrize("login", LOGIN)
def test_action_accepts_own_origins(swarm, sketch, login):
    n = make(swarm, sketch, login)
    h = hostname(n)
    origin = f"http://{h}.fritz.box"
    assert act(n, headers={"Origin": origin}).status_code == 200
    assert pressed(swarm, n)
    # Zweite Aktion erst nach Ende des Tastendrucks/Sperrzeit; Statuscode reicht, kein weiterer Druck noetig
    swarm.sleep(6000)
    assert act(n, headers={"Origin": f"http://{n.ip}:{port(swarm)}"}).status_code == 200


# ============================================================== keine CORS-Header (beide Modi)
@pytest.mark.parametrize("sketch", SKETCHES)
@pytest.mark.parametrize("login", LOGIN)
def test_no_cors_headers(swarm, sketch, login):
    n = make(swarm, sketch, login)
    own = f"http://{n.ip}:{port(swarm)}"
    for origin in ("http://evil.com", own):
        for method, path in [("GET", READ_PATHS[sketch][0]), ("GET", READ_PATHS[sketch][1]),
                             ("OPTIONS", ACTION[sketch]), ("OPTIONS", READ_PATHS[sketch][1]), ("OPTIONS", "/")]:
            r = n.request(method, path, headers={"Origin": origin, "Access-Control-Request-Method": "POST"})
            bad = [h for h in r.headers if h.lower().startswith("access-control-")]
            assert not bad, (method, path, origin, bad)


# ============================================================== WebSocket (Multi)
@pytest.mark.parametrize("login", LOGIN)
def test_websocket_origin_rules_in_both_modes(swarm, login):
    from websockets.exceptions import InvalidStatus
    n = make(swarm, "ServerWatch_Multi", login)
    for origin in ["http://evil.com", "null"]:
        with pytest.raises((InvalidStatus, ConnectionError, OSError)):
            with n.websocket(additional_headers={"Origin": origin}) as ws:
                ws.recv(timeout=swarm.real_s(3000))
    with n.websocket(additional_headers={"Origin": f"http://{n.ip}"}) as ws:
        assert ws_messages(ws, swarm.real_s(4000))
    with n.websocket() as ws:  # ohne Origin
        assert ws_messages(ws, swarm.real_s(4000))


# ============================================================== Schwarm-Endpunkte unveraendert
@pytest.mark.parametrize("login", LOGIN)
def test_swarm_endpoints_unchanged_in_both_modes(swarm, login):
    n = make(swarm, "ServerWatch_Multi", login)
    nonce = n.get("/api/nonce", auth=None)
    assert nonce.status_code == 200
    # falscher Schluessel -> abgelehnt, richtiger Schluessel -> akzeptiert
    assert n.signed_control("power", key="falscher-schluessel").status_code in (401, 403)
    assert n.signed_control("power", key=SWARM_KEY).status_code == 200
    assert pressed(swarm, n)
    assert n.post("/api/control", auth=None, json={"action": "power"}).status_code in (400, 401, 403)
    assert n.get("/api/localstatus?c=" + "0" * 32, auth=None).status_code == 200
    assert n.get("/api/localstatus?c=xyz", auth=None).status_code == 400
