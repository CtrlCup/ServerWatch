"""ServerTemp (Lueftersteuerung): Kennlinie, Rampe, Modi, Failsafe, NVS, /api/curve, Sicherheit.

Test-First: Spezifikation siehe Plan "Firmware ServerTemp" und "Sicherheitsvorgaben".
Alle Zeiten sind simulierte Millisekunden (swarm.sleep); Toleranzen sind absichtlich grosszuegig (Sim-Jitter).
Die Tests laufen ueber die Fixture `sketch`; fuer ServerTemp_Multi spaeter nur SKETCHES erweitern.
"""
import os
import re
import struct

import pytest

from simnet import BIN_DIR, WEB_PASSWORD, expected_hostname

SKETCHES = ["ServerTemp"]  # spaeter: + "ServerTemp_Multi"
PWM_PIN = 5
NS = "servertemp"
DEFAULT_NAME = "Rack-Lüfter"
DEFAULT_CURVE = [[25, 30], [35, 45], [45, 70], [55, 100]]
CUSTOM_CURVE = [[30, 0], [40, 50], [50, 100]]
BLOB = 0x42
U8 = 1


@pytest.fixture(params=SKETCHES)
def sketch(request):
    return request.param


# ------------------------------------------------------------------ Hilfen
def make(swarm, sketch, temp="25", cfg=None, rpm=None, start=True, pre=None):
    """Startet einen ServerTemp-Knoten (Login aus). pre(node) laeuft vor dem Start (z. B. NVS praeparieren)."""
    assert os.path.exists(os.path.join(BIN_DIR, sketch)), \
        f"Sim-Binary '{sketch}' fehlt: Firmware ServerTemp/.../{sketch}.ino ist noch nicht implementiert"
    c = {"useLogin": "false"}
    c.update(cfg or {})
    n = swarm.add("Fan", sketch=sketch, cfg=c, start=False)
    n.set_temp(temp)
    n.set_fan_rpm(rpm)
    if pre:
        pre(n)
    if start:
        n.start()
        swarm.wait_ready(n)
    return n


def restart(swarm, n):
    n.kill()
    n.start()
    swarm.wait_ready(n)


def S(n):
    return n.status()["local"]


def ctl(n, action, target="local", **kw):
    return n.post("/control", json={"target": target, "action": action}, **kw)


def ok(r):
    assert r.status_code == 200, (r.status_code, r.text)
    assert r.json() == {"success": True}


def duty_pct(d):
    return None if d is None else d * 100 / 255


def pct(n):
    return duty_pct(n.pwm(PWM_PIN))


def ledc(n):
    """Alle ledc-Ereignisse des Lueferpins: [(t_ms, duty)]."""
    out = []
    for t, e in n.events():
        p = e.split()
        if len(p) == 3 and p[0] == "ledc" and p[1] == str(PWM_PIN):
            out.append((t, int(p[2])))
    return out


def wait_pct(swarm, n, target, tol=3, timeout_ms=150000):
    v = swarm.wait_for(lambda: pct(n) is not None and abs(pct(n) - target) <= tol, timeout_ms, poll_ms=500)
    assert v, f"pct erreichte {target} nicht, zuletzt {pct(n)}\n{n.serial()[-1500:]}"


def wait_status(swarm, n, cond, timeout_ms, desc="Status"):
    v = swarm.wait_for(lambda: cond(S(n)), timeout_ms, poll_ms=500)
    assert v, f"{desc} nicht erreicht, zuletzt {S(n)}"


def ramp_ok(events, up=10.0, down=2.0, tol=5.0):
    """Pruefe alle Paare i<j: Anstieg <= up %/s, Abfall <= down %/s (plus Toleranz)."""
    pts = [(t, duty_pct(d)) for t, d in events]
    for i in range(len(pts)):
        for j in range(i + 1, len(pts)):
            dt, dp = (pts[j][0] - pts[i][0]) / 1000.0, pts[j][1] - pts[i][1]
            if dp > up * dt + tol:
                return False, f"Anstieg {dp:.1f}% in {dt:.2f}s ({pts[i]} -> {pts[j]})"
            if -dp > down * dt + tol:
                return False, f"Abfall {-dp:.1f}% in {dt:.2f}s ({pts[i]} -> {pts[j]})"
    return True, ""


def curve_pct(temp, curve=DEFAULT_CURVE):
    """Soll-Prozent der Kennlinie bei temp (lineare Interpolation, ausserhalb geklemmt)."""
    if temp <= curve[0][0]:
        return curve[0][1]
    for (t0, p0), (t1, p1) in zip(curve, curve[1:]):
        if temp <= t1:
            return p0 + (p1 - p0) * (temp - t0) / (t1 - t0)
    return curve[-1][1]


def wait_quiet(swarm, n, quiet_ms=3000, max_rounds=100):
    """Wartet, bis der Luefterausgang stabil ist (kein neues ledc-Ereignis mehr in quiet_ms); Rampe fertig."""
    for _ in range(max_rounds):
        c = len(ledc(n))
        swarm.sleep(quiet_ms)
        if len(ledc(n)) == c:
            return
    raise AssertionError("Ausgang wird nicht ruhig (Rampe laeuft weiter)")


def blob(points):
    return bytes([1, len(points)]) + b"".join(struct.pack("bB", t, p) for t, p in points)


def post_curve(n, points, **kw):
    return n.post("/api/curve", json={"points": points}, **kw)


def settle(swarm, n, mode):
    """Bringt den Knoten (Temp 25 C) in einen niedrigen, ruhigen Zustand je Modus."""
    if mode == "auto":
        wait_pct(swarm, n, 30)
    elif mode == "manual":
        ok(ctl(n, "fan:speed:30"))
        wait_pct(swarm, n, 30)
    elif mode == "off":
        ok(ctl(n, "fan:off"))
        wait_pct(swarm, n, 0, tol=0.5)
    elif mode == "curve0":
        assert post_curve(n, [[20, 0], [100, 0]]).status_code == 200
        wait_pct(swarm, n, 0, tol=0.5)


