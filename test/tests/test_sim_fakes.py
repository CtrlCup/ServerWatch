"""Selbsttest der Simulations-Fakes fuer ServerTemp (OneWire/DallasTemperature, LEDC, Interrupts, Preferences).

Baut den Test-Sketch test/sim/selftest/FakeSelftest (kein Produkt-Sketch) und prueft die Fakes gegen die
dokumentierte Semantik von arduino-esp32 2.0.17. Zeiten sind simulierte Millisekunden.
"""
import os
import struct
import subprocess
import sys

import pytest

from simnet import ROOT, NVS_TYPES

SKETCH = "FakeSelftest"
SKETCH_PATH = os.path.join(ROOT, "test", "sim", "selftest", "FakeSelftest", "FakeSelftest.ino")
TACH_PIN, PWM_PIN = 6, 5


@pytest.fixture(scope="module", autouse=True)
def build_selftest():
    subprocess.run([sys.executable, os.path.join(ROOT, "test", "sim", "build.py"), SKETCH_PATH], check=True,
                   stdout=subprocess.DEVNULL)


def kv(resp):
    assert resp.status_code == 200, resp.text
    return dict(line.split("=", 1) for line in resp.text.splitlines() if "=" in line)


def start(swarm, env=None, **kw):
    n = swarm.add("Fake", sketch=SKETCH, env=env, **kw)
    swarm.wait_ready(n)
    return n


def temp(n):
    return kv(n.get("/t", timeout_ms=10000, auth=None))


def pulses(n, ms=1000):
    return kv(n.get(f"/pulses?ms={ms}", timeout_ms=10000, auth=None))


def state(n):
    return kv(n.get("/state", auth=None))


# ------------------------------------------------------------------ DS18B20
@pytest.mark.parametrize("value, devs, expected", [
    ("23.5", "1", 23.5),
    ("-5.25", "1", -5.25),
    ("85", "1", 85.0),
    ("disconnected", "0", -127.0),
    (None, "0", -127.0),
])
def test_temperature_values(swarm, value, devs, expected):
    n = swarm.add("Fake", sketch=SKETCH, start=False)
    n.set_temp(value)
    n.start()
    swarm.wait_ready(n)
    t = temp(n)
    assert t["devs"] == devs
    assert float(t["temp"]) == expected
    assert float(t["temp_blocking"]) == expected
    assert t["disconnected_const"] == "-127"


def test_temperature_follows_file_and_rescan(swarm):
    n = start(swarm)
    n.set_temp("disconnected")
    t = temp(n)
    assert t["devs"] == "0" and float(t["temp"]) == -127.0
    n.set_temp("31.0")
    t = temp(n)  # /t scannt den Bus bei jedem Aufruf neu (begin())
    assert t["devs"] == "1" and float(t["temp"]) == 31.0


def test_conversion_time_11_bit(swarm):
    n = start(swarm)
    n.set_temp("20")
    t = temp(n)
    assert t["complete_immediately"] == "0"
    assert int(t["request_ms"]) < 50                      # setWaitForConversion(false): kehrt sofort zurueck
    assert 375 <= int(t["conv_ms"]) <= 375 + 120          # 750 ms >> (12 - 11)
    assert 375 <= int(t["blocking_ms"]) <= 375 + 120      # blockierend: gleiche Zeit


def test_resolution_9_bit_before_begin_12_after(swarm):
    n = start(swarm)
    n.set_temp("20")
    r = kv(n.get("/res", timeout_ms=10000, auth=None))
    assert 90 <= int(r["before_begin_ms"]) <= 90 + 120     # 750 ms >> 3 (9 Bit)
    assert 750 <= int(r["after_begin_ms"]) <= 750 + 150    # begin() ermittelt 12 Bit


