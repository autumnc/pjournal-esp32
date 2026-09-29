#!/usr/bin/env python3
"""Run all host-side IME regression checks."""

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECKS = [
    "check_ime_cases.py",
    "check_ime_behavior.py",
    "check_ime_session.py",
    "check_ime_tables.py",
]


def main():
    for script in CHECKS:
        path = ROOT / "scripts" / script
        print(f"==> {script}", flush=True)
        subprocess.run([sys.executable, str(path)], cwd=ROOT, check=True)
    print("OK: all IME checks passed")


if __name__ == "__main__":
    main()