# ================================================================== Status / Konfiguration / Hostname
def test_status_schema(swarm, sketch):
    n = make(swarm, sketch, temp="23.5")
    wait_status(swarm, n, lambda s: s["temp"] is not None, 15000, "erste Messung")
    j = n.get("/api/status").json()
    assert set(j) == {"servers", "defaultCredentials"}  # Single: kein swarmEnabled
    assert isinstance(j["defaultCredentials"], bool)
    assert list(j["servers"]) == ["local"]
    s = j["servers"]["local"]
    for k in ("isLocal", "type", "id", "hostname", "serverName", "temp", "sensorOk", "rpm", "pct", "mode", "fanOn",
              "failsafe", "stall", "fanType", "hasRpm", "version", "espIP", "espReachable"):
        assert k in s, k
    assert s["isLocal"] is True and s["type"] == "fan"
    assert isinstance(s["id"], str) and s["id"]
    assert s["hostname"] == expected_hostname(DEFAULT_NAME, n.ip, "servertemp-")
    assert s["serverName"] == DEFAULT_NAME
    assert s["temp"] == 23.5 and round(s["temp"], 1) == s["temp"]
    assert s["sensorOk"] is True
    assert isinstance(s["pct"], int) and 0 <= s["pct"] <= 100
    assert s["mode"] == "auto" and s["failsafe"] == "" and s["stall"] is False
    assert s["fanType"] == "pwm4" and s["hasRpm"] is True
    assert isinstance(s["fanOn"], bool)
    assert s["version"] == "1.0.0" and s["espIP"] == n.ip and s["espReachable"] is True


def test_dashboard_page_served(swarm, sketch):
    n = make(swarm, sketch)
    r = n.get("/", auth=None)
    assert r.status_code == 200 and "<html" in r.text.lower()


def test_wstoken_absent_on_single(swarm, sketch):
    if sketch != "ServerTemp":
        pytest.skip("nur Single")
    n = make(swarm, sketch)
    assert n.get("/api/wstoken", auth=None).status_code == 404


@pytest.mark.parametrize("name", [DEFAULT_NAME, "Ä Ö/ü ß!", "A" * 40, "<script>alert(1)</script>", "  ", "--x--"])
def test_hostname_sanitized(swarm, sketch, name):
    n = make(swarm, sketch, cfg={"deviceName": name})
    h = expected_hostname(name, n.ip, "servertemp-")
    assert h.startswith("servertemp-") and "<" not in h
    assert f"Hostname: {h}" in n.serial()
    assert S(n)["hostname"] == h
    assert n.get("/api/status", headers={"Host": h}).status_code == 200


def test_device_name_xss_only_in_json_string(swarm, sketch):
    name = "<script>alert(1)</script>"
    n = make(swarm, sketch, cfg={"deviceName": name})
    r = n.get("/api/status")
    assert r.headers["Content-Type"].startswith("application/json")
    j = r.json()  # gueltiges JSON, Name nur als Wert
    s = j["servers"]["local"]
    assert s["serverName"] == name  # exakt als String-Wert, kein Markup-Kontext
    assert re.fullmatch(r"[a-z0-9-]+", s["hostname"]), s["hostname"]
    page = n.get("/", auth=None).text
    assert "alert(1)" not in page


# ================================================================== Kennlinie
def test_curve_default_get(swarm, sketch):
    n = make(swarm, sketch)
    r = n.get("/api/curve")
    assert r.status_code == 200
    assert r.json() == {"points": DEFAULT_CURVE, "emergencyTemp": 60, "minPct": 20}


@pytest.mark.parametrize("seq", [
    [58, 50, 40, 35, 30, 20],   # fallend
    [20, 30, 35, 40, 50, 58],   # steigend
], ids=["fallend", "steigend"])
def test_curve_points_below_on_between_above(swarm, sketch, seq):
    n = make(swarm, sketch, temp=str(seq[0]))
    for t in seq:
        n.set_temp(t)
        wait_pct(swarm, n, curve_pct(t), tol=2.5, timeout_ms=200000)
    # keine Failsafe-Zustaende auf dem Weg
    assert S(n)["failsafe"] == ""


def test_curve_custom_applied_and_interpolated(swarm, sketch):
    n = make(swarm, sketch, temp="45")
    assert post_curve(n, CUSTOM_CURVE).status_code == 200
    wait_pct(swarm, n, 75, tol=2.5, timeout_ms=100000)  # 45 -> 50 % zwischen (40,50),(50,100)? = 75


def test_ramp_down_at_most_2_pct_per_s_auto(swarm, sketch):
    """Boot 100 % -> Kennlinie 30 %: langsamer Abfall."""
    n = make(swarm, sketch, temp="25")
    wait_pct(swarm, n, 30, tol=2, timeout_ms=150000)
    ev = ledc(n)
    assert ev[0][1] == 255
    good, why = ramp_ok(ev)
    assert good, why
    reach = next(t for t, d in ev if duty_pct(d) <= 32)
    assert reach - ev[0][0] >= 25000, "Abfall zu schnell (erwartet ~35 s fuer 100 -> 30 %)"


def test_ramp_up_at_most_10_pct_per_s_auto(swarm, sketch):
    n = make(swarm, sketch, temp="20")
    wait_pct(swarm, n, 30, tol=2, timeout_ms=150000)
    base = len(ledc(n))
    t0 = n.events()[-1][0]
    n.set_temp(58)  # < Notlauf, Kennlinie 100 %
    wait_pct(swarm, n, 100, tol=1, timeout_ms=60000)
    ev = ledc(n)[base - 1:]
    good, why = ramp_ok(ev)
    assert good, why
    t_full = next(t for t, d in ev if d == 255)
    assert t_full - t0 >= 4000, "Anstieg ohne Rampe"
    assert S(n)["failsafe"] == ""


