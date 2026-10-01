"""Sicherheit (ServerWatch_Multi + Serverwatch): Login, CSRF/DNS-Rebinding, Schwarm-Signaturen,
WebSocket-Token/Origin, Eingabepruefung. Testfaelle nach Security-Review (Issues #8, #9, #10, #18, #20).

Die Steuer-Endpunkte druecken physisch den Power-/Reset-Knopf eines Servers.
"""
import secrets

import pytest
import requests

from simnet import POWER_BUTTON_PIN, RESET_BUTTON_PIN, SWARM_KEY, WEB_PASSWORD, hmac_hex, remote_entry, ws_messages

DISCOVERY_MS = 40000


@pytest.fixture
def node(swarm):
    swarm.set_server("refused")  # Server aus: 'power' waere erlaubt
    n = swarm.add("Alpha", power=False)
    swarm.wait_ready(n)
    return n


def no_press(swarm, node, ms=3000):
    swarm.sleep(ms)
    return not node.pulses(POWER_BUTTON_PIN) and not node.pulses(RESET_BUTTON_PIN)


def control(node, **kw):
    return node.post("/control", json={"target": "local", "action": "power"}, **kw)


# ============================================================== Login (#8)
@pytest.mark.parametrize("auth", [None, ("admin", "falsch"), ("", ""), ("admin", "")])
def test_dashboard_control_requires_authentication(swarm, node, auth):
    assert control(node, auth=auth).status_code == 401
    assert no_press(swarm, node)


def test_malformed_authorization_header_is_rejected(swarm, node):
    for value in ["Basic", "Basic !!!", "Basic " + "A" * 500, "Bearer xyz", "Digest foo"]:
        r = node.post("/control", auth=None, headers={"Authorization": value}, json={"target": "local", "action": "power"})
        assert r.status_code in (401, 429), value
        swarm.sleep(1200)  # Bremse nach Fehlversuch abwarten
    assert no_press(swarm, node)


@pytest.mark.parametrize("path", ["/", "/api/status", "/api/wstoken", "/api/diag"])
def test_read_endpoints_require_authentication(node, path):
    assert node.get(path, auth=None).status_code == 401
    assert node.get(path).status_code == 200


def test_failed_login_is_throttled(swarm, node):
    """Nach einem Fehlversuch wird dieselbe IP kurz gebremst (kein schnelles Durchprobieren)."""
    assert control(node, auth=("admin", "falsch1")).status_code == 401
    assert control(node, auth=("admin", "falsch2")).status_code == 429
    swarm.sleep(1500)
    assert node.get("/api/status").status_code == 200


def test_wstoken_is_not_cached(node):
    r = node.get("/api/wstoken")
    assert r.headers.get("Cache-Control") == "no-store"
    assert len(r.json()["token"]) == 32


def test_diag_is_not_cached_not_cross_origin_and_leaks_no_secrets(node):
    for headers in ({}, {"Origin": "http://evil.example"}):
        r = node.get("/api/diag", headers=headers)
        assert r.status_code == 200
        assert r.headers.get("Cache-Control") == "no-store"
        assert r.headers.get("Content-Type", "").startswith("application/json")
        assert not [h for h in r.headers if h.lower().startswith("access-control-allow-")]
        for secret in ("DEIN_WLAN_NAME", "DEIN_WLAN_PASSWORT", WEB_PASSWORD, SWARM_KEY, node.ws_token()):
            assert secret not in r.text
    assert node.post("/api/diag", json={}).status_code in (404, 405)


def test_default_credentials_are_flagged(swarm):
    """Ohne eigenes Web-Passwort (secrets.h) meldet das Dashboard die Standard-Zugangsdaten."""
    n = swarm.add("Alpha", cfg={"webUser": None, "webPassword": None})
    n.env["SWCFG_webPassword"] = "serverwatch"  # Standard-Zugangsdaten fuer die Anfragen des Tests
    swarm.wait_ready(n)
    assert n.get("/api/status").json()["defaultCredentials"] is True


def test_custom_credentials_are_not_flagged(node):
    assert node.get("/api/status").json()["defaultCredentials"] is False


def test_single_requires_authentication(swarm):
    swarm.set_server("refused")
    n = swarm.add("Solo", sketch="Serverwatch", power=False)
    swarm.wait_ready(n)
    for path in ["/", "/status", "/api/diag"]:
        assert n.get(path, auth=None).status_code == 401, path
    r = n.post("/poweron", auth=None, json={})
    assert r.status_code == 401
    assert no_press(swarm, n)


