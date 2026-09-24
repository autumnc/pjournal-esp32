#!/usr/bin/env python3
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SEG_SOURCE = ROOT / "main" / "ime" / "seg_table_source.txt"
IME_CPP = ROOT / "main" / "ime" / "IME.cpp"


def initial_of(syllable, keep_zh_ch_sh):
    if keep_zh_ch_sh and syllable[:2] in ("zh", "ch", "sh"):
        return syllable[:2]
    return syllable[:1]


def load_seg_entries():
    entries = []
    seen = set()
    for raw in SEG_SOURCE.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        syllables, word = line.split("\t", 1)
        parts = syllables.strip().split()
        key = (" ".join(parts), word.strip())
        if key in seen:
            continue
        seen.add(key)
        code = "".join(parts)
        entries.append({
            "syllables": parts,
            "code": code,
            "initial": "".join(initial_of(s, True) for s in parts),
            "compact_initial": "".join(initial_of(s, False) for s in parts),
            "word": word.strip(),
        })
    return entries


def seg_candidates(query, entries):
    q = query.replace("'", "")
    out = []
    for e in entries:
        if e["code"].startswith(q):
            out.append((0 if len(e["code"]) == len(q) else 1, len(e["code"]), e["word"]))
    return [word for _, _, word in sorted(out)]


def initial_candidates(query, entries):
    out = []
    for e in entries:
        matches = []
        if e["initial"].startswith(query):
            matches.append(e["initial"])
        if e["compact_initial"].startswith(query):
            matches.append(e["compact_initial"])
        if not matches:
            continue
        best_len = min(len(m) for m in matches)
        out.append((0 if best_len == len(query) else 1, best_len, e["word"]))
    return [word for _, _, word in sorted(out)]


def load_builtin_predict():
    text = IME_CPP.read_text(encoding="utf-8")
    block = text.split("static const BuiltinPredictEntry BUILTIN_PREDICT[] = {", 1)[1]
    block = block.split("};", 1)[0]
    entries = {}
    entry_re = re.compile(r'\{"([^"]+)",\s*\{([^}]*)\}\},')
    for match in entry_re.finditer(block):
        key = match.group(1)
        items = [m.group(1) for m in re.finditer(r'"([^"]+)"', match.group(2))]
        entries[key] = items
    return entries


def parse_user_line(raw):
    line = raw.strip()
    if len(line) < 3 or " " not in line:
        return None
    code, rest = line.split(" ", 1)
    if not code:
        return None
    parts = rest.split(" ")
    word = parts[0]
    if len(word.encode("utf-8")) < 2:
        return None
    count = 1
    trad = False
    if len(parts) >= 2 and parts[1].isdigit():
        count = max(1, int(parts[1]))
    if len(parts) >= 3 and parts[2] in ("1", "t", "T"):
        trad = True
    return code, word, count, trad


def merge_user_entries(lines):
    merged = {}
    for line in lines:
        parsed = parse_user_line(line)
        if not parsed:
            continue
        code, word, count, trad = parsed
        key = code, word, trad
        merged[key] = max(merged.get(key, 0), count)
    return merged


def require_contains(name, actual, expected, limit=8):
    head = actual[:limit]
    if expected not in head:
        raise SystemExit(f"{name}: expected {expected!r} in first {limit}, got {head!r}")


def require_equal(name, actual, expected):
    if actual != expected:
        raise SystemExit(f"{name}: expected {expected!r}, got {actual!r}")


def main():
    entries = load_seg_entries()
    predict = load_builtin_predict()

    cases = [
        ("full shuru", seg_candidates("shuru", entries), "输入"),
        ("full shurufa", seg_candidates("shurufa", entries), "输入法"),
        ("apostrophe xian", seg_candidates("xi'an", entries), "西安"),
        ("initial sr", initial_candidates("sr", entries), "输入"),
        ("initial srf", initial_candidates("srf", entries), "输入法"),
        ("initial lyjp", initial_candidates("lyjp", entries), "蓝牙键盘"),
    ]
    for name, actual, expected in cases:
        require_contains(name, actual, expected)

    require_equal("predict 我", predict["我"][:3], ["们", "的", "也"])
    require_equal("predict 输", predict["输"][:3], ["入", "出", "法"])

    merged = merge_user_entries([
        "shuru 输入 2",
        "shuru 输入 5",
        "shuru 輸入 3 1",
        "bad-count 词 x",
        "broken",
    ])
    require_equal("journal max count", merged[("shuru", "输入", False)], 5)
    require_equal("journal trad", merged[("shuru", "輸入", True)], 3)
    require_equal("journal malformed fallback", merged[("bad-count", "词", False)], 1)

    print("OK: IME behavior regression checks passed")


if __name__ == "__main__":
    main()