def test_ramp_applies_in_manual(swarm, sketch):
    n = make(swarm, sketch, temp="20")
    wait_pct(swarm, n, 30, tol=2, timeout_ms=150000)
    base = len(ledc(n))
    ok(ctl(n, "fan:speed:90"))
    wait_pct(swarm, n, 90, tol=2, timeout_ms=60000)
    ok(ctl(n, "fan:speed:70"))
    wait_pct(swarm, n, 70, tol=2, timeout_ms=60000)
    good, why = ramp_ok(ledc(n)[base - 1:])
    assert good, why


def test_turn_off_and_hysteresis(swarm, sketch):
    """Kennlinie 0 % -> aus; wieder an erst bei >= (Temp beim Ausschalten + 2 K)."""
    n = make(swarm, sketch, temp="30.2")
    assert post_curve(n, CUSTOM_CURVE).status_code == 200
    wait_pct(swarm, n, 1, tol=2, timeout_ms=150000)  # 30,2 C -> ca. 1 %
    n.set_temp(29)  # EMA kreuzt 30 bei ca. 29,8 -> Ausschalttemperatur in (29, 30]
    wait_pct(swarm, n, 0, tol=0.5, timeout_ms=60000)
    n.set_temp(30.9)  # Kennlinie > 0, aber noch unter Ausschalttemperatur + 2 K (>= 31)
    for _ in range(10):
        swarm.sleep(2500)
        assert n.pwm(PWM_PIN) == 0, "Hysterese verletzt: Luefter zu frueh wieder an"
    n.set_temp(33)
    assert swarm.wait_for(lambda: (n.pwm(PWM_PIN) or 0) > 0, 40000), "Luefter blieb trotz Temp > Schwelle aus"


# ================================================================== Modi / Aktionen
def test_mode_actions(swarm, sketch):
    n = make(swarm, sketch, temp="45")  # auto -> 70 %
    wait_pct(swarm, n, 70, tol=2.5, timeout_ms=100000)
    ok(ctl(n, "fan:manual"))  # friert aktuellen pct ein
    assert S(n)["mode"] == "manual"
    n.set_temp(25)
    swarm.sleep(25000)
    wait_pct(swarm, n, 70, tol=3, timeout_ms=5000)
    ok(ctl(n, "fan:speed:40"))
    assert S(n)["mode"] == "manual"
    wait_pct(swarm, n, 40, tol=2.5, timeout_ms=60000)
    ok(ctl(n, "fan:off"))
    assert S(n)["mode"] == "off"
    wait_pct(swarm, n, 0, tol=0.5, timeout_ms=100000)
    assert S(n)["pct"] == 0
    ok(ctl(n, "fan:auto"))
    assert S(n)["mode"] == "auto"
    wait_pct(swarm, n, 30, tol=3, timeout_ms=100000)


def test_speed_zero_and_hundred(swarm, sketch):
    n = make(swarm, sketch)
    ok(ctl(n, "fan:speed:100"))
    wait_pct(swarm, n, 100, tol=1, timeout_ms=60000)
    ok(ctl(n, "fan:speed:0"))
    assert S(n)["mode"] == "manual"
    wait_pct(swarm, n, 0, tol=0.5, timeout_ms=100000)


@pytest.mark.parametrize("action", [
    "fan:speed:", "fan:speed:101", "fan:speed:-1", "fan:speed:+5", "fan:speed:050", "fan:speed:00", "fan:speed:5x",
    "fan:speed: 5", "fan:speed:5 ", "fan:speed:5:1", "fan:speed:1000", "fan:speed:5.5", "FAN:off", "Fan:auto",
    "fan:OFF", "fan:", "fan", "", "fan:speed", "fan:auto ", " fan:auto", "fan:speed:" + "5" * 290, "x" * 300,
    "fan:speed:0x10", "fan:speed:\t5",
])
def test_action_parser_rejects(swarm, sketch, action):
    n = make(swarm, sketch)
    r = ctl(n, action)
    assert r.status_code == 400, (action, r.status_code, r.text)
    j = r.json()
    assert j["success"] is False and isinstance(j.get("error"), str) and j["error"]
    s = S(n)
    assert s["mode"] == "auto"
    assert n.nvs_writes() == 0


def test_action_parser_16_char_limit_boundary(swarm, sketch):
    n = make(swarm, sketch)
    assert len("fan:speed:100") <= 16
    ok(ctl(n, "fan:speed:100"))
    assert ctl(n, "fan:speed:" + "0" * 7).status_code == 400  # 17 Zeichen und fuehrende Nullen


@pytest.mark.parametrize("action", ["power", "reset", "shutdown", "poweroff", "fan:speed:5;power"])
def test_server_actions_rejected(swarm, sketch, action):
    n = make(swarm, sketch)
    r = ctl(n, action)
    assert r.status_code == 400
    assert r.json()["success"] is False


@pytest.mark.parametrize("target", ["other", "remote-1", "LOCAL", "abcdef0123456789"])
def test_control_unknown_target_404(swarm, sketch, target):
    n = make(swarm, sketch)
    assert ctl(n, "fan:auto", target=target).status_code == 404
    assert S(n)["mode"] == "auto"


# ================================================================== dc3
def test_dc3_below_min_is_zero(swarm, sketch):
    n = make(swarm, sketch, cfg={"fanType": "dc3"})
    ok(ctl(n, "fan:speed:10"))
    wait_pct(swarm, n, 0, tol=0.5, timeout_ms=150000)
    i = len(ledc(n))
    swarm.sleep(15000)
    assert all(d == 0 for _, d in ledc(n)[i:])
    for d in {d for _, d in ledc(n)}:
        assert d == 0 or d >= round(20 * 255 / 100), f"Duty {d} unter fanMinPct"
    ok(ctl(n, "fan:speed:19"))
    swarm.sleep(15000)
    assert n.pwm(PWM_PIN) == 0