# ============================================================== CSRF / DNS-Rebinding (#9)
@pytest.mark.parametrize("ctype,body", [
    ("text/plain", '{"target":"local","action":"power"}'),
    ("application/x-www-form-urlencoded", "target=local&action=power"),
    ("multipart/form-data; boundary=x", "--x\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--x--\r\n"),
])
def test_control_rejects_non_json_content_type(swarm, node, ctype, body):
    r = node.post("/control", data=body, headers={"Content-Type": ctype})
    assert r.status_code == 415
    assert no_press(swarm, node)


def test_control_accepts_json_with_charset(swarm, node):
    r = node.post("/control", data='{"target":"local","action":"power"}',
                  headers={"Content-Type": "application/json; charset=utf-8"})
    assert r.status_code == 200
    assert swarm.wait_for(lambda: node.pulses(POWER_BUTTON_PIN), 3000)


@pytest.mark.parametrize("origin", ["http://evil.example", "null", "http://127.0.10.1.evil.example", "https://evil.example"])
def test_control_rejects_foreign_origin(swarm, node, origin):
    r = node.post("/control", json={"target": "local", "action": "power"}, headers={"Origin": origin})
    assert r.status_code == 403, origin
    assert no_press(swarm, node)


def test_control_accepts_own_origin(swarm, node):
    r = node.post("/control", json={"target": "local", "action": "power"},
                  headers={"Origin": f"http://{node.ip}:{80 + swarm.port_offset}"})
    assert r.status_code == 200


def test_dns_rebinding_host_is_rejected(swarm, node):
    """Fremde Domain zeigt per DNS-Rebinding auf den ESP: Host und Origin lauten beide evil."""
    for path, method in [("/control", "POST"), ("/api/status", "GET"), ("/api/nonce", "GET"), ("/api/diag", "GET"), ("/", "GET")]:
        r = node.request(method, path, headers={"Host": "evil.example", "Origin": "http://evil.example"},
                         json={"target": "local", "action": "power"} if method == "POST" else None)
        assert r.status_code in (403, 421), (path, r.status_code)
    assert no_press(swarm, node)


def test_get_on_control_does_nothing(swarm, node):
    assert node.get("/control").status_code in (404, 405)
    assert no_press(swarm, node)


def test_cross_site_text_plain_post_on_node_endpoint_is_rejected(swarm, node):
    r = node.post("/api/control", auth=None, data='{"action":"power"}',
                  headers={"Content-Type": "text/plain", "Origin": "http://evil.example"})
    assert r.status_code in (403, 415)
    assert no_press(swarm, node)


def test_single_poweron_not_triggerable_via_get(swarm):
    """GET /poweron (z. B. <img src>) darf nichts ausloesen; nur POST mit JSON."""
    swarm.set_server("refused")
    n = swarm.add("Solo", sketch="Serverwatch", power=False)
    swarm.wait_ready(n)
    assert n.get("/poweron", timeout_ms=5000).status_code in (404, 405)
    assert n.post("/poweron", data="x", headers={"Content-Type": "text/plain"}).status_code == 415
    assert n.post("/poweron", json={}, headers={"Origin": "http://evil.example"}).status_code == 403
    assert no_press(swarm, n)
    assert n.post("/poweron", json={}).status_code == 200
    assert swarm.wait_for(lambda: n.pulses(POWER_BUTTON_PIN), 3000)


# ============================================================== Eingabepruefung (#13)
@pytest.mark.parametrize("body", ["{kaputt", "[]", '"text"', '{"target":"local","action":5}',
                                  '{"target":"local","action":["power"]}', '{"target":["local"],"action":"power"}',
                                  '{"target":"local","action":"power","x":"' + "A" * 10000 + '"}'])
def test_control_rejects_malformed_input(swarm, node, body):
    r = node.post("/control", data=body, headers={"Content-Type": "application/json"})
    assert r.status_code in (400, 413), body[:40]
    assert no_press(swarm, node)
    assert node.get("/api/status").ok  # kein Absturz


def test_dashboard_control_unknown_action_reports_error(swarm, node):
    r = node.post("/control", json={"target": "local", "action": "reboot-now"})
    assert r.status_code == 400 and r.json()["success"] is False


def test_power_press_refused_while_server_online(swarm):
    """Regression #11: 'power' (Starten) bei laufendem Server wuerde ihn herunterfahren."""
    n = swarm.add("Alpha", power=True)
    swarm.set_server("up")
    swarm.wait_ready(n)
    swarm.sleep(4000)
    assert n.status()["local"]["serverOnline"] is True
    r = n.post("/control", json={"target": "local", "action": "power"})
    assert r.status_code == 409 or r.json().get("success") is False
    assert no_press(swarm, n)


