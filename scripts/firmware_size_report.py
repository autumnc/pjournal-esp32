#!/usr/bin/env python3
"""Report firmware image sizes and app partition headroom."""

from pathlib import Path
import argparse
import json

ROOT = Path(__file__).resolve().parents[1]


def parse_size(text):
    text = text.strip().lower()
    if text.startswith("0x"):
        return int(text, 16)
    return int(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build-yong-ime")
    ap.add_argument("--app-partition-size", default="0xc00000")
    args = ap.parse_args()

    build = ROOT / args.build_dir
    app = build / "pjournal.bin"
    merged = build / "pjournal-merged.bin"
    part_size = parse_size(args.app_partition_size)
    if not app.exists():
        raise SystemExit(f"missing app image: {app}")

    app_size = app.stat().st_size
    free = part_size - app_size
    report = {
        "build_dir": str(build.relative_to(ROOT)),
        "app": str(app.relative_to(ROOT)),
        "app_size": app_size,
        "app_partition_size": part_size,
        "app_free": free,
        "app_free_percent": round(free * 100 / part_size, 2),
    }
    if merged.exists():
        report["merged"] = str(merged.relative_to(ROOT))
        report["merged_size"] = merged.stat().st_size
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