def test_dc3_kickstart_from_zero(swarm, sketch):
    n = make(swarm, sketch, cfg={"fanType": "dc3"})
    ok(ctl(n, "fan:speed:0"))
    wait_pct(swarm, n, 0, tol=0.5, timeout_ms=150000)
    base = len(ledc(n))
    ok(ctl(n, "fan:speed:50"))
    swarm.wait_for(lambda: len(ledc(n)) > base and ledc(n)[-1][1] != 0, 20000)
    ev = ledc(n)[base:]
    nonzero = [(t, d) for t, d in ev if d != 0]
    assert nonzero and nonzero[0][1] == 255, f"kein Kickstart mit 100 %: {ev[:5]}"
    t_kick = nonzero[0][0]
    swarm.wait_for(lambda: any(d != 255 and d != 0 for _, d in ledc(n)[base:]), 20000)
    after = next(t for t, d in ledc(n)[base:] if t > t_kick and d != 255)
    assert 800 <= after - t_kick <= 3000, f"Kickstartdauer {after - t_kick} ms"
    wait_pct(swarm, n, 50, tol=3, timeout_ms=150000)


def test_dc3_rpm_null_below_100_valid_at_100(swarm, sketch):
    n = make(swarm, sketch, cfg={"fanType": "dc3"}, rpm="2400")
    s = S(n)
    assert s["fanType"] == "dc3"
    ok(ctl(n, "fan:speed:50"))
    wait_pct(swarm, n, 50, tol=3, timeout_ms=150000)
    s = S(n)
    assert s["rpm"] is None and s["hasRpm"] is False
    ok(ctl(n, "fan:speed:100"))
    wait_pct(swarm, n, 100, tol=1, timeout_ms=60000)
    wait_status(swarm, n, lambda s: s["rpm"] is not None and abs(s["rpm"] - 2400) <= 400 and s["hasRpm"] is True,
                20000, "rpm bei 100 %")


def test_dc3_never_stalls(swarm, sketch):
    n = make(swarm, sketch, cfg={"fanType": "dc3"}, rpm="0")
    ok(ctl(n, "fan:speed:100"))
    wait_pct(swarm, n, 100, tol=1, timeout_ms=60000)
    swarm.sleep(15000)
    assert S(n)["stall"] is False


# ================================================================== Tacho / stall / fanOn (pwm4)
def test_pwm4_rpm_from_tach(swarm, sketch):
    n = make(swarm, sketch, rpm="1800")
    ok(ctl(n, "fan:speed:50"))
    wait_status(swarm, n, lambda s: s["rpm"] is not None and abs(s["rpm"] - 1800) <= 200, 30000, "rpm 1800")
    assert S(n)["hasRpm"] is True
    n.set_fan_rpm(600)
    wait_status(swarm, n, lambda s: abs(s["rpm"] - 600) <= 100, 30000, "rpm 600")
    n.set_fan_rpm(0)
    wait_status(swarm, n, lambda s: s["rpm"] == 0, 30000, "rpm 0")


def test_stall_after_5s_at_30_pct_or_more(swarm, sketch):
    n = make(swarm, sketch, rpm="1500")
    ok(ctl(n, "fan:speed:50"))
    wait_pct(swarm, n, 50, tol=3, timeout_ms=100000)
    wait_status(swarm, n, lambda s: s["rpm"] and s["rpm"] > 1000, 20000)
    assert S(n)["stall"] is False
    n.set_fan_rpm(0)
    assert S(n)["stall"] is False  # sofort: noch keine 5 s
    swarm.sleep(1000)
    assert S(n)["stall"] is False  # Reserve gegen Last-Jitter (Stillstand erst nach 5 s simulierter Zeit)
    wait_status(swarm, n, lambda s: s["stall"] is True, 20000, "stall")
    n.set_fan_rpm(1500)
    wait_status(swarm, n, lambda s: s["stall"] is False, 20000, "stall endet")


def test_no_stall_below_30_pct(swarm, sketch):
    n = make(swarm, sketch, rpm="0")
    ok(ctl(n, "fan:speed:20"))
    wait_pct(swarm, n, 20, tol=3, timeout_ms=100000)
    swarm.sleep(15000)
    assert S(n)["stall"] is False


def test_fan_on_via_rpm_when_pwm_zero(swarm, sketch):
    n = make(swarm, sketch, rpm="0")
    ok(ctl(n, "fan:off"))
    wait_pct(swarm, n, 0, tol=0.5, timeout_ms=150000)
    wait_status(swarm, n, lambda s: s["fanOn"] is False and s["pct"] == 0, 10000, "fanOn false")
    n.set_fan_rpm(900)  # Luefter dreht trotz 0 % (viele 4-Pin-Luefter stoppen nicht)
    wait_status(swarm, n, lambda s: s["pct"] == 0 and s["fanOn"] is True, 20000, "fanOn ueber rpm")


def test_fan_on_with_pct(swarm, sketch):
    n = make(swarm, sketch, rpm="0")
    wait_status(swarm, n, lambda s: s["pct"] > 0 and s["fanOn"] is True, 10000)


# ================================================================== Failsafe
@pytest.mark.parametrize("mode", ["auto", "manual", "off"])
def test_sensor_failure(swarm, sketch, mode):
    n = make(swarm, sketch, temp="25")
    settle(swarm, n, mode)
    n.set_temp("disconnected")
    swarm.sleep(2500)
    assert S(n)["sensorOk"] is True, "sensorOk erst nach 3 ungueltigen Messungen in Folge"
    wait_status(swarm, n, lambda s: s["sensorOk"] is False, 15000, "sensorOk false")
    s = S(n)
    assert s["failsafe"] == "sensor" and s["mode"] == mode
    if mode == "off":
        swarm.sleep(4000)
        assert n.pwm(PWM_PIN) == 0 and S(n)["pct"] == 0 and S(n)["failsafe"] == "sensor"
    else:
        assert swarm.wait_for(lambda: n.pwm(PWM_PIN) == 255, 10000), "kein sofortiger 100 %-Failsafe"
        assert S(n)["pct"] == 100


