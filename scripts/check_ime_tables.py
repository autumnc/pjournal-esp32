#!/usr/bin/env python3
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "main" / "ime" / "seg_table_source.txt"
HDR = ROOT / "main" / "ime" / "seg_table.h"


ENTRY_RE = re.compile(
    r'\{\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)",\s*(\d+),\s*"([^"]*)"\s*\},'
)
ARRAY_RE = re.compile(r"static const uint16_t (\w+)\[] = \{(.*?)\};", re.S)


def initial_of(syllable, keep_zh_ch_sh):
    if keep_zh_ch_sh and syllable[:2] in ("zh", "ch", "sh"):
        return syllable[:2]
    return syllable[:1]


def parse_source():
    entries = []
    seen = set()
    for raw in SRC.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        syllables, word = line.split("\t", 1)
        syllables = " ".join(syllables.strip().split())
        word = word.strip()
        key = (syllables, word)
        if key in seen:
            continue
        seen.add(key)
        parts = syllables.split(" ")
        entries.append((
            syllables,
            "".join(parts),
            "".join(initial_of(s, True) for s in parts),
            "".join(initial_of(s, False) for s in parts),
            str(len(parts)),
            word,
        ))
    return entries


def prefix_key(text):
    if not text:
        return 26 * 26
    c0 = ord(text[0]) - ord("a")
    if c0 < 0 or c0 >= 26:
        return 26 * 26
    if len(text) == 1:
        return c0 * 26
    c1 = ord(text[1]) - ord("a")
    if c1 < 0 or c1 >= 26:
        return 26 * 26
    return c0 * 26 + c1


def build_order_and_index(entries, field_idx):
    ordered = sorted(enumerate(entries), key=lambda item: (item[1][field_idx], item[0]))
    counts = [0] * (26 * 26)
    for _, entry in ordered:
        k = prefix_key(entry[field_idx])
        if k < len(counts):
            counts[k] += 1
    index = [0]
    total = 0
    for count in counts:
        total += count
        index.append(total)
    return [idx for idx, _ in ordered], index


def parse_header():
    text = HDR.read_text(encoding="utf-8")
    entries = []
    for line in text.splitlines():
        match = ENTRY_RE.search(line)
        if match:
            entries.append(match.groups())
    arrays = {}
    for match in ARRAY_RE.finditer(text):
        arrays[match.group(1)] = [int(v) for v in re.findall(r"\d+", match.group(2))]
    return entries, arrays


def main():
    expected = parse_source()
    actual, arrays = parse_header()
    if actual != expected:
        raise SystemExit(
            f"seg_table.h mismatch: expected {len(expected)} entries, got {len(actual)}"
        )
    expected_arrays = {}
    expected_arrays["SEG_CODE_ORDER"], expected_arrays["SEG_CODE_INDEX"] = build_order_and_index(expected, 1)
    expected_arrays["SEG_INITIAL_ORDER"], expected_arrays["SEG_INITIAL_INDEX"] = build_order_and_index(expected, 2)
    expected_arrays["SEG_COMPACT_INITIAL_ORDER"], expected_arrays["SEG_COMPACT_INITIAL_INDEX"] = build_order_and_index(expected, 3)
    for name, expected_values in expected_arrays.items():
        actual_values = arrays.get(name)
        if actual_values != expected_values:
            raise SystemExit(f"{name} mismatch")
    print(f"OK: {len(actual)} SEG_TABLE entries")


if __name__ == "__main__":
    main()
