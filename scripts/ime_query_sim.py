#!/usr/bin/env python3
import argparse
import bisect
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TABLE = ROOT / "main" / "ime" / "ime_table_pinyin.bin"
DEFAULT_SEG = ROOT / "main" / "ime" / "seg_table_source.txt"
INDEX_ENTRIES = 26 * 26 + 1
HEADER_SIZE = 12


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def utf8_chars(text):
    return len(text)


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


class Ime3:
    def __init__(self, path):
        self.blob = Path(path).read_bytes()
        if len(self.blob) < HEADER_SIZE or self.blob[:4] != b"IME3":
            raise SystemExit(f"{path}: not an IME3 table")
        self.code_len = self.blob[5]
        self.single_count = u32(self.blob, 8)
        self.record_size = self.code_len + 4
        self.single_base = HEADER_SIZE + INDEX_ENTRIES * 4
        self.index = [u32(self.blob, HEADER_SIZE + i * 4) for i in range(INDEX_ENTRIES)]
        single_end = self.single_base + self.single_count * self.record_size
        self.word_count = u32(self.blob, single_end)
        word_index_base = single_end + 4
        self.word_index = [u32(self.blob, word_index_base + i * 4) for i in range(INDEX_ENTRIES)]
        self.word_base = word_index_base + INDEX_ENTRIES * 4
        self.word_size = self.word_index[-1]
        self.word_data = self.blob[self.word_base:self.word_base + self.word_size]

    def single_window(self, code):
        k = prefix_key(code)
        if k >= 26 * 26:
            return 0, self.single_count
        if len(code) == 1:
            return self.index[k], self.index[k + 26]
        return self.index[k], self.index[k + 1]

    def single_record(self, i):
        off = self.single_base + i * self.record_size
        code = self.blob[off:off + self.code_len].split(b"\0", 1)[0].decode()
        text = self.blob[off + self.code_len:off + self.code_len + 3].decode()
        flag = self.blob[off + self.code_len + 3]
        return code, text, flag

    def singles(self, code, limit):
        lo, hi = self.single_window(code)
        keys = [self.single_record(i)[0] for i in range(lo, hi)]
        pos = bisect.bisect_left(keys, code)
        out = []
        for i in range(lo + pos, hi):
            rec_code, text, flag = self.single_record(i)
            if not rec_code.startswith(code):
                break
            if flag & 0x02:
                continue
            if text not in out:
                out.append(text)
                if len(out) >= limit:
                    break
        return out

    def word_window(self, code):
        k = prefix_key(code)
        if k >= 26 * 26:
            return 0, self.word_size
        if len(code) == 1:
            return self.word_index[k], self.word_index[k + 26]
        return self.word_index[k], self.word_index[k + 1]

    def phrases(self, code, exact=False, limit=40):
        lo, hi = self.word_window(code)
        out = []
        pos = lo
        safety = 0
        while pos < hi and safety < 20000 and len(out) < limit:
            safety += 1
            cl = self.word_data[pos]
            if cl == 0 or pos + 1 + cl > hi:
                break
            rec_code = self.word_data[pos + 1:pos + 1 + cl].decode()
            pos += 1 + cl
            n = self.word_data[pos]
            pos += 1
            group_match = rec_code == code if exact else rec_code.startswith(code)
            for _ in range(n):
                wl = self.word_data[pos]
                pos += 1
                word = self.word_data[pos:pos + wl].decode()
                flag = self.word_data[pos + wl]
                pos += wl + 1
                if group_match and not (flag & 0x02) and word not in out:
                    out.append(word)
                    if len(out) >= limit:
                        break
            if rec_code > code and not rec_code.startswith(code):
                break
        return out


def load_seg(path):
    entries = []
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "\t" not in line:
            continue
        syllables, word = line.split("\t", 1)
        parts = syllables.split()
        code = "".join(parts)
        initial = "".join((s[:2] if s[:2] in ("zh", "ch", "sh") else s[:1]) for s in parts)
        compact = "".join(s[:1] for s in parts)
        entries.append((code, initial, compact, word.strip()))
    return entries


def seg_matches(entries, code, limit):
    scored = []
    for idx, (full, initial, compact, word) in enumerate(entries):
        if full.startswith(code):
            scored.append((0 if len(full) == len(code) else 2, len(full), idx, word))
        elif len(code) >= 2 and (initial.startswith(code) or compact.startswith(code)):
            m = min(len(x) for x in (initial, compact) if x.startswith(code))
            scored.append((1 if m == len(code) else 3, m, idx, word))
    out = []
    for _, _, _, word in sorted(scored):
        if word not in out:
            out.append(word)
            if len(out) >= limit:
                break
    return out


def query(table, seg_entries, code, limit):
    code = "".join(ch.lower() for ch in code if ch.isalpha())
    if not code:
        return []
    out = []
    def add_many(items):
        for item in items:
            if item not in out:
                out.append(item)
                if len(out) >= limit:
                    return
    if len(code) >= 4:
        add_many(table.phrases(code, exact=True, limit=8))
    add_many(table.singles(code, limit=16))
    add_many(seg_matches(seg_entries, code, limit=24))
    add_many(table.phrases(code, exact=False, limit=limit))
    return out[:limit]


def main():
    ap = argparse.ArgumentParser(description="Offline approximate IME candidate query.")
    ap.add_argument("codes", nargs="+", help="pinyin codes to inspect")
    ap.add_argument("--table", default=str(DEFAULT_TABLE))
    ap.add_argument("--seg", default=str(DEFAULT_SEG))
    ap.add_argument("--limit", type=int, default=20)
    args = ap.parse_args()

    table = Ime3(args.table)
    seg_entries = load_seg(args.seg)
    for code in args.codes:
        cands = query(table, seg_entries, code, args.limit)
        print(f"{code}: {' '.join(cands)}")


if __name__ == "__main__":
    main()