def test_sensor_failure_failsafe_without_ramp(swarm, sketch):
    n = make(swarm, sketch, temp="25")
    wait_pct(swarm, n, 30)
    wait_quiet(swarm, n)
    base = len(ledc(n))
    n.set_temp("disconnected")
    wait_status(swarm, n, lambda s: s["sensorOk"] is False, 30000)
    ev = ledc(n)[base:]
    assert ev and ev[0][1] == 255, f"Sprung auf 100 % erwartet: {ev[:4]}"


def test_sensor_recovers_after_rescan(swarm, sketch):
    n = make(swarm, sketch, temp="25")
    wait_pct(swarm, n, 30)
    n.set_temp("disconnected")
    wait_status(swarm, n, lambda s: s["sensorOk"] is False, 15000)
    n.set_temp(30)
    wait_status(swarm, n, lambda s: s["sensorOk"] is True and s["failsafe"] == "", 60000, "Sensor erholt")
    wait_pct(swarm, n, curve_pct(30), tol=3, timeout_ms=150000)  # Rampe zurueck auf Kennlinie


def test_sensor_missing_at_boot(swarm, sketch):
    n = make(swarm, sketch, temp=None)
    assert n.pwm(PWM_PIN) in (None, 255)
    wait_status(swarm, n, lambda s: s["sensorOk"] is False and s["failsafe"] == "sensor", 20000)
    assert n.pwm(PWM_PIN) == 255
    n.set_temp(40)  # Sensor angesteckt: Bus-Neuscan alle 30 s
    wait_status(swarm, n, lambda s: s["sensorOk"] is True, 60000, "Neuscan")


@pytest.mark.parametrize("bad", ["126", "130", "-21"])
def test_out_of_range_is_invalid(swarm, sketch, bad):
    n = make(swarm, sketch, temp="25")
    wait_pct(swarm, n, 30)
    n.set_temp(bad)
    wait_status(swarm, n, lambda s: s["sensorOk"] is False, 60000)
    s = S(n)
    assert s["failsafe"] == "sensor"  # nicht "overtemp"
    assert swarm.wait_for(lambda: n.pwm(PWM_PIN) == 255, 15000, poll_ms=250)


def test_boot_runs_at_100_pct_first(swarm, sketch):
    n = make(swarm, sketch, temp="25")
    ev = ledc(n)
    assert ev and ev[0][1] == 255, f"Boot nicht mit 100 %: {ev[:3]}"


def test_boot_100_pct_until_first_valid_measurement(swarm, sketch):
    n = make(swarm, sketch, temp="disconnected")
    swarm.sleep(1500)
    assert n.pwm(PWM_PIN) == 255


@pytest.mark.parametrize("mode", ["auto", "manual", "off", "curve0"])
def test_overtemp_all_modes(swarm, sketch, mode):
    n = make(swarm, sketch, temp="25")
    settle(swarm, n, mode)
    wait_quiet(swarm, n)
    base = len(ledc(n))
    n.set_temp(61)
    assert swarm.wait_for(lambda: n.pwm(PWM_PIN) == 255, 15000, poll_ms=100), "Notlauf blieb aus"
    ev = ledc(n)[base:]
    assert ev[0][1] == 255, f"Notlauf ohne Rampe erwartet: {ev[:4]}"
    wait_status(swarm, n, lambda s: s["failsafe"] == "overtemp", 10000)
    assert S(n)["pct"] == 100
    n.set_temp(58)   # unter 60, ueber 55: Notlauf bleibt (Hysterese)
    swarm.sleep(8000)
    assert S(n)["failsafe"] == "overtemp" and n.pwm(PWM_PIN) == 255
    n.set_temp("55.0")  # Ende erst bei < 55
    swarm.sleep(8000)
    assert S(n)["failsafe"] == "overtemp" and n.pwm(PWM_PIN) == 255
    n.set_temp(54.5)
    wait_status(swarm, n, lambda s: s["failsafe"] == "", 10000, "Notlauf beendet")
    if mode in ("off", "curve0"):
        n.set_temp(25)
        wait_pct(swarm, n, 0, tol=0.5, timeout_ms=200000)
        if mode == "off":
            assert S(n)["mode"] == "off"


def test_overtemp_boundary_60(swarm, sketch):
    n = make(swarm, sketch, temp="25")
    ok(ctl(n, "fan:off"))
    wait_pct(swarm, n, 0, tol=0.5, timeout_ms=150000)
    n.set_temp(59.5)
    swarm.sleep(8000)
    assert S(n)["failsafe"] == "" and n.pwm(PWM_PIN) == 0
    n.set_temp("60.0")
    assert swarm.wait_for(lambda: n.pwm(PWM_PIN) == 255, 6000, poll_ms=100)


def test_sensor_85_valid_after_first_value(swarm, sketch):
    """85 C nach gueltigen Messungen ist ein echter Messwert: Notlauf."""
    n = make(swarm, sketch, temp="30")
    wait_status(swarm, n, lambda s: s["temp"] is not None, 15000)
    n.set_temp("85")
    wait_status(swarm, n, lambda s: s["failsafe"] == "overtemp", 8000)
    assert n.pwm(PWM_PIN) == 255


def test_overtemp_has_priority_over_sensor_error(swarm, sketch):
    n = make(swarm, sketch, temp="25")
    ok(ctl(n, "fan:off"))
    wait_pct(swarm, n, 0, tol=0.5, timeout_ms=150000)
    n.set_temp(70)
    wait_status(swarm, n, lambda s: s["failsafe"] == "overtemp", 8000)
    assert n.pwm(PWM_PIN) == 255 and S(n)["mode"] == "off"


def test_control_continues_without_wifi(swarm, sketch):
    n = make(swarm, sketch, temp="20")
    wait_pct(swarm, n, 30, tol=2, timeout_ms=150000)
    n.set_ap(False)
    swarm.sleep(3000)
    n.set_temp(58)
    assert swarm.wait_for(lambda: (pct(n) or 0) >= 98, 40000), "Regelung steht ohne WLAN"
    n.set_temp(61)
    assert swarm.wait_for(lambda: n.pwm(PWM_PIN) == 255, 20000), "Notlauf ohne WLAN blieb aus"
    n.set_ap(True)
    wait_status(swarm, n, lambda s: s["pct"] == 100 and s["failsafe"] == "overtemp", 60000, "WLAN wieder da")


