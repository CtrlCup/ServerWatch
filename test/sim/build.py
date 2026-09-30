#!/usr/bin/env python3
"""Baut die Sketches als Host-Programme gegen die Fake-Bibliotheken in fakes/.

Der Sketch-Code bleibt unveraendert, es werden nur zwei mechanische Schritte angewendet
(wie beim Arduino-Builder bzw. fuer die Pro-Knoten-Konfiguration):
  1. Top-level `const char* NAME = "..."` (oder `= MAKRO;`) -> `sim_cfg("NAME", ...)` und
     `const bool NAME = true|false;` -> `sim_cfg_bool(...)`, damit jeder simulierte Knoten per
     Umgebungsvariable SWCFG_NAME eigene Werte (serverName, ...) bekommt.
  2. Funktionsprototypen werden vor der ersten Funktionsdefinition eingefuegt (Arduino-Verhalten).

Nutzung: python3 test/sim/build.py [Sketch.ino ...]   (Standard: beide Sketches)
Ausgabe: test/.build/sim/<Sketch>  (ausfuehrbar; anderes Ziel per SIM_OUT)
"""
import os
import re
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.environ.get("SIM_OUT") or os.path.join(ROOT, "test", ".build", "sim")
DEPS = os.path.join(HERE, ".deps")
FAKES = os.path.join(HERE, "fakes")
ARDUINOJSON_URL = "https://github.com/bblanchon/ArduinoJson/releases/download/v6.21.5/ArduinoJson-v6.21.5.h"
SKETCHES = ["ServerWatch_Multi.ino", "Serverwatch.ino"]

CXX = os.environ.get("CXX", "g++")
CXXFLAGS = [
    "-std=gnu++17", "-O1", "-g", "-Wall", "-Wno-unused-variable", "-Wno-unused-function",
    "-DARDUINO=10819", "-DESP32", "-DARDUINO_ARCH_ESP32", "-DSERVERWATCH_SIM=1",
    "-DARDUINOJSON_ENABLE_PROGMEM=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
    "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
    "-I", FAKES, "-I", DEPS, "-I", ROOT,  # ROOT: Header neben den Sketches (dashboard_html.h, secrets.h)
]

RAW_STRING = re.compile(r'R"([^(\s]*)\((.*?)\)\1"', re.S)
CONFIG_STR = re.compile(r'^(const\s+char\s*\*\s*(?:const\s+)?)(\w+)(\s*=\s*)("(?:[^"\\\n]|\\.)*"|[A-Z_][A-Z0-9_]*)\s*;', re.M)
CONFIG_BOOL = re.compile(r'^(const\s+bool\s+)(\w+)(\s*=\s*)(true|false)\s*;', re.M)
FUNC_DEF = re.compile(
    r'^(?!(?:if|else|for|while|switch|return|case|do)\b)'
    r'((?:static\s+|inline\s+)*[A-Za-z_][\w:<>,]*(?:\s*[\*&])?)\s+([\*&]?\s*[A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*(?:const\s*)?\{',
    re.M)


def ensure_deps():
    os.makedirs(DEPS, exist_ok=True)
    target = os.path.join(DEPS, "ArduinoJson.h")
    if not os.path.exists(target):
        print("Lade ArduinoJson 6.21.5 ...")
        urllib.request.urlretrieve(ARDUINOJSON_URL, target)


def transform(src: str):
    names = [m.group(2) for m in CONFIG_STR.finditer(src)] + [m.group(2) for m in CONFIG_BOOL.finditer(src)]
    src = CONFIG_STR.sub(lambda m: f'{m.group(1)}{m.group(2)}{m.group(3)}sim_cfg("{m.group(2)}", {m.group(4)});', src)
    src = CONFIG_BOOL.sub(lambda m: f'{m.group(1)}{m.group(2)}{m.group(3)}sim_cfg_bool("{m.group(2)}", {m.group(4)});', src)

    # Prototypen: Raw-Strings (HTML/JS) vorher ausblenden, Positionen bleiben gleich lang.
    masked = RAW_STRING.sub(lambda m: "R" + re.sub(r"[^\n]", " ", m.group(0)[1:]), src)
    protos, first = [], None
    for m in FUNC_DEF.finditer(masked):
        ret, name, args = m.group(1).strip(), m.group(2).replace(" ", ""), m.group(3).strip()
        if ret in ("return", "else", "new", "delete"):
            continue
        args = re.sub(r"\s*=\s*[^,]+", "", args)  # Default-Argumente nicht wiederholen
        protos.append(f"{ret} {name}({args});")
        if first is None:
            first = m.start()
    if first is not None:
        src = src[:first] + "// --- automatisch erzeugte Prototypen (sim/build.py) ---\n" + "\n".join(protos) + "\n\n" + src[first:]
    return '#include "Arduino.h"\n#line 1\n' + src, names


def run(cmd):
    print(" ".join(cmd) if os.environ.get("SIM_VERBOSE") else f"  {os.path.basename(cmd[-1])}")
    subprocess.run(cmd, check=True)


def main(argv):
    sketches = argv or SKETCHES
    ensure_deps()
    os.makedirs(OUT, exist_ok=True)
    runtime_obj = os.path.join(OUT, "sim_runtime.o")
    runtime_src = os.path.join(FAKES, "sim_runtime.cpp")
    headers = [os.path.join(FAKES, f) for f in os.listdir(FAKES)]
    if not os.path.exists(runtime_obj) or os.path.getmtime(runtime_obj) < max(os.path.getmtime(h) for h in headers):
        run([CXX, *CXXFLAGS, "-c", "-o", runtime_obj, runtime_src])
    for sketch in sketches:
        name = os.path.splitext(os.path.basename(sketch))[0]
        sketch_dir = os.path.dirname(os.path.join(ROOT, sketch))
        with open(os.path.join(ROOT, sketch), encoding="utf-8") as f:
            code, names = transform(f.read())
        cpp = os.path.join(OUT, name + ".cpp")
        with open(cpp, "w", encoding="utf-8") as f:
            f.write(code)
        with open(os.path.join(OUT, name + ".config"), "w") as f:
            f.write("\n".join(names) + "\n")
        print(f"{sketch}: ueberschreibbare Konfiguration: {', '.join(names) or '-'}")
        obj = os.path.join(OUT, name + ".o")
        run([CXX, *CXXFLAGS, "-I", sketch_dir, "-Wno-switch", "-c", "-o", obj, cpp])
        run([CXX, "-o", os.path.join(OUT, name), obj, runtime_obj, "-lpthread"])
    print("Sim-Build OK")


if __name__ == "__main__":
    main(sys.argv[1:])