# ------------------------------------------------------------------ LEDC
@pytest.mark.parametrize("freq, bits, ret", [
    (25000, 12, 0),        # 25000 * 4096 = 102 MHz > 80 MHz
    (25000, 8, 25000),     # 6.4 MHz
    (19531, 12, 19531),    # knapp unter der Grenze
    (19532, 12, 0),        # knapp darueber
    (0, 8, 0),
])
def test_ledc_setup_apb_limit(swarm, freq, bits, ret):
    n = start(swarm)
    r = kv(n.get(f"/ledc?ch=1&freq={freq}&bits={bits}&duty=7", auth=None))
    assert int(r["ret"]) == ret
    assert n.pwm(9) == (7 if ret else None)                # abgelehnt: Kanal unkonfiguriert, ledcWrite wirkungslos


def test_ledc_events_and_pwm_file(swarm):
    n = start(swarm)
    assert n.pwm(PWM_PIN) is None
    for duty in (0, 128, 255):
        n.get(f"/pwm?duty={duty}", auth=None)
        assert n.pwm(PWM_PIN) == duty
    evs = [e for _, e in n.events() if e.startswith("ledc ")]
    assert evs == [f"ledc {PWM_PIN} 0", f"ledc {PWM_PIN} 128", f"ledc {PWM_PIN} 255"]


def test_ledc_state_does_not_survive_restart(swarm):
    n = start(swarm)
    n.get("/pwm?duty=200", auth=None)
    assert n.pwm(PWM_PIN) == 200
    n.get("/restart", auth=None)
    assert swarm.wait_for(lambda: n.boots() >= 2 and n.get("/", 2000).status_code == 200, 20000)
    assert n.pwm(PWM_PIN) is None


def test_arduino_version_header(swarm):
    n = start(swarm)
    s = state(n)
    assert s["esp"] == "2.0.17" and s["ver_ok"] == "1"
    assert int(s["ver_val"]) == (2 << 16) | (0 << 8) | 17


# ------------------------------------------------------------------ Tacho
def within(value, expected, tol=0.15):
    return abs(value - expected) <= max(3, expected * tol)


@pytest.mark.parametrize("rpm", [3000, 600, 1500])
def test_tach_pulses_follow_rpm_file(swarm, rpm):
    n = start(swarm)
    n.set_fan_rpm(rpm)
    p = int(pulses(n)["pulses"])
    assert within(p, rpm * 2 / 60), f"{p} Pulse in 1 s bei {rpm} rpm"


def test_tach_zero_without_ledc_or_rpm(swarm):
    n = start(swarm)
    assert int(pulses(n)["pulses"]) == 0
    n.set_fan_rpm(0)
    assert int(pulses(n)["pulses"]) == 0


def test_tach_model_from_duty(swarm):
    n = start(swarm)
    n.get("/pwm?duty=255", auth=None)
    assert within(int(pulses(n)["pulses"]), 100)           # 3000 rpm * 2 / 60
    n.get("/pwm?duty=128", auth=None)
    assert within(int(pulses(n)["pulses"]), 50, 0.2)       # ~50 %
    n.get("/pwm?duty=0", auth=None)
    assert int(pulses(n)["pulses"]) == 0


def test_tach_model_max_rpm_env(swarm):
    n = start(swarm, env={"SIM_FAN_MAX_RPM": "1500"})
    n.get("/pwm?duty=255", auth=None)
    assert within(int(pulses(n)["pulses"]), 50)


def test_tach_rpm_file_overrides_model_and_none_returns_to_model(swarm):
    n = start(swarm)
    n.get("/pwm?duty=255", auth=None)
    n.set_fan_rpm(600)
    assert within(int(pulses(n)["pulses"]), 20)
    n.set_fan_rpm(None)
    assert within(int(pulses(n)["pulses"]), 100)


def test_tach_model_power_pin_low_stops_fan(swarm):
    n = start(swarm)
    n._write("fan_power_pin", "7")
    n.get("/pwm?duty=255", auth=None)
    assert within(int(pulses(n)["pulses"]), 100)
    n.get("/pin?pin=7&v=0", auth=None)
    assert int(pulses(n)["pulses"]) == 0
    n.get("/pin?pin=7&v=1", auth=None)
    assert within(int(pulses(n)["pulses"]), 100)