# ================================================================== NVS / Persistenz
def test_curve_persisted_debounced_and_survives_restart(swarm, sketch):
    n = make(swarm, sketch)
    assert post_curve(n, CUSTOM_CURVE).status_code == 200
    swarm.sleep(15000)
    assert n.nvs_writes("curve") == 0, "Entprellung: nicht vor 30 s schreiben"
    assert swarm.wait_for(lambda: n.nvs_raw(NS, "curve") is not None, 40000, poll_ms=1000), "curve nie geschrieben"
    assert n.nvs_raw(NS, "curve") == blob(CUSTOM_CURVE)
    assert n.nvs_type(NS, "curve") == BLOB
    assert n.nvs_writes("curve") == 1
    restart(swarm, n)
    assert n.get("/api/curve").json()["points"] == CUSTOM_CURVE


def test_mode_and_manpct_persisted(swarm, sketch):
    n = make(swarm, sketch)
    ok(ctl(n, "fan:speed:55"))
    assert swarm.wait_for(lambda: n.nvs_raw(NS, "manPct") is not None and n.nvs_raw(NS, "mode") is not None,
                          60000, poll_ms=1000)
    assert n.nvs_raw(NS, "manPct") == bytes([55]) and n.nvs_type(NS, "manPct") == U8
    assert n.nvs_raw(NS, "mode") == bytes([1]) and n.nvs_type(NS, "mode") == U8
    restart(swarm, n)
    s = S(n)
    assert s["mode"] == "manual"
    wait_pct(swarm, n, 55, tol=3, timeout_ms=150000)
    n.kill()  # auch nach hartem Stromausfall
    n.start()
    swarm.wait_ready(n)
    assert S(n)["mode"] == "manual"


def test_off_is_never_saved_and_restarts_in_auto(swarm, sketch):
    n = make(swarm, sketch)
    ok(ctl(n, "fan:speed:55"))
    assert swarm.wait_for(lambda: n.nvs_raw(NS, "mode") == bytes([1]), 60000, poll_ms=1000)
    ok(ctl(n, "fan:off"))
    assert S(n)["mode"] == "off"
    # auto wird entprellt gespeichert (max. 1 Schreibvorgang/Minute und Schluessel)
    assert swarm.wait_for(lambda: n.nvs_raw(NS, "mode") == bytes([0]), 150000, poll_ms=1000), \
        f"mode=auto nicht gespeichert, NVS: {n.nvs_raw(NS, 'mode')}"
    assert n.nvs_raw(NS, "mode") != bytes([2])
    restart(swarm, n)
    assert S(n)["mode"] == "auto"
    wait_status(swarm, n, lambda s: s["pct"] > 0, 20000)


def test_off_from_fresh_state_writes_nothing_and_restarts_auto(swarm, sketch):
    n = make(swarm, sketch)
    ok(ctl(n, "fan:off"))
    swarm.sleep(70000)
    assert n.nvs_raw(NS, "mode") in (None, bytes([0]))
    restart(swarm, n)
    assert S(n)["mode"] == "auto"


def test_nvs_debounce_100_speed_changes(swarm, sketch):
    n = make(swarm, sketch)
    for i in range(100):
        r = ctl(n, f"fan:speed:{1 + (i * 7) % 99}")
        assert r.status_code == 200
        swarm.sleep(100)
    ok(ctl(n, "fan:speed:77"))
    swarm.sleep(15000)
    assert n.nvs_writes("manPct") == 0 and n.nvs_writes("mode") == 0, "Schreibvorgang waehrend der Entprellung"
    assert swarm.wait_for(lambda: n.nvs_raw(NS, "manPct") == bytes([77]), 60000, poll_ms=1000)
    swarm.sleep(70000)
    for key in ("manPct", "mode", "curve"):
        assert n.nvs_writes(key) <= 2, (key, n.nvs_writes(key))
    assert n.nvs_raw(NS, "manPct") == bytes([77])


def test_unchanged_value_not_rewritten(swarm, sketch):
    n = make(swarm, sketch)
    ok(ctl(n, "fan:speed:40"))
    assert swarm.wait_for(lambda: n.nvs_raw(NS, "manPct") == bytes([40]), 60000, poll_ms=1000)
    w = n.nvs_writes("manPct")
    ok(ctl(n, "fan:speed:41"))
    swarm.sleep(2000)
    ok(ctl(n, "fan:speed:40"))
    swarm.sleep(80000)
    assert n.nvs_writes("manPct") == w, "gleicher Wert erneut geschrieben"


def test_identical_curve_not_written(swarm, sketch):
    n = make(swarm, sketch)
    r = post_curve(n, DEFAULT_CURVE)
    assert r.status_code == 200
    swarm.sleep(50000)
    assert n.nvs_writes("curve") == 0
    assert post_curve(n, DEFAULT_CURVE).status_code == 200
    assert n.nvs_writes() == 0


def test_failsafe_states_never_saved(swarm, sketch):
    n = make(swarm, sketch)
    n.set_temp("disconnected")
    wait_status(swarm, n, lambda s: s["failsafe"] == "sensor", 20000)
    n.set_temp(30)
    wait_status(swarm, n, lambda s: s["failsafe"] == "", 60000)
    n.set_temp(70)
    wait_status(swarm, n, lambda s: s["failsafe"] == "overtemp", 8000)
    swarm.sleep(80000)
    assert n.nvs_writes() == 0, n.events()[-10:]


