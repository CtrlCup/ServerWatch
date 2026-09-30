#!/usr/bin/env python3
"""Entfernt einen behobenen Fehler aus der Testsuite: alle @known_bug("<key>")-Marker
und den Eintrag in KNOWN_BUGS (tests/conftest.py).  Nutzung: test/unmark_bug.py <key> [...]"""
import glob
import os
import re
import sys

TESTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests")

for key in sys.argv[1:]:
    hits = 0
    for path in glob.glob(os.path.join(TESTS, "*.py")):
        src = open(path, encoding="utf-8").read()
        new = re.sub(rf'^@known_bug\("{re.escape(key)}"\)\n', "", src, flags=re.M)
        new = re.sub(rf'^    "{re.escape(key)}": \d+,\n', "", new, flags=re.M)
        if new != src:
            hits += src.count(f'"{key}"') - new.count(f'"{key}"')
            open(path, "w", encoding="utf-8").write(new)
    print(f"{key}: {hits} Stellen entfernt")
