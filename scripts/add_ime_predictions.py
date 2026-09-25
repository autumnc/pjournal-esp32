#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path


INDEX_ENTRIES = 26 * 26 + 1
HEADER_SIZE = 12
HANZI_SIZE = 3
FLAG_SIZE = 1


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def utf8_char_count(text):
    return len(text)


def parse_source(path):
    groups = {}
    order = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "\t" in line:
            key, rest = line.split("\t", 1)
            words = rest.replace("\t", " ").split()
        else:
            parts = line.split()
            if len(parts) < 2:
                continue
            key, words = parts[0], parts[1:]
        key = key.strip()
        if not 1 <= utf8_char_count(key) <= 4:
            raise SystemExit(f"bad key length: {key!r}")
        if key not in groups:
            groups[key] = []
            order.append(key)
        for word in words:
            if not 1 <= utf8_char_count(word) <= 4:
                raise SystemExit(f"bad candidate length for {key!r}: {word!r}")
            if word not in groups[key]:
                groups[key].append(word)
        groups[key] = groups[key][:16]
    return [(key, groups[key]) for key in order if groups[key]]


def ime3_word_data_end(blob):
    if len(blob) < HEADER_SIZE or blob[:4] != b"IME3":
        raise SystemExit("not an IME3 table")
    code_len = blob[5]
    single_count = u32(blob, 8)
    rec_size = code_len + HANZI_SIZE + FLAG_SIZE
    single_base = HEADER_SIZE + INDEX_ENTRIES * 4
    single_end = single_base + single_count * rec_size
    if single_end + 4 + INDEX_ENTRIES * 4 > len(blob):
        raise SystemExit("IME3 table has no phrase section")
    word_index_base = single_end + 4
    word_data_base = word_index_base + INDEX_ENTRIES * 4
    word_data_size = u32(blob, word_index_base + (INDEX_ENTRIES - 1) * 4)
    word_data_end = word_data_base + word_data_size
    if word_data_end > len(blob):
        raise SystemExit("bad IME3 word data size")
    return word_data_end


def build_predict_blob(groups):
    out = bytearray()
    out += struct.pack("<I", len(groups))
    for key, words in groups:
        kb = key.encode("utf-8")
        out += kb
        out.append(len(words))
        for word in words:
            wb = word.encode("utf-8")
            if len(wb) > 255:
                raise SystemExit(f"candidate too long: {word!r}")
            out.append(len(wb))
            out += wb
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description="Append/replace IME3 prediction section.")
    ap.add_argument("--table", default="main/ime/ime_table_pinyin.bin")
    ap.add_argument("--source", default="main/ime/predict_source.txt")
    ap.add_argument("--output", default=None)
    args = ap.parse_args()

    table = Path(args.table)
    source = Path(args.source)
    out_path = Path(args.output) if args.output else table

    blob = table.read_bytes()
    base_end = ime3_word_data_end(blob)
    groups = parse_source(source)
    pred = build_predict_blob(groups)
    out_path.write_bytes(blob[:base_end] + pred)
    print(f"wrote {out_path}: {len(groups)} predict groups, {len(pred)} bytes")


if __name__ == "__main__":
    main()