@pytest.mark.parametrize("data, typ", [
    (b"\x02\x02" + struct.pack("bBbB", 20, 30, 40, 80), "blob"),   # falsche Version
    (b"\x01\x01" + struct.pack("bB", 20, 30), "blob"),             # 1 Punkt
    (b"\x01\x07" + struct.pack("bB", 0, 0) * 7, "blob"),           # 7 Punkte
    (b"\x01\x03" + struct.pack("bBbB", 20, 30, 40, 80), "blob"),   # abgeschnitten
    (b"\x01\x02" + struct.pack("bBbBbB", 20, 30, 40, 80, 50, 90), "blob"),  # Laenge passt nicht
    (b"\x01\x02" + struct.pack("bBbB", 40, 30, 20, 80), "blob"),   # t fallend
    (b"\x01\x02" + struct.pack("bBbB", 20, 30, 20, 80), "blob"),   # t gleich
    (b"\x01\x02" + struct.pack("bBbB", 20, 80, 40, 30), "blob"),   # p fallend
    (b"\x01\x02" + struct.pack("bBbB", 20, 30, 40, 200), "blob"),  # p > 100
    (b"\x01\x02" + struct.pack("bBbB", -20, 30, 40, 80), "blob"),  # t < -10
    (b"\x01\x02" + struct.pack("bBbB", 20, 30, 120, 80), "blob"),  # t > 100
    (b"", "blob"),
    (b"\xff" * 40, "blob"),
    (b"\x01\x02" + struct.pack("bBbB", 20, 30, 40, 80), "u8"),     # falscher Typ
    ("kaputt", "str"),
], ids=["version", "n1", "n7", "truncated", "length", "t-desc", "t-equal", "p-desc", "p-over", "t-low", "t-high",
        "empty", "ff", "type", "string"])
def test_corrupt_curve_gives_default(swarm, sketch, data, typ):
    n = make(swarm, sketch, pre=lambda n: n.nvs_corrupt(NS, "curve", data, typ))
    assert n.get("/api/curve").json()["points"] == DEFAULT_CURVE


def test_valid_stored_curve_is_loaded(swarm, sketch):
    n = make(swarm, sketch, pre=lambda n: n.nvs_corrupt(NS, "curve", blob(CUSTOM_CURVE), "blob"))
    assert n.get("/api/curve").json()["points"] == CUSTOM_CURVE


@pytest.mark.parametrize("data, typ", [(bytes([7]), "u8"), (bytes([255]), "u8"), ("manual", "str"), (b"\x01\x00\x00\x00", "u32")])
def test_corrupt_mode_gives_auto(swarm, sketch, data, typ):
    n = make(swarm, sketch, pre=lambda n: n.nvs_corrupt(NS, "mode", data, typ))
    assert S(n)["mode"] == "auto"


def test_corrupt_manpct_never_exceeds_100(swarm, sketch):
    def pre(n):
        n.nvs_corrupt(NS, "mode", bytes([1]), "u8")
        n.nvs_corrupt(NS, "manPct", bytes([250]), "u8")
    n = make(swarm, sketch, pre=pre)
    swarm.sleep(5000)
    assert 0 <= S(n)["pct"] <= 100
    wait_status(swarm, n, lambda s: s["pct"] <= 100, 5000)
    assert n.pwm(PWM_PIN) <= 255


# ================================================================== /api/curve Validierung
VALID_CURVES = [
    [[-10, 0], [100, 100]],
    [[20, 50], [30, 50]],
    [[0, 0], [10, 10], [20, 20], [30, 30], [40, 40], [50, 50]],
    [[25, 0], [35, 100]],
]
INVALID_CURVES = [
    ("leer", []),
    ("ein Punkt", [[20, 30]]),
    ("sieben Punkte", [[i * 10, i * 10] for i in range(7)]),
    ("t gleich", [[20, 30], [20, 40]]),
    ("t fallend", [[30, 30], [20, 40]]),
    ("t zu klein", [[-11, 0], [20, 40]]),
    ("t zu gross", [[20, 0], [101, 40]]),
    ("p fallend", [[20, 50], [30, 40]]),
    ("p negativ", [[20, -1], [30, 40]]),
    ("p zu gross", [[20, 0], [30, 101]]),
    ("float t", [[20.5, 30], [30, 40]]),
    ("float p", [[20, 30.5], [30, 40]]),
    ("string t", [["20", 30], [30, 40]]),
    ("null", [[None, 30], [30, 40]]),
    ("bool", [[True, 30], [30, 40]]),
    ("Paar zu kurz", [[20], [30, 40]]),
    ("Paar zu lang", [[20, 30, 1], [30, 40]]),
    ("Paar kein Array", [20, 30]),
    ("riesige Zahl", [[20, 0], [99999999999, 40]]),
    ("Objekt statt Paar", [{"t": 1, "p": 2}, {"t": 2, "p": 3}]),
]


@pytest.mark.parametrize("points", VALID_CURVES)
def test_curve_post_valid(swarm, sketch, points):
    n = make(swarm, sketch)
    r = post_curve(n, points)
    assert r.status_code == 200, r.text
    assert n.get("/api/curve").json()["points"] == points
    assert n.get("/api/curve").json()["emergencyTemp"] == 60


def test_curve_post_invalid_all_rejected_nothing_written(swarm, sketch):
    n = make(swarm, sketch)
    for name, points in INVALID_CURVES:
        r = post_curve(n, points)
        assert r.status_code == 400, (name, r.status_code, r.text)
        j = r.json()
        assert j.get("success") is False and isinstance(j.get("error"), str) and j["error"], name
    for name, body in [("points fehlt", {}), ("points kein Array", {"points": "x"}), ("points Objekt", {"points": {}}),
                       ("null", None), ("Liste", [[20, 30], [30, 40]]), ("emergencyTemp", {"emergencyTemp": 90})]:
        r = n.post("/api/curve", json=body)
        assert r.status_code == 400, (name, r.status_code)
    r = n.post("/api/curve", data="{kaputt", headers={"Content-Type": "application/json"})
    assert r.status_code == 400
    r = n.post("/api/curve", data="", headers={"Content-Type": "application/json"})
    assert r.status_code == 400
    assert n.get("/api/curve").json() == {"points": DEFAULT_CURVE, "emergencyTemp": 60, "minPct": 20}
    swarm.sleep(45000)
    assert n.nvs_writes() == 0


