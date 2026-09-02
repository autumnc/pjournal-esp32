#!/usr/bin/env python3
"""
One-off: render markdown symbols at 16pt for main/symbol_glyphs.h.

- bullet(0x2022) checkbox(0x2610) check(0x2713) square(0x25A0): render at 16px
  from NF-Mono.ttf, advance forced to 8 (halfAdvance at 16pt).
- heading glyphs HL1-HL6 (uF03A4..uF03B3): render at ~29px for a 14-15px box,
  yo=14 (top at ascent, like 28pt/22pt where box top = ascent), advance 16
  (2 half-cells). Prints previews at both render sizes to pick the better one.

Do NOT fold into extract_symbols.py — it clobbers hand-applied icons.
"""
import freetype


def render_glyph(face, cp, pixel_size):
    face.set_pixel_sizes(0, pixel_size)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO
    face.load_char(cp, flags)
    bitmap = face.glyph.bitmap
    w = bitmap.width
    h = bitmap.rows
    xo = face.glyph.bitmap_left
    yo = face.glyph.bitmap_top
    adv = face.glyph.advance.x >> 6
    raw = bytes(bitmap.buffer)
    row_bytes = (w + 7) // 8
    out = bytearray()
    for r in range(h):
        start = r * bitmap.pitch
        out.extend(raw[start:start + row_bytes])
    return w, h, xo, yo, adv, bytes(out)


def emit(name, size, bits):
    print(f"static const uint8_t SYM_{name}_{size}_BITS[] = {{")
    line = "   "
    for i, b in enumerate(bits):
        line += f" 0x{b:02X},"
        if (i + 1) % 12 == 0:
            print(line)
            line = "   "
    if line.strip():
        print(line)
    print("};")
    print()


def preview(w, h, bits):
    lines = []
    rb = (w + 7) // 8
    for r in range(h):
        row = ""
        for c in range(w):
            byte = bits[r * rb + c // 8]
            on = (byte >> (7 - (c % 8))) & 1
            row += "#" if on else "."
        lines.append(row)
    return "\n".join(lines)


def main():
    ttf_path = "/media/sf_share/NF-Mono.ttf"
    face = freetype.Face(ttf_path)

    # --- small symbols at 16px ---
    glyphs = [(0x2022, "BULLET"), (0x2610, "CHECKBOX"),
              (0x2713, "CHECK"), (0x25A0, "SQUARE")]
    print("// --- 16px small symbols (advance forced to 8) ---")
    for cp, name in glyphs:
        w, h, xo, yo, adv, bits = render_glyph(face, cp, 16)
        emit(name, 16, bits)
        print(f"  # {name} U+{cp:04X}: {w}x{h} xo={xo} yo={yo} adv={adv} (forced 8)")
        print(preview(w, h, bits))
        print()

    # --- heading glyphs: render 29px -> 15x15 box, used at 16pt with yo=14, adv=16 ---
    hl = [(0xF03A4, "HL1"), (0xF03A7, "HL2"), (0xF03AA, "HL3"),
          (0xF03AD, "HL4"), (0xF03B1, "HL5"), (0xF03B3, "HL6")]
    print("// --- heading render 29px (15x15, yo=14, adv=16) ---")
    for cp, name in hl:
        w, h, xo, yo, adv, bits = render_glyph(face, cp, 29)
        assert (w, h) == (15, 15), f"{name}: got {w}x{h}"
        emit(name, 16, bits)
        print(f"  # {name} U+{cp:04X}: {w}x{h} xo={xo} yo={yo} adv={adv}")
    print()


if __name__ == '__main__':
    main()
