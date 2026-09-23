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


def main():
    ap = argparse.ArgumentParser(description="Inspect pjournal IME3 table layout.")
    ap.add_argument("path", nargs="?", default="main/ime/ime_table_pinyin.bin")
    args = ap.parse_args()

    blob = Path(args.path).read_bytes()
    if len(blob) < HEADER_SIZE or blob[:4] != b"IME3":
        raise SystemExit("not an IME3 table")

    scheme = blob[4]
    code_len = blob[5]
    single_count = u32(blob, 8)
    rec_size = code_len + HANZI_SIZE + FLAG_SIZE
    single_base = HEADER_SIZE + INDEX_ENTRIES * 4
    single_end = single_base + single_count * rec_size
    print(f"file={args.path}")
    print(f"size={len(blob)} scheme={scheme} code_len={code_len}")
    print(f"single_count={single_count} single_bytes={single_count * rec_size}")

    if single_end + 4 > len(blob):
        print("word_section=missing")
        return

    word_count = u32(blob, single_end)
    word_index_base = single_end + 4
    word_data_base = word_index_base + INDEX_ENTRIES * 4
    if word_count <= 0 or word_data_base > len(blob):
        print("word_section=empty")
        return

    word_index = [u32(blob, word_index_base + i * 4) for i in range(INDEX_ENTRIES)]
    word_data_size = word_index[-1]
    word_data_end = word_data_base + word_data_size
    print(f"word_count={word_count} word_data_size={word_data_size}")

    nonempty = []
    for i in range(INDEX_ENTRIES - 1):
        span = word_index[i + 1] - word_index[i]
        if span > 0:
            nonempty.append((span, i))
    nonempty.sort(reverse=True)
    print(f"word_windows_nonempty={len(nonempty)}")
    for span, i in nonempty[:20]:
        a = chr(ord("a") + i // 26)
        b = chr(ord("a") + i % 26)
        print(f"  {a}{b}: {span} bytes")

    if word_data_end + 4 <= len(blob):
        remain = blob[word_data_end:]
        padded = all(c in (0x00, 0xFF) for c in remain)
        print(f"tail_bytes={len(remain)} tail_padding={padded}")
        if not padded:
            pred_count = u32(blob, word_data_end)
            print(f"predict_count={pred_count} predict_bytes={len(blob) - word_data_end - 4}")
        else:
            print("predict_section=missing")


if __name__ == "__main__":
    main()
