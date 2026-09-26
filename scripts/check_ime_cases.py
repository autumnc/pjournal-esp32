#!/usr/bin/env python3
import argparse
from pathlib import Path

import ime_query_sim


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CASES = ROOT / "scripts" / "ime_cases.txt"


def load_cases(path):
    cases = []
    for line_no, raw in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            raise SystemExit(f"{path}:{line_no}: expected code<TAB>candidate<TAB>max_rank")
        code, candidate, rank_text = parts
        try:
            max_rank = int(rank_text)
        except ValueError:
            raise SystemExit(f"{path}:{line_no}: bad max_rank {rank_text!r}")
        if max_rank < 1:
            raise SystemExit(f"{path}:{line_no}: max_rank must be >= 1")
        cases.append((code, candidate, max_rank))
    return cases


def main():
    ap = argparse.ArgumentParser(description="Check approximate IME candidate ranking cases.")
    ap.add_argument("--cases", default=str(DEFAULT_CASES))
    ap.add_argument("--limit", type=int, default=20)
    args = ap.parse_args()

    table = ime_query_sim.Ime3(ime_query_sim.DEFAULT_TABLE)
    seg_entries = ime_query_sim.load_seg(ime_query_sim.DEFAULT_SEG)
    failures = []
    for code, expected, max_rank in load_cases(args.cases):
        candidates = ime_query_sim.query(table, seg_entries, code, max(args.limit, max_rank))
        try:
            rank = candidates.index(expected) + 1
        except ValueError:
            rank = 0
        if rank == 0 or rank > max_rank:
            failures.append((code, expected, max_rank, candidates[:max_rank + 4]))
    if failures:
        for code, expected, max_rank, preview in failures:
            print(f"FAIL {code}: expected {expected!r} within {max_rank}, got {' '.join(preview)}")
        raise SystemExit(1)
    print(f"OK: {len(load_cases(args.cases))} IME ranking cases passed")


if __name__ == "__main__":
    main()