def test_curve_post_body_over_1kb_rejected(swarm, sketch):
    import requests
    n = make(swarm, sketch)
    body = '{"points":[[20,30],[30,40]],"pad":"' + "x" * 2000 + '"}'
    try:
        r = n.post("/api/curve", data=body, headers={"Content-Type": "application/json"})
        assert r.status_code in (400, 413), r.status_code
    except requests.ConnectionError:
        pass  # Server schliesst ohne Antwort: ebenfalls abgelehnt
    assert n.get("/api/curve").json()["points"] == DEFAULT_CURVE


def test_curve_cannot_change_emergency_temp(swarm, sketch):
    n = make(swarm, sketch)
    r = n.post("/api/curve", json={"points": [[20, 30], [30, 40]], "emergencyTemp": 90})
    assert r.status_code in (200, 400)
    assert n.get("/api/curve").json()["emergencyTemp"] == 60


# ================================================================== Sicherheit
@pytest.mark.parametrize("login", ["false", "true"])
def test_login_modes_curve_and_control(swarm, sketch, login):
    n = make(swarm, sketch, cfg={"useLogin": login})
    unauth = 401 if login == "true" else 200
    assert n.get("/api/curve", auth=None).status_code == unauth
    swarm.sleep(1200)
    assert post_curve(n, CUSTOM_CURVE, auth=None).status_code == unauth
    swarm.sleep(1200)
    assert n.post("/control", auth=None, json={"target": "local", "action": "fan:speed:40"}).status_code == unauth
    swarm.sleep(1200)
    if login == "true":
        assert S(n)["mode"] == "auto"
        assert n.get("/api/curve").json()["points"] == DEFAULT_CURVE
        assert n.get("/api/curve", auth=("admin", "falsch")).status_code == 401
        assert n.get("/api/curve", auth=("admin", "falsch2")).status_code == 429
        swarm.sleep(1500)
        assert n.get("/api/curve").status_code == 200
        ok(ctl(n, "fan:speed:40"))
    else:
        assert S(n)["mode"] == "manual"
        assert n.get("/api/curve", auth=("admin", "falsch")).status_code == 200


@pytest.mark.parametrize("login", ["false", "true"])
def test_login_modes_read_endpoints(swarm, sketch, login):
    n = make(swarm, sketch, cfg={"useLogin": login})
    expect = 401 if login == "true" else 200
    for path in ("/", "/api/status", "/api/diag"):
        assert n.get(path, auth=None).status_code == expect, path
        swarm.sleep(1200)


@pytest.mark.parametrize("login", ["false", "true"])
def test_diag_no_secrets(swarm, sketch, login):
    n = make(swarm, sketch, cfg={"useLogin": login})
    r = n.get("/api/diag")
    assert r.status_code == 200
    assert r.headers.get("Cache-Control") == "no-store"
    assert r.headers.get("Content-Type", "").startswith("application/json")
    for secret in ("DEIN_WLAN_NAME", "DEIN_WLAN_PASSWORT", WEB_PASSWORD):
        assert secret not in r.text
    assert n.post("/api/diag", json={}).status_code in (404, 405)


@pytest.mark.parametrize("login", ["false", "true"])
def test_host_and_origin_and_content_type(swarm, sketch, login):
    n = make(swarm, sketch, cfg={"useLogin": login})
    h = expected_hostname(DEFAULT_NAME, n.ip, "servertemp-")
    for host in ("evil.com", f"{h}.evil.com", f"{h}x", "localhost"):
        assert n.get("/api/curve", headers={"Host": host}).status_code == 403, host
        assert post_curve(n, CUSTOM_CURVE, headers={"Host": host}).status_code == 403, host
        assert ctl(n, "fan:off", headers={"Host": host}).status_code == 403, host
    for host in (n.ip, h, f"{h}.local", f"{h}.fritz.box", h.upper()):
        assert n.get("/api/curve", headers={"Host": host}).status_code == 200, host
        swarm.sleep(50)
    for origin in ("http://evil.com", "null", f"http://{h}.evil.com", f"https://{n.ip}"):
        assert post_curve(n, CUSTOM_CURVE, headers={"Origin": origin}).status_code == 403, origin
        assert ctl(n, "fan:off", headers={"Origin": origin}).status_code == 403, origin
    for ctype, body in (("text/plain", "{}"), ("application/x-www-form-urlencoded", "points=1")):
        assert n.post("/api/curve", data=body, headers={"Content-Type": ctype}).status_code == 415
        assert n.post("/control", data=body, headers={"Content-Type": ctype}).status_code == 415
    assert post_curve(n, CUSTOM_CURVE, headers={"Origin": f"http://{h}.fritz.box"}).status_code == 200
    # nichts davon hat den Zustand veraendert
    assert S(n)["mode"] == "auto"


@pytest.mark.parametrize("login", ["false", "true"])
def test_no_cors_headers(swarm, sketch, login):
    n = make(swarm, sketch, cfg={"useLogin": login})
    for origin in ("http://evil.com", f"http://{n.ip}"):
        for method, path in (("GET", "/api/status"), ("GET", "/api/curve"), ("OPTIONS", "/api/curve"),
                             ("OPTIONS", "/control")):
            r = n.request(method, path, headers={"Origin": origin, "Access-Control-Request-Method": "POST"})
            assert not [k for k in r.headers if k.lower().startswith("access-control-")], (method, path, origin)


def test_local_domain_empty_means_none(swarm, sketch):
    n = make(swarm, sketch, cfg={"localDomain": ""})
    h = expected_hostname(DEFAULT_NAME, n.ip, "servertemp-")
    assert n.get("/api/status", headers={"Host": f"{h}.local"}).status_code == 200
    assert n.get("/api/status", headers={"Host": f"{h}.fritz.box"}).status_code == 403
