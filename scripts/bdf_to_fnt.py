#!/usr/bin/env python3
"""
BDF to pjournal font format converter.
Converts TerminusSongMedium-28.bdf to a compact binary format (.fnt).

Output binary format:
  [4] magic "PJFN"
  [2] header_size (always 24)
  [2] line_height (pixels)
  [2] ascent
  [2] descent
  [2] glyph_count
  [4] ascii_offset  (offset to ASCII direct table, or 0 if none)
  [4] cjk_offset    (offset to CJK block table)
  [4] meta_offset   (offset to glyph metadata array)
  [4] data_offset   (offset to bitmap data)

ASCII direct table (at ascii_offset):
  95 entries × 4 bytes each = uint32 offsets into meta array, index 0 = codepoint 0x20

CJK block table (at cjk_offset):
  [2] block_count
  For each block:
    [4] start_cp (unicode codepoint)
    [4] end_cp
    [4] first_meta_idx (index into meta array)

Glyph metadata (at meta_offset):
  For each glyph (glyph_count entries):
    [2] width
    [2] height
    [1] x_offset (signed)
    [1] y_offset (signed)
    [1] advance
    [1] pad
    [4] bitmap_offset (relative to data_offset start)

Bitmap data (at data_offset):
  For each glyph: width×height bits, row-padded to byte boundary.
"""

import struct
import sys
import os

def parse_bdf(bdf_path):
    glyphs = {}  # encoding -> {width, height, x_off, y_off, advance, bitmap}
    with open(bdf_path, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    props = {}
    i = 0
    while i < len(lines):
        l = lines[i].strip()
        if l.startswith('FONT_ASCENT'):
            props['ascent'] = int(l.split()[1])
        elif l.startswith('FONT_DESCENT'):
            props['descent'] = abs(int(l.split()[1]))
        elif l == 'ENDPROPERTIES':
            break
        i += 1

    # Parse glyphs
    i = 0
    while i < len(lines):
        l = lines[i].strip()
        if l.startswith('STARTCHAR'):
            encoding = None
            bbx = None
            dwidth = None
            bitmap_lines = []
            i += 1
            # Read until ENDCHAR
            while i < len(lines):
                l2 = lines[i].strip()
                if l2.startswith('ENCODING'):
                    encoding = int(l2.split()[1])
                elif l2.startswith('DWIDTH'):
                    dwidth = int(l2.split()[1])
                elif l2.startswith('BBX'):
                    parts = l2.split()
                    bbx = (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))
                elif l2.startswith('BITMAP'):
                    i += 1
                    while i < len(lines) and not lines[i].strip().startswith('ENDCHAR'):
                        hex_str = lines[i].strip()
                        if hex_str:
                            bitmap_lines.append(hex_str)
                        i += 1
                    break
                i += 1

            if encoding is not None and bbx is not None:
                w, h, xo, yo = bbx
                # Parse bitmap hex data
                bitmap = bytearray()
                for hl in bitmap_lines:
                    val = int(hl, 16)
                    for shift in range((len(hl)*4) - 8, -8, -8):
                        if shift >= 0:
                            bitmap.append((val >> shift) & 0xFF)
                        else:
                            bitmap.append(0)

                # Determine advance width from DWIDTH (14 for ASCII, 28 for CJK)
                advance = dwidth if dwidth is not None else bbx[0]

                glyphs[encoding] = {
                    'width': w,
                    'height': h,
                    'x_off': xo,
                    'y_off': yo,
                    'advance': advance,
                    'bitmap': bytes(bitmap),
                }
        i += 1

    return glyphs, props

