#!/usr/bin/env python3
"""
Convert embedded font-library .bin (direct-mapped bitmap cell format) to BDF.

File layout (FontLibrary18.bin / 华文中宋20.bin):
  - ASCII 0x20-0x7E: halfwidth cells, column-major, LSB-first:
      pixel(r, c) = data[off + c*3 + r//8] >> (r%8) & 1
      FL18: off = 27*(cp-1), 9 cols x 18 rows, 27 B/cell
      hw20: off = 30*(cp-1), 10 cols x 20 rows, 30 B/cell
  - Everything else: one 54/60-byte cell per codepoint, direct mapped
      unit index e = cp - 65, off = e * unit
      FL18: 18 cols x 18 rows (54 B), hw20: 20 cols x 20 rows (60 B)
      File spans cp 0x41..0xFFFF; empty cells = no glyph.
"""
import sys

class Font:
    def __init__(self, path, rows, half_cols, full_cols, unit,
                 ascii_off, rows_bytes=3):
        self.data = open(path, 'rb').read()
        self.rows = rows
        self.half_cols = half_cols
        self.full_cols = full_cols
        self.unit = unit
        self.ascii_off = ascii_off  # offset function for cp 0x20-0x7E
        self.rb = rows_bytes
        self.n_units = len(self.data) // unit
        self.glyphs = {}  # cp -> (bitmap rows list-of-lists, advance)

    def _decode(self, off, cols):
        rows, rb = self.rows, self.rb
        g = [[0] * cols for _ in range(rows)]
        for c in range(cols):
            for r in range(rows):
                if (self.data[off + c * rb + r // 8] >> (r % 8)) & 1:
                    g[r][c] = 1
        return g

    def extract(self):
        for cp in range(0x20, 0x7F):
            self.glyphs[cp] = (self._decode(self.ascii_off(cp), self.half_cols),
                               self.half_cols)
        for cp in range(0xA1, 0x10000):
            if 0xD800 <= cp <= 0xDFFF:
                continue
            e = cp - 65
            if e >= self.n_units:
                break
            g = self._decode(e * self.unit, self.full_cols)
            if not any(any(row) for row in g):
                continue
            self.glyphs[cp] = (g, self.full_cols)
        return self

    def write_bdf(self, out_path, ascent, descent, family):
        R = self.rows
        lines = []
        w_max = max(self.full_cols, self.half_cols)
        lines.append('STARTFONT 2.1')
        lines.append(f'FONT -{family}-Medium-R-Normal--{R}-{w_max}-72-72-P-0-ISO10646-1')
        lines.append(f'SIZE {R} 72 72')
        lines.append(f'FONTBOUNDINGBOX {w_max} {R} 0 {descent - R}')
        lines.append('STARTPROPERTIES 4')
        lines.append(f'FONT_ASCENT {ascent}')
        lines.append(f'FONT_DESCENT {descent}')
        lines.append('DEFAULT_CHAR 32')
        lines.append('CHARSET_REGISTRY ISO10646')
        lines.append('ENDPROPERTIES')
        lines.append(f'CHARS {len(self.glyphs)}')
        for cp in sorted(self.glyphs):
            g, adv = self.glyphs[cp]
            ink = [(r, c) for r in range(R) for c in range(len(g[0])) if g[r][c]]
            if ink:
                r0 = min(r for r, _ in ink); r1 = max(r for r, _ in ink)
                c0 = min(c for _, c in ink); c1 = max(c for _, c in ink)
                w, h = c1 - c0 + 1, r1 - r0 + 1
                x, y = c0, (ascent - 1) - r1
            else:
                w = h = x = y = 0
            rb = (w + 7) // 8
            lines.append(f'STARTCHAR uni{cp:04X}')
            lines.append(f'ENCODING {cp}')
            sw = adv * 1000 // R
            lines.append(f'SWIDTH {sw}')
            lines.append(f'DWIDTH {adv}')
            lines.append(f'BBX {w} {h} {x} {y}')
            lines.append('BITMAP')
            for r in range(r0, r1 + 1) if ink else []:
                row = 0
                for c in range(c0, c1 + 1):
                    row = (row << 1) | g[r][c]
                row <<= (rb * 8 - w)
                lines.append(f'{row:0{rb * 2}X}')
            lines.append('ENDCHAR')
        lines.append('ENDFONT')
        with open(out_path, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        print(f"{out_path}: {len(self.glyphs)} glyphs")


def main():
    fonts = [
        ('/media/sf_share/FontLibrary18.bin', '/media/sf_share/FontLibrary18.bdf',
         dict(rows=18, half_cols=9, full_cols=18, unit=54,
              ascii_off=lambda cp: 27 * (cp - 1), ascent=15, descent=3,
              family='FontLibrary18')),
        ('/media/sf_share/FontLibrary18B.bin', '/media/sf_share/FontLibrary18B.bdf',
         dict(rows=18, half_cols=9, full_cols=18, unit=54,
              ascii_off=lambda cp: 27 * (cp - 1), ascent=15, descent=3,
              family='FontLibrary18B')),
        ('/media/sf_share/华文中宋20.bin', '/media/sf_share/华文中宋20.bdf',
         dict(rows=20, half_cols=10, full_cols=20, unit=60,
              ascii_off=lambda cp: 30 * (cp - 1), ascent=18, descent=2,
              family='STZhongsong20')),
    ]
    for src, dst, kw in fonts:
        Font(src, **{k: v for k, v in kw.items()
                     if k not in ('ascent', 'descent', 'family')}) \
            .extract() \
            .write_bdf(dst, kw['ascent'], kw['descent'], kw['family'])


if __name__ == '__main__':
    main()