def test_critical_section_blocks_isr(swarm):
    n = start(swarm)
    n.set_fan_rpm(3000)
    p = pulses(n)
    assert int(p["crit_delta"]) == 0                       # portENTER_CRITICAL haelt die ISR an
    assert int(p["nointr_delta"]) == 0                     # noInterrupts() ebenso
    assert int(p["after_crit_delta"]) >= 10                # danach laufen die Pulse weiter


def test_no_catch_up_after_blocked_section(swarm):
    n = start(swarm)
    n.set_fan_rpm(1500)                                    # 50 Pulse/s, Halbperiode 10 ms
    p = pulses(n)
    assert int(p["crit_delta"]) == 0
    # nach 300 ms Blockade hoechstens die ausstehende Flanke plus ~30 ms Normalbetrieb (nicht ~15 nachgeholte)
    assert int(p["burst_delta"]) <= 3
    assert 5 <= int(p["after_crit_delta"]) <= 15           # 170 ms * 50 Hz = 8.5


def test_no_phantom_pulse_after_attach(swarm):
    n = start(swarm)
    n.set_fan_rpm(600)                                     # 20 Pulse/s, Halbperiode 50 ms, Periode 100 ms
    r = kv(n.get("/attach_probe?ms=20", auth=None))
    assert r["early"] == "0"                               # vor Ablauf der Halbperiode keine Flanke
    assert 4 <= int(r["later"]) <= 8                       # danach laufen Pulse (320 ms * 20 Hz = 6,4)


def test_no_isr_after_detach_returns(swarm):
    n = start(swarm)
    n.set_fan_rpm(3000)
    r = kv(n.get("/detach_stress", timeout_ms=30000, auth=None))
    assert r["bad"] == "0"
    assert int(pulses(n)["pulses"]) > 50                   # am Ende wieder angehaengt


def test_detach_stops_isr_and_attach_resumes(swarm):
    n = start(swarm)
    n.set_fan_rpm(3000)
    assert int(pulses(n)["pulses"]) > 50
    n.get("/detach", auth=None)
    assert int(pulses(n)["pulses"]) == 0
    n.get("/attach", auth=None)
    assert int(pulses(n)["pulses"]) > 50


# ------------------------------------------------------------------ Preferences
def test_preferences_api_and_raw_format(swarm):
    n = start(swarm)
    r = kv(n.get("/nvs", auth=None))
    assert r["ro_missing_ns"] == "0"
    assert (r["put_uc"], r["put_i"], r["put_b"], r["put_s"], r["put_blob"]) == ("1", "4", "1", "5", "4")
    assert r["blob_len"] == "4"
    assert r["get_blob_small"] == "0"                      # Puffer zu klein
    assert r["get_blob_big"] == "4:250"
    assert r["missing_default"] == "77"
    assert (r["is_key"], r["is_key_missing"]) == ("1", "0")
    assert (r["remove_missing"], r["remove_tmp"], r["is_key_tmp"]) == ("0", "1", "0")
    assert r["long_key"] == "0"                            # Schluessel > 15 Zeichen
    assert r["put_after_end"] == "0" and r["put_readonly"] == "0"
    assert n.nvs_raw("selftest", "uc") == b"\xc8"
    assert n.nvs_raw("selftest", "i") == struct.pack("<i", -12345)
    assert n.nvs_raw("selftest", "b") == b"\x01"
    assert n.nvs_raw("selftest", "s") == b"hello\x00"
    assert n.nvs_raw("selftest", "blob") == bytes([1, 2, 3, 250])
    assert n.nvs_raw("selftest", "nope") is None
    assert [n.nvs_type("selftest", k) for k in ("uc", "i", "b", "s", "blob")] == [
        NVS_TYPES["u8"], NVS_TYPES["i32"], NVS_TYPES["u8"], NVS_TYPES["str"], NVS_TYPES["blob"]]
    s = state(n)
    assert (s["uc"], s["i"], s["b"], s["s"], s["blob_len"]) == ("200", "-12345", "1", "hello", "4")