def convert_bdf_to_fnt(bdf_path, output_path):
    glyphs, props = parse_bdf(bdf_path)
    ascent = props.get('ascent', 22)
    descent = props.get('descent', 6)
    line_height = ascent + descent

    # Separate ASCII and CJK
    ascii_glyphs = {}
    cjk_glyphs = []
    other_glyphs = []

    for cp in sorted(glyphs.keys()):
        g = glyphs[cp]
        if 0x20 <= cp <= 0x7E:
            ascii_glyphs[cp] = g
        elif 0x3400 <= cp <= 0x9FFF:
            cjk_glyphs.append((cp, g))
        else:
            other_glyphs.append((cp, g))

    all_glyphs = []

    # ASCII in codepoint order (0x20-0x7E)
    ascii_offsets = []
    for cp in range(0x20, 0x7F):
        if cp in ascii_glyphs:
            ascii_offsets.append(len(all_glyphs))
            all_glyphs.append(ascii_glyphs[cp])
        else:
            ascii_offsets.append(0xFFFF)  # missing glyph

    # CJK sorted by codepoint (blocks)
    cjk_start = len(all_glyphs)
    for cp, g in cjk_glyphs:
        all_glyphs.append(g)

    # Other glyphs
    for cp, g in other_glyphs:
        all_glyphs.append(g)

    glyph_count = len(all_glyphs)

    # Build bitmap data
    bitmap_data = bytearray()
    bitmap_offsets = []
    for g in all_glyphs:
        bw = g['width']
        bh = g['height']
        row_bytes = (bw + 7) // 8
        expected_size = row_bytes * bh
        raw = g['bitmap']

        bitmap_offsets.append(len(bitmap_data))

        if len(raw) >= expected_size:
            bitmap_data.extend(raw[:expected_size])
        else:
            bitmap_data.extend(raw)
            bitmap_data.extend(b'\x00' * (expected_size - len(raw)))

    # Build CJK block table
    cjk_blocks = []
    idx = cjk_start
    for cp, g in cjk_glyphs:
        if not cjk_blocks or cjk_blocks[-1][1] + 1 != cp:
            cjk_blocks.append([cp, cp, idx])
        else:
            cjk_blocks[-1][1] = cp
        idx += 1

    # Build "other" block table (non-ASCII, non-CJK glyphs)
    other_start = len(all_glyphs) - len(cjk_glyphs) - len([o for o in ascii_offsets if o != 0xFFFF])
    # Actually other_start is just cjk_start + len(cjk_glyphs) = start of other_glyphs in all_glyphs
    other_meta_start = cjk_start + len(cjk_glyphs)
    other_blocks = []
    idx = other_meta_start
    for cp, g in other_glyphs:
        if not other_blocks or other_blocks[-1][1] + 1 != cp:
            other_blocks.append([cp, cp, idx])
        else:
            other_blocks[-1][1] = cp
        idx += 1

    # Write output
    buf = bytearray()
    # Header
    buf.extend(b'PJFN')
    header_size = struct.pack('<H', 24)
    buf.extend(header_size)
    buf.extend(struct.pack('<H', line_height))
    buf.extend(struct.pack('<H', ascent))
    buf.extend(struct.pack('<H', descent))
    buf.extend(struct.pack('<H', glyph_count))

    # Calculate offsets relative to byte 24 (reference point for hdr_adj=6 in C++)
    base = 24
    ascii_offset = base
    ascii_table_size = 95 * 4
    cjk_offset = ascii_offset + ascii_table_size
    cjk_block_count = len(cjk_blocks)
    cjk_table_size = 2 + cjk_block_count * 12
    meta_offset = cjk_offset + cjk_table_size
    meta_size = glyph_count * 12
    data_offset = meta_offset + meta_size
    other_block_count = len(other_blocks)
    other_table_size = 2 + other_block_count * 12
    other_offset = data_offset + len(bitmap_data)

    buf.extend(struct.pack('<I', ascii_offset))
    buf.extend(struct.pack('<I', cjk_offset))
    buf.extend(struct.pack('<I', meta_offset))
    buf.extend(struct.pack('<I', data_offset))
    buf.extend(struct.pack('<I', other_offset))

    # ASCII direct table
    for off in ascii_offsets:
        buf.extend(struct.pack('<I', off if off != 0xFFFF else 0xFFFFFFFF))

    # CJK block table
    buf.extend(struct.pack('<H', cjk_block_count))
    for start_cp, end_cp, first_meta in cjk_blocks:
        buf.extend(struct.pack('<III', start_cp, end_cp, first_meta))

    # Glyph metadata
    for i, g in enumerate(all_glyphs):
        buf.extend(struct.pack('<H', g['width']))
        buf.extend(struct.pack('<H', g['height']))
        buf.extend(struct.pack('<b', g['x_off']))
        buf.extend(struct.pack('<b', g['y_off']))
        buf.extend(struct.pack('<B', g['advance']))
        buf.extend(b'\x00')  # pad
        buf.extend(struct.pack('<I', bitmap_offsets[i]))

    # Bitmap data
    buf.extend(bitmap_data)

    # Other block table (non-ASCII, non-CJK glyph lookup)
    buf.extend(struct.pack('<H', other_block_count))
    for start_cp, end_cp, first_meta in other_blocks:
        buf.extend(struct.pack('<III', start_cp, end_cp, first_meta))

    with open(output_path, 'wb') as f:
        f.write(buf)

    print(f"Converted {bdf_path} -> {output_path}")
    print(f"  Glyphs: {glyph_count} (ASCII:{len([o for o in ascii_offsets if o != 0xFFFF])} "
          f"CJK:{len(cjk_glyphs)} Other:{len(other_glyphs)})")
    print(f"  Total size: {len(buf)} bytes")
    print(f"  Line height: {line_height}, Ascent: {ascent}, Descent: {descent}")

if __name__ == '__main__':
    import sys
    bdf_path = sys.argv[1] if len(sys.argv) > 1 else "/media/sf_share/Terminus/TerminusSongMedium-28.bdf"
    output_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "main", "terminus28.fnt")
    convert_bdf_to_fnt(bdf_path, os.path.abspath(output_path))
