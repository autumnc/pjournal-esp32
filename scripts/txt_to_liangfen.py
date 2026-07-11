#!/usr/bin/env python3
"""
Convert liangfen.txt to binary dictionary format for pjournal IME.

Binary format:
  [1354] index: 677 × uint16 LE (26*26+1 prefix boundary table)
  [N * 16] records, each:
    [12] code (null-padded)
    [3]  hanzi (UTF-8, 3 bytes)
    [1]  flag (0 = normal)

Index entry k gives the first record whose code >= 2-letter prefix k.
Index entry k+1 ends the range.  For 1-char codes: range = index[c0*26] to index[(c0+1)*26].
"""

import struct
import os
import bisect

INPUT = "/media/sf_share/liangfen.txt"
OUTPUT = os.path.join(os.path.dirname(__file__), "..", "main", "ime", "liangfen.bin")

RECORD_SIZE = 16
INDEX_ENTRIES = 26 * 26 + 1  # 677


def main():
    pairs = []

    with open(INPUT, "r", encoding="utf-8-sig") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            code = parts[0]
            # Keep only alphabetic chars
            code = "".join(c for c in code if c.isalpha())
            if not code:
                continue
            if len(code) > 12:
                code = code[:12]

            for ch in parts[1:]:
                if not ch:
                    continue
                pairs.append((code, ch))

    # Sort by code (ASCII/byte order for binary search)
    pairs.sort(key=lambda x: x[0])

    # Build 26*26+1 prefix boundary index
    index = [0] * INDEX_ENTRIES
    codes_list = [p[0] for p in pairs]  # sorted array for bisect

    for c0 in range(26):
        for c1 in range(26):
            k = c0 * 26 + c1
            prefix = chr(ord('a') + c0) + chr(ord('a') + c1)
            # Find first code >= prefix
            idx = bisect.bisect_left(codes_list, prefix)
            index[k] = idx

    # Sentinel: past the last prefix "zz"
    index[676] = len(pairs)

    # Write binary
    buf = bytearray()

    for k in range(INDEX_ENTRIES):
        buf.extend(struct.pack("<H", index[k]))

    for code, ch in pairs:
        # Code: 12 bytes null-padded
        code_bytes = code.encode("ascii")
        buf.extend(code_bytes.ljust(12, b'\x00')[:12])

        # Hanzi: up to 3 UTF-8 bytes
        ch_bytes = ch.encode("utf-8")
        if len(ch_bytes) > 3:
            print(f"Warning: '{ch}' > 3 UTF-8 bytes, truncating")
            ch_bytes = ch_bytes[:3]
        buf.extend(ch_bytes.ljust(3, b'\x00')[:3])

        buf.append(0)  # flag

    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    with open(OUTPUT, "wb") as f:
        f.write(buf)

    print(f"Liangfen dictionary: {len(pairs)} records -> {OUTPUT}")
    print(f"  Total size: {len(buf)} bytes ({len(buf)//1024} KB)")
    print(f"  Unique codes: {len(set(c for c,_ in pairs))}")

    # Verify
    for c0 in range(26):
        c1 = 0
        k = c0 * 26 + c1
        nxt = index[k + 26] if (c0 + 1) * 26 < INDEX_ENTRIES else len(pairs)
        lo = index[k]
        if lo < nxt:
            print(f"  Prefix '{chr(97+c0)}': range [{lo}, {nxt}) = {nxt-lo} records")
            break  # just first for validation

    print(f"  File size: {len(buf)} bytes")


if __name__ == "__main__":
    main()