def test_preferences_persist_over_restart_and_kill(swarm):
    n = start(swarm)
    assert state(n)["boots"] == "1"
    kv(n.get("/nvs", auth=None))
    n.get("/restart", auth=None)
    assert swarm.wait_for(lambda: n.boots() >= 2 and n.get("/", 2000).status_code == 200, 20000)
    s = state(n)
    assert s["boots"] == "2" and s["i"] == "-12345" and s["s"] == "hello"
    n.kill()
    n.start()
    swarm.wait_ready(n)
    s = state(n)
    assert s["boots"] == "3" and s["i"] == "-12345" and s["blob_len"] == "4"


def test_nvs_write_events_are_counted(swarm):
    n = start(swarm)
    assert n.nvs_writes() == 1 and n.nvs_writes("boots") == 1 and n.nvs_writes("selftest/boots") == 1
    kv(n.get("/nvs", auth=None))
    assert n.nvs_writes("uc") == 1 and n.nvs_writes("i") == 1
    assert n.nvs_writes("tmp") == 1                        # jeder put... zaehlt, auch wenn spaeter entfernt
    assert n.nvs_writes("0123456789abcdef") == 0           # abgelehnter put schreibt nicht
    assert n.nvs_writes("nope") == 0
    total = n.nvs_writes()
    kv(n.get("/nvs", auth=None))
    assert n.nvs_writes() == 2 * total - 1                 # beim zweiten Mal ohne den Boot-Schreibvorgang
    assert n.nvs_writes("uc") == 2


def test_nvs_corrupt_gives_defaults(swarm):
    n = start(swarm)
    kv(n.get("/nvs", auth=None))
    n.nvs_corrupt("selftest", "i", b"\x01", "i32")          # richtiger Typ, falsche Groesse
    n.nvs_corrupt("selftest", "uc", b"", "u8")              # leer
    n.nvs_corrupt("selftest", "b", "xx", "u8")
    n.nvs_corrupt("selftest", "s", "valid")                 # unbekannter Typ
    s = state(n)
    assert s["i"] == "-1" and s["uc"] == "99" and s["b"] == "0" and s["s"] == "none"
    assert n.nvs_raw("selftest", "i") == b"\x01"
    n.nvs_corrupt("selftest", "i", struct.pack("<i", 7), "u32")  # passende Groesse, falscher Typ
    assert state(n)["i"] == "-1"
    n.nvs_corrupt("selftest", "i", struct.pack("<i", 7), "i32")
    assert state(n)["i"] == "7"


def test_nvs_type_mismatch_gives_default(swarm):
    n = start(swarm)
    r = kv(n.get("/nvs_types", auth=None))
    assert r["begin_partition"] == "1"
    assert (r["int_on_uint"], r["uint_on_uint"], r["uint_on_int"]) == ("-1", "5", "99")
    assert (r["bloblen_on_str"], r["bytes_on_str"], r["uint_on_str"]) == ("0", "0", "99")
    assert (r["str_on_blob"], r["bytes_on_blob"]) == ("def", "3")
    assert (r["bytes_nobuf"], r["bytes_nobuf2"], r["bytes_nolen"]) == ("0", "0", "0")
    assert (r["uchar_on_bool"], r["bool_on_uchar"]) == ("1", "1")   # beides U8 wie im echten NVS


def test_nvs_clear_removes_keys(swarm):
    n = start(swarm)
    kv(n.get("/nvs", auth=None))
    assert kv(n.get("/clear", auth=None))["cleared"] == "1"
    assert n.nvs_raw("selftest", "i") is None and n.nvs_raw("selftest", "boots") is None
    assert state(n)["i"] == "-1"
