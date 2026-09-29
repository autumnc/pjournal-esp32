#!/usr/bin/env python3
"""Summarize IME perflog output from ESP-IDF monitor logs."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


LOOKUP_RE = re.compile(
    r"perf lookup code='(?P<code>[^']*)'\s+"
    r"exit=(?P<exit>\S+)\s+total=(?P<total>\d+)us\s+"
    r"cand=(?P<cand>\d+)\s+limit=(?P<limit>\d+)\s+"
    r"hv=(?P<hv>[01])\s+inc=(?P<inc>[01])\s+fixed=(?P<fixed>[01])"
)
SENTENCE_RE = re.compile(
    r"perf sentence code='(?P<code>[^']*)'\s+"
    r"total=(?P<total>\d+)us\s+arcs=(?P<arcs>\d+)\s+"
    r"nodes=(?P<nodes>\d+)\s+groups=(?P<groups>\d+)\s+added=(?P<added>\d+)"
)


@dataclass
class TimingGroup:
    total_us: list[int] = field(default_factory=list)
    max_candidates: int = 0
    max_limit: int = 0
    exits: dict[str, int] = field(default_factory=dict)
    max_arcs: int = 0
    max_nodes: int = 0
    max_groups: int = 0
    max_added: int = 0

    def add_lookup(self, match: re.Match[str]) -> None:
        self.total_us.append(int(match.group("total")))
        self.max_candidates = max(self.max_candidates, int(match.group("cand")))
        self.max_limit = max(self.max_limit, int(match.group("limit")))
        exit_name = match.group("exit")
        self.exits[exit_name] = self.exits.get(exit_name, 0) + 1

    def add_sentence(self, match: re.Match[str]) -> None:
        self.total_us.append(int(match.group("total")))
        self.max_arcs = max(self.max_arcs, int(match.group("arcs")))
        self.max_nodes = max(self.max_nodes, int(match.group("nodes")))
        self.max_groups = max(self.max_groups, int(match.group("groups")))
        self.max_added = max(self.max_added, int(match.group("added")))


def percentile_nearest_rank(values: list[int], percent: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    rank = int((percent / 100.0) * len(ordered) + 0.999999)
    rank = min(max(rank, 1), len(ordered))
    return ordered[rank - 1]


def summarize_group(group: TimingGroup) -> dict[str, object]:
    total = group.total_us
    count = len(total)
    avg = sum(total) / count if count else 0.0
    row: dict[str, object] = {
        "count": count,
        "avg_us": round(avg, 1),
        "p95_us": percentile_nearest_rank(total, 95),
        "max_us": max(total) if total else 0,
    }
    if group.max_candidates or group.max_limit or group.exits:
        row.update(
            {
                "max_candidates": group.max_candidates,
                "max_limit": group.max_limit,
                "exits": group.exits,
            }
        )
    if group.max_arcs or group.max_nodes or group.max_groups or group.max_added:
        row.update(
            {
                "max_arcs": group.max_arcs,
                "max_nodes": group.max_nodes,
                "max_groups": group.max_groups,
                "max_added": group.max_added,
            }
        )
    return row


def parse_lines(lines: Iterable[str]) -> tuple[dict[str, TimingGroup], dict[str, TimingGroup]]:
    lookup: dict[str, TimingGroup] = {}
    sentence: dict[str, TimingGroup] = {}
    for line in lines:
        match = LOOKUP_RE.search(line)
        if match:
            group = lookup.setdefault(match.group("code"), TimingGroup())
            group.add_lookup(match)
            continue
        match = SENTENCE_RE.search(line)
        if match:
            group = sentence.setdefault(match.group("code"), TimingGroup())
            group.add_sentence(match)
    return lookup, sentence


def iter_inputs(paths: list[str]) -> Iterable[str]:
    if not paths:
        yield from sys.stdin
        return

    for raw_path in paths:
        if raw_path == "-":
            yield from sys.stdin
            continue
        with Path(raw_path).open("r", encoding="utf-8", errors="replace") as handle:
            yield from handle


def compact_exits(exits: dict[str, int]) -> str:
    if not exits:
        return "-"
    return ",".join(f"{key}:{exits[key]}" for key in sorted(exits))


def print_lookup_table(groups: dict[str, TimingGroup], limit: int) -> None:
    print("lookup")
    print("code                  n    avg_us   p95_us   max_us  cand  limit  exits")
    print("--------------------  ---  -------  -------  -------  ----  -----  -----")
    for code, group in sorted(groups.items(), key=lambda item: max(item[1].total_us), reverse=True)[:limit]:
        summary = summarize_group(group)
        print(
            f"{code[:20]:20}  {summary['count']:>3}  {summary['avg_us']:>7}  "
            f"{summary['p95_us']:>7}  {summary['max_us']:>7}  "
            f"{summary.get('max_candidates', 0):>4}  {summary.get('max_limit', 0):>5}  "
            f"{compact_exits(summary.get('exits', {}))}"
        )


def print_sentence_table(groups: dict[str, TimingGroup], limit: int) -> None:
    print()
    print("sentence")
    print("code                  n    avg_us   p95_us   max_us  arcs  nodes  groups  added")
    print("--------------------  ---  -------  -------  -------  ----  -----  ------  -----")
    for code, group in sorted(groups.items(), key=lambda item: max(item[1].total_us), reverse=True)[:limit]:
        summary = summarize_group(group)
        print(
            f"{code[:20]:20}  {summary['count']:>3}  {summary['avg_us']:>7}  "
            f"{summary['p95_us']:>7}  {summary['max_us']:>7}  "
            f"{summary.get('max_arcs', 0):>4}  {summary.get('max_nodes', 0):>5}  "
            f"{summary.get('max_groups', 0):>6}  {summary.get('max_added', 0):>5}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize 'perf lookup' and 'perf sentence' IME logs."
    )
    parser.add_argument("logs", nargs="*", help="Monitor log files. Omit or pass '-' for stdin.")
    parser.add_argument("--json", action="store_true", help="Emit JSON instead of tables.")
    parser.add_argument("--limit", type=int, default=40, help="Rows per table, sorted by max latency.")
    args = parser.parse_args()

    lookup, sentence = parse_lines(iter_inputs(args.logs))
    if args.json:
        payload = {
            "lookup": {code: summarize_group(group) for code, group in sorted(lookup.items())},
            "sentence": {code: summarize_group(group) for code, group in sorted(sentence.items())},
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return 0

    if not lookup and not sentence:
        print("No IME perflog lines found.", file=sys.stderr)
        return 1

    if lookup:
        print_lookup_table(lookup, args.limit)
    if sentence:
        print_sentence_table(sentence, args.limit)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
