#!/usr/bin/env python3
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "main" / "ime" / "seg_table_source.txt"
HDR = ROOT / "main" / "ime" / "seg_table.h"
TRAD_HDR = ROOT / "main" / "ime" / "trad_table.h"
TRAD_LO, TRAD_HI = 0x346E, 0x9FD3

# 繁简同形字不能进繁体位图, 否则简体模式会藏掉"为什么/图像/沈阳/瞭望"等正常词组。
TRAD_MUST_BE_UNMARKED = "么仝俱像叚哩喆堃彷搧椀沈淼澂牴犇甦甯瞭礆祇筦絜耑舖菉藉衹覆迺遶邨阪陞龢"
TRAD_MUST_BE_MARKED = "們這說會麼爲與舊縣裏"


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


def parse_trad_bitmap():
    text = TRAD_HDR.read_text(encoding="utf-8")
    match = re.search(r"kTradBitmap\[(\d+)\] = \{(.*?)\};", text, re.S)
    if not match:
        raise SystemExit("trad_table.h mismatch: kTradBitmap not found")
    values = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", match.group(2))]
    if len(values) != int(match.group(1)):
        raise SystemExit("trad_table.h mismatch: bitmap size")
    return values


def trad_marked(bitmap, ch):
    off = ord(ch) - TRAD_LO
    if off < 0 or off >= len(bitmap) * 8:
        return False
    return (bitmap[off >> 3] >> (off & 7)) & 1


def check_trad_table():
    bitmap = parse_trad_bitmap()
    bad = [c for c in TRAD_MUST_BE_UNMARKED if trad_marked(bitmap, c)]
    if bad:
        raise SystemExit(f"trad_table.h 误标繁简同形字: {''.join(bad)}")
    bad = [c for c in TRAD_MUST_BE_MARKED if not trad_marked(bitmap, c)]
    if bad:
        raise SystemExit(f"trad_table.h 漏标繁体字: {''.join(bad)}")
    try:
        import opencc
    except ImportError:
        print("OK: trad_table.h 关键位检查 (opencc 未安装, 跳过全量校验)")
        return
    cc = opencc.OpenCC("t2s")
    for cp in range(TRAD_LO, TRAD_HI + 1):
        expected = cc.convert(chr(cp)) != chr(cp)
        if expected != bool(trad_marked(bitmap, chr(cp))):
            raise SystemExit(f"trad_table.h 与 OpenCC t2s 不一致: U+{cp:04X}")
    print(f"OK: trad_table.h 与 OpenCC t2s 一致 ({TRAD_HI - TRAD_LO + 1} 码位)")


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
    check_trad_table()


if __name__ == "__main__":
    main()
