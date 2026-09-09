#!/usr/bin/env python3
"""
TTF/OTF -> BDF rasterizer for fixed-size CJK bitmap fonts.

Renders each codepoint with FreeType in monochrome mode (hinted), optionally
applying a directional bold that preserves songti contrast:
  down       union with the bitmap shifted 1px down   (thin horizontals 1->2px)
  downright  down, then union with shift 1px right    (verticals +1px too)
  heavy      down, right, down again                  (very bold, can clog)

Example:
  ttf2bdf.py /media/sf_share/simsun.ttc /media/sf_share/simsun20.bdf --px 20 --bold down
"""
import argparse
import collections

import freetype as ft


def render(face, cp, px):
    face.set_pixel_sizes(0, px)
    face.load_char(chr(cp), ft.FT_LOAD_RENDER | ft.FT_LOAD_TARGET_MONO)
    bm = face.glyph.bitmap
    if bm.rows == 0 or bm.width == 0:
        return None
    if bm.pixel_mode == ft.FT_PIXEL_MODE_MONO:
        bits = [[(bm.buffer[r * bm.pitch + (c >> 3)] >> (7 - (c & 7))) & 1
                 for c in range(bm.width)] for r in range(bm.rows)]
    else:
        bits = [[1 if bm.buffer[r * bm.pitch + c] > 127 else 0
                 for c in range(bm.width)] for r in range(bm.rows)]
    adv = (face.glyph.advance.x + 32) // 64
    return bits, max(1, adv)


def union_down(bits):
    w = max(len(r) for r in bits)
    out = [[0] * w for _ in range(len(bits) + 1)]
    for r, row in enumerate(bits):
        row = row + [0] * (w - len(row))
        for c in range(w):
            if row[c]:
                out[r][c] = out[r + 1][c] = 1
    return out[:-1] if not any(out[-1]) else out


def union_right(bits):
    out = []
    for row in bits:
        nr = list(row) + [0]
        for i, v in enumerate(row):
            if v:
                nr[i + 1] = 1
        out.append(nr)
    return out


def union_right_light(bits):
    # 只把 1px 宽的竖向结构加宽到 2px：h-run==1 的像素才补右侧一位。
    # 已是 2px 的竖笔保持 2px，横笔(h-run=长度)厚度不变 —— 任何字源都统一成 横1纵2。
    w = max(len(r) for r in bits)
    out = [list(r) + [0] * (w + 1 - len(r)) for r in bits]
    for r, row in enumerate(bits):
        c = 0
        while c < len(row):
            if row[c]:
                c0 = c
                while c < len(row) and row[c]:
                    c += 1
                if c - c0 == 1:
                    out[r][c0 + 1] = 1
            else:
                c += 1
    return out


def apply_bold(bits, mode):
    if mode == 'down':
        return union_down(bits)
    if mode == 'right':
        return union_right(bits)
    if mode == 'right1':
        return union_right_light(bits)
    if mode == 'downright':
        return union_right(union_down(bits))
    if mode == 'heavy':
        return union_down(union_right(union_down(bits)))
    return bits


def crop_origin(bits):
    rows = [i for i, r in enumerate(bits) if any(r)]
    if not rows:
        return None, 0, 0
    cols = [c for c in range(len(bits[0])) if any(r[c] for r in bits)]
    return [r[cols[0]:cols[-1] + 1] for r in bits[rows[0]:rows[-1] + 1]], cols[0], rows[0]


def ref_codepoints(path):
    cps = []
    cur = None
    for line in open(path):
        if line.startswith('ENCODING '):
            cps.append(int(line.split()[1]))
    return cps


def measure_metrics(face, px):
    # baseline: bottom ink row of 'A' + 1
    face.set_pixel_sizes(0, px)
    face.load_char('A', ft.FT_LOAD_RENDER | ft.FT_LOAD_TARGET_MONO)
    bm = face.glyph.bitmap
    bottom = 0
    for r in range(bm.rows):
        row = bm.buffer[r * bm.pitch:(r + 1) * bm.pitch]
        if any(row):
            bottom = r
    return bottom + 1 if bottom else int(px * 0.8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('font')
    ap.add_argument('out')
    ap.add_argument('--px', type=int, required=True)
    ap.add_argument('--bold', choices=['none', 'down', 'right', 'right1', 'downright', 'heavy'],
                    default='none')
    ap.add_argument('--face-index', type=int, default=0)
    ap.add_argument('--wght', type=float, default=None,
                    help='variable font wght coordinate, e.g. 200')
    ap.add_argument('--cps-from', default=None,
                    help='BDF whose ENCODING list defines the charset')
    args = ap.parse_args()

    face = ft.Face(args.font, args.face_index)
    if args.wght is not None:
        face.set_var_design_coords([args.wght])
    ascent = measure_metrics(face, args.px)
    descent = args.px - ascent

    if args.cps_from:
        cps = ref_codepoints(args.cps_from)
    else:
        cps = list(range(0x20, 0x7F)) + list(range(0xA1, 0x10000))

    glyphs = {}
    for cp in cps:
        if 0xD800 <= cp <= 0xDFFF:
            continue
        try:
            res = render(face, cp, args.px)
        except Exception:
            res = None
        if not res:
            continue
        bits, adv = res
        if args.bold != 'none':
            bits = apply_bold(bits, args.bold)
        body, x_off, y_top = crop_origin(bits)
        if body is None:
            if cp == 0x20:
                glyphs[cp] = ([], 0, 0, 0, adv)
            continue
        glyphs[cp] = (body, x_off, y_top, 0, adv)

    lines = []
    lines.append('STARTFONT 2.1')
    lines.append(f"FONT -simsun-Medium-R-Normal--{args.px}-72-72-P-0-ISO10646-1")
    lines.append(f'SIZE {args.px} 72 72')
    lines.append(f'FONTBOUNDINGBOX {args.px} {args.px} 0 {descent - args.px}')
    lines.append('STARTPROPERTIES 4')
    lines.append(f'FONT_ASCENT {ascent}')
    lines.append(f'FONT_DESCENT {descent}')
    lines.append('DEFAULT_CHAR 32')
    lines.append('CHARSET_REGISTRY ISO10646')
    lines.append('ENDPROPERTIES')
    lines.append(f'CHARS {len(glyphs)}')
    for cp in sorted(glyphs):
        body, x_off, y_top, _, adv = glyphs[cp]
        h = len(body)
        w = len(body[0]) if body else 0
        y_off = (ascent - 1) - (y_top + h - 1)
        rb = (w + 7) // 8
        lines.append(f'STARTCHAR uni{cp:04X}')
        lines.append(f'ENCODING {cp}')
        lines.append(f'SWIDTH {adv * 1000 // args.px}')
        lines.append(f'DWIDTH {adv}')
        lines.append(f'BBX {w} {h} {x_off} {y_off}')
        lines.append('BITMAP')
        for r in body:
            v = 0
            for b in r:
                v = (v << 1) | b
            v <<= (rb * 8 - w)
            lines.append(f'{v:0{rb * 2}X}')
        lines.append('ENDCHAR')
    lines.append('ENDFONT')
    with open(args.out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"{args.out}: {len(glyphs)} glyphs, asc={ascent} desc={descent}")


if __name__ == '__main__':
    main()