def test_single_poweron_refused_while_server_online(swarm):
    """Regression #11 fuer die Einzel-Version."""
    n = swarm.add("Solo", sketch="Serverwatch", power=True)
    swarm.set_server("up")
    swarm.wait_ready(n)
    assert swarm.wait_for(lambda: n.get("/status").json()["online"], 8000)
    assert n.post("/poweron", json={}).status_code == 409
    assert no_press(swarm, n)


# ============================================================== Schwarm-Signaturen (#10)
def test_node_control_requires_signature(swarm, node):
    for body in [{"action": "power"}, {"action": "power", "nonce": "0" * 32}, {"action": "power", "sig": "0" * 64}]:
        r = node.post("/api/control", auth=None, json=body)
        assert r.status_code in (401, 403), body
    assert no_press(swarm, node)


def test_node_control_with_valid_signature(swarm, node):
    assert node.signed_control("power").status_code == 200
    assert swarm.wait_for(lambda: node.pulses(POWER_BUTTON_PIN), 3000)


def test_node_control_wrong_key_is_rejected(swarm, node):
    assert node.signed_control("power", key="falscher-schluessel-123456").status_code == 403
    assert no_press(swarm, node)


def test_node_control_nonce_is_single_use(swarm, node):
    nonce = node.get("/api/nonce", auth=None).json()["nonce"]
    assert node.signed_control("power", key="falsch-falsch-falsch-1", nonce=nonce).status_code == 403
    # Nonce ist auch nach einem Fehlversuch verbraucht
    assert node.signed_control("power", nonce=nonce).status_code == 403
    assert no_press(swarm, node)
    nonce = node.get("/api/nonce", auth=None).json()["nonce"]
    assert node.signed_control("power", nonce=nonce).status_code == 200
    swarm.sleep(2000)
    assert node.signed_control("power", nonce=nonce).status_code == 403  # Replay


def test_node_control_unknown_or_expired_nonce(swarm, node):
    assert node.signed_control("power", nonce=secrets.token_hex(16)).status_code == 403
    nonce = node.get("/api/nonce", auth=None).json()["nonce"]
    swarm.sleep(31000)
    assert node.signed_control("power", nonce=nonce).status_code == 403
    assert no_press(swarm, node)


def test_node_control_nonce_flood_evicts_old_nonces(swarm, node):
    first = node.get("/api/nonce", auth=None).json()["nonce"]
    for _ in range(16):
        node.get("/api/nonce", auth=None)
    assert node.signed_control("power", nonce=first).status_code == 403


def test_node_control_signature_is_bound_to_action_and_target(swarm, node):
    nonce = node.get("/api/nonce", auth=None).json()["nonce"]
    sig = hmac_hex(SWARM_KEY, f"ctl|{node.node_id()}|{nonce}|reset")
    r = node.post("/api/control", auth=None, json={"action": "power", "nonce": nonce, "sig": sig})
    assert r.status_code == 403  # Signatur fuer 'reset', gesendet mit 'power'
    assert node.signed_control("power", target_id="0a0b0c0d0e0f").status_code == 403  # fuer anderen Knoten
    assert no_press(swarm, node)


@pytest.mark.parametrize("nonce", ["", "zz" * 16, "0" * 31, "0" * 33, "A" * 32, "0" * 16 + ":" + "0" * 15])
def test_node_control_rejects_malformed_nonce(swarm, node, nonce):
    assert node.signed_control("power", nonce=nonce).status_code in (400, 403)


def test_unknown_action_is_rejected(swarm, node):
    assert node.signed_control("selfdestruct").status_code == 400


def test_malformed_json_is_rejected(swarm, node):
    r = node.post("/api/control", auth=None, data="{kaputt", headers={"Content-Type": "application/json"})
    assert r.status_code == 400


def test_localstatus_is_signed(swarm, node):
    import json
    c = secrets.token_hex(16)
    r = node.get("/api/localstatus?c=" + c, auth=None).json()
    assert r["sig"] == hmac_hex(SWARM_KEY, f"st|{c}|{r['payload']}")
    payload = json.loads(r["payload"])
    assert payload["id"] == node.node_id() and payload["ip"] == node.ip


@pytest.mark.parametrize("c", [None, "", "0" * 31, "0" * 33, "g" * 32, "0" * 30 + ":a", "0" * 30 + '"a'])
def test_localstatus_rejects_bad_challenge(node, c):
    path = "/api/localstatus" if c is None else "/api/localstatus?c=" + requests.utils.quote(c)
    assert node.get(path, auth=None).status_code == 400


