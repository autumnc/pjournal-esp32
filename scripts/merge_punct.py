#!/usr/bin/env python3
"""Replace punctuation/other glyphs in GB18030 BDF with those from Medium BDF."""

import sys

def parse_bdf(bdf_path):
    glyphs = {}
    props_lines = []
    post_glyph_lines = []
    in_props = False
    in_glyphs = False
    chars_count = 0

    with open(bdf_path, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    i = 0
    while i < len(lines):
        l = lines[i]
        s = l.strip()
        if s == 'STARTPROPERTIES':
            in_props = True
            props_lines.append(l)
            i += 1
            continue
        if in_props:
            props_lines.append(l)
            if s == 'ENDPROPERTIES':
                in_props = False
            i += 1
            continue
        if s.startswith('CHARS '):
            chars_count = int(s.split()[1])
            props_lines.append(l)
            i += 1
            continue

        if s.startswith('STARTCHAR'):
            in_glyphs = True
            encoding = None
            glyph_lines = [l]
            i += 1
            while i < len(lines):
                l2 = lines[i]
                s2 = l2.strip()
                glyph_lines.append(l2)
                if s2.startswith('ENCODING'):
                    encoding = int(s2.split()[1])
                elif s2 == 'ENDCHAR':
                    break
                i += 1
            if encoding is not None:
                glyphs[encoding] = glyph_lines
            i += 1
        else:
            if not in_glyphs:
                props_lines.append(l)
            else:
                post_glyph_lines.append(l)
            i += 1

    return glyphs, props_lines, post_glyph_lines, chars_count


def main():
    gb18030_path = "/media/sf_share/点阵20-24/20/TerminusSong-GB18030-20.bdf"
    medium_path = "/media/sf_share/点阵20-24/20/TerminusSongMedium-20.bdf"
    output_path = "/media/sf_share/点阵20-24/20/TerminusSong-GB18030-20.bdf"

    gb18030_glyphs, header, trailer, _ = parse_bdf(gb18030_path)
    medium_glyphs, _, _, _ = parse_bdf(medium_path)

    # Replace non-ASCII, non-CJK glyphs with Medium versions
    replaced = 0
    for cp in sorted(gb18030_glyphs.keys()):
        if 0x20 <= cp <= 0x7E:
            continue  # keep ASCII
        if 0x3400 <= cp <= 0x9FFF:
            continue  # keep CJK
        if cp in medium_glyphs:
            gb18030_glyphs[cp] = medium_glyphs[cp]
            replaced += 1

    # Write output
    with open(output_path, 'w', encoding='utf-8') as f:
        for l in header:
            f.write(l)
        for cp in sorted(gb18030_glyphs.keys()):
            for l in gb18030_glyphs[cp]:
                f.write(l)
        for l in trailer:
            f.write(l)

    print(f"Replaced {replaced} glyphs (non-ASCII, non-CJK)")
    print(f"Output: {output_path}")


if __name__ == '__main__':
    main()
