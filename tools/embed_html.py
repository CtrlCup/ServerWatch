#!/usr/bin/env python3
"""Erzeugt dashboard_html.h aus serverwatch_Multi_interface.html (einzige Quelle, Issue #21).

  python3 tools/embed_html.py          # Header neu schreiben
  python3 tools/embed_html.py --check  # nur prüfen, ob der Header aktuell ist (Exit-Code 1 wenn nicht)

Die Minifizierung ist bewusst einfach: Einrückung, Leerzeilen und ganzzeilige Kommentare
(//, /* */, <!-- -->) werden entfernt. Mehrzeilige Strings mit bedeutsamer Einrückung gibt es nicht.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "serverwatch_Multi_interface.html")
TARGET = os.path.join(ROOT, "dashboard_html.h")
FULL_LINE_COMMENT = re.compile(r"^(//.*|/\*.*\*/|<!--.*-->)$")


def render():
    with open(SOURCE, encoding="utf-8") as f:
        lines = [line.strip() for line in f]
    body = "\n".join(line for line in lines if line and not FULL_LINE_COMMENT.match(line))
    if ")rawliteral\"" in body:
        raise SystemExit("Quelle enthält den Raw-String-Terminator")
    return ("// AUTOMATISCH ERZEUGT aus serverwatch_Multi_interface.html mit tools/embed_html.py.\n"
            "// Nicht von Hand ändern: HTML-Datei bearbeiten und das Skript ausführen.\n"
            "#pragma once\n\n"
            "const char htmlTemplate[] PROGMEM = R\"rawliteral(\n" + body + "\n)rawliteral\";\n")


def main():
    out = render()
    current = open(TARGET, encoding="utf-8").read() if os.path.exists(TARGET) else ""
    if "--check" in sys.argv:
        if current != out:
            print("dashboard_html.h ist veraltet: python3 tools/embed_html.py ausführen")
            sys.exit(1)
        print("dashboard_html.h ist aktuell")
        return
    with open(TARGET, "w", encoding="utf-8") as f:
        f.write(out)
    print(f"dashboard_html.h geschrieben ({len(out)} Bytes)")


if __name__ == "__main__":
    main()