def _rogue_payload(**fields):
    import json
    base = {"id": "0a0b0c0d0e0f", "hostname": "serverwatch-evil-0d0e0f", "ip": "", "serverName": "Evil",
            "serverIP": "10.0.0.1", "serverPort": 80, "serverOnline": False, "serverPower": False, "powerSense": True,
            "pingTime": 0, "hasReset": True, "version": "1.0", "uptime": 1, "rssi": -50}
    base.update(fields)
    return json.dumps(base)


@pytest.mark.parametrize("variant", ["unsigned", "wrong_key", "plain_status"])
def test_fake_node_is_not_accepted(swarm, variant):
    a = swarm.add("Alpha")
    swarm.wait_ready(a)
    payload = _rogue_payload()
    body = {"unsigned": {"payload": payload, "sig": ""},
            "wrong_key": {"payload": payload, "sig": hmac_hex("anderer-schluessel-1234", "st|x|" + payload)},
            "plain_status": {"id": "0a0b0c0d0e0f", "serverName": "Evil"}}[variant]
    evil = swarm.add_rogue("serverwatch-evil", {"/api/localstatus": (200, body)})
    swarm.sleep(DISCOVERY_MS)
    assert remote_entry(a.status(), evil) == (None, None)
    assert any(m == "GET" and p.startswith("/api/localstatus") for m, p, _, _ in evil.requests), "Rogue wurde nie gefragt"


def test_relayed_status_of_real_node_is_not_accepted(swarm):
    """Ein Fake-Knoten ohne Schluessel reicht die Challenge an einen echten Knoten weiter
    (Relay). Die signierte Antwort nennt die IP des echten Knotens und wird verworfen."""
    a = swarm.add("Alpha")
    swarm.sleep(5000)
    b = swarm.add("Beta")
    swarm.wait_ready(a, b)
    evil = swarm.add_rogue("serverwatch-relay", {})

    class Relay(dict):
        def get(self, path, default=None):
            last = evil.requests[-1][1] if evil.requests else ""
            if path == "/api/localstatus" and "c=" in last:
                c = last.split("c=", 1)[1]
                return 200, b.get("/api/localstatus?c=" + c, auth=None).json()
            return default

    evil.routes = Relay()
    swarm.sleep(DISCOVERY_MS)
    entries = [e for e in a.status().values() if not e.get("isLocal")]
    assert all(e["espIP"] != evil.ip for e in entries), entries
    assert remote_entry(a.status(), b)[1], "echter Knoten Beta fehlt"


def test_swarm_disabled_without_key(swarm):
    """Ohne eigenen SWARM_KEY (secrets.h) ist der Schwarm aus (fail closed)."""
    a = swarm.add("Alpha", cfg={"swarmKey": None})
    swarm.sleep(5000)
    b = swarm.add("Beta")
    swarm.wait_ready(a, b)
    swarm.sleep(DISCOVERY_MS)
    full = a.get("/api/status").json()
    assert full["swarmEnabled"] is False
    assert list(full["servers"]) == ["local"]
    assert a.get("/api/nonce", auth=None).status_code == 403
    assert remote_entry(b.status(), a) == (None, None)


# ============================================================== WebSocket (#8, #18)
@pytest.mark.parametrize("path", ["/", "/?t=", "/?t=0123456789abcdef0123456789abcdef", "/?x=TOKEN", "/?t=TOKENx", "/?tt=TOKEN"])
def test_websocket_requires_valid_token(swarm, node, path):
    path = path.replace("TOKEN", node.ws_token())
    try:
        with node.websocket(path=path) as ws:
            msgs = ws_messages(ws, swarm.real_s(4000))
    except Exception:
        msgs = []
    assert msgs == [], "Ohne gueltigen Token duerfen keine Daten kommen"


def test_websocket_with_valid_token_gets_updates(swarm, node):
    with node.websocket() as ws:
        assert ws_messages(ws, swarm.real_s(4000))


@pytest.mark.parametrize("origin", ["http://evil.example", "null"])
def test_websocket_rejects_foreign_origin(swarm, node, origin):
    """Cross-Site WebSocket Hijacking."""
    from websockets.exceptions import InvalidStatus
    with pytest.raises((InvalidStatus, ConnectionError, OSError)):
        with node.websocket(additional_headers={"Origin": origin}) as ws:
            ws.recv(timeout=swarm.real_s(3000))


def test_websocket_accepts_own_origin(swarm, node):
    with node.websocket(additional_headers={"Origin": f"http://{node.ip}"}) as ws:
        assert ws_messages(ws, swarm.real_s(4000))
