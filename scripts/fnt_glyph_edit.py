#!/usr/bin/env python3
# PJFN .fnt 字形查看/手动编辑工具。
#
# 用法:
#   show    <fnt> <char|cp>                 查看字形(位图+meta)
#   export  <fnt> <char|cp> <art.txt>       导出字形为 ASCII 画稿供手改
#   apply   <fnt> <char|cp> <art.txt> [--xo N --yo N --adv N]
#                                           用画稿替换字形(重建 data 段与全部偏移)
#   meta    <fnt> <char|cp> [--xo N --yo N --adv N]
#                                           只改偏移/步进(原位补丁, 不动位图)
#   fixlatin <fnt>                          按类别规则把 0x21-0x7E 归位到基线
#
# 画稿格式: '#'=墨点 '.'=空, 每行等宽; 行数=高度, 最长行=宽度。
# 坐标约定(与固件 drawGlyph 一致): y 为基线(baseline), 字形墨底边缘 = 基线 - yo;
# 即 yo=0 表示字形坐在基线上, yo=-2 表示下探 2px(下降部)。
import struct, sys
from pathlib import Path

ADJ = 10  # parseBlob 的 hdr_adj: 文件内存储偏移 = 实际位置 - 10


class Fnt:
    def __init__(self, path):
        self.path = Path(path)
        self.b = bytearray(self.path.read_bytes())
        assert self.b[:4] == b"PJFN"
        self.line_h = self.b[6] | self.b[7] << 8
        self.asc = self.b[8] | self.b[9] << 8
        self.desc = self.b[10] | self.b[11] << 8
        self.gcount = self.b[12] | self.b[13] << 8
        self.a_off = self.u32(14) + ADJ
        self.c_off = self.u32(18) + ADJ
        self.m_off = self.u32(22) + ADJ
        self.d_off = self.u32(26) + ADJ
        self.o_off = self.u32(30) + ADJ
        self.blocks = []
        for off in (self.c_off, self.o_off):
            n = self.u16(off)
            for i in range(n):
                self.blocks.append(struct.unpack_from("<3I", self.b, off + 2 + i * 12))

    def u16(self, off):
        return self.b[off] | self.b[off + 1] << 8

    def u32(self, off):
        return struct.unpack_from("<I", self.b, off)[0]

    def meta_idx(self, cp):
        if 0x20 <= cp <= 0x7E:
            idx = self.u32(self.a_off + (cp - 0x20) * 4)
            return None if idx == 0xFFFFFFFF else idx
        for s, e, fm in self.blocks:
            if s <= cp <= e:
                idx = fm + (cp - s)
                return idx if idx < self.gcount else None
        return None

    def meta(self, cp):
        idx = self.meta_idx(cp)
        if idx is None:
            return None, None
        o = self.m_off + idx * 12
        w, h = struct.unpack_from("<HH", self.b, o)
        xo, yo = struct.unpack_from("<bb", self.b, o + 4)
        adv = self.b[o + 6]
        bmo = self.u32(o + 8)
        return idx, dict(w=w, h=h, xo=xo, yo=yo, adv=adv, bmo=bmo)

    def bitmap(self, g):
        rb = (g["w"] + 7) // 8
        rows = []
        for r in range(g["h"]):
            row = [(self.b[self.d_off + g["bmo"] + r * rb + c // 8] >> (7 - c % 8)) & 1
                   for c in range(g["w"])]
            rows.append(row)
        return rows

    def patch_meta(self, idx, xo=None, yo=None, adv=None):
        o = self.m_off + idx * 12
        if xo is not None:
            struct.pack_into("<b", self.b, o + 4, xo)
        if yo is not None:
            struct.pack_into("<b", self.b, o + 5, yo)
        if adv is not None:
            self.b[o + 6] = adv

    def rebuild(self, bitmaps, metas):
        """bitmaps: list of rows(0/1) per glyph; metas: list of dict(w,h,xo,yo,adv).
        重建 data 段并重写 meta 的 bmo 与文件内全部偏移(other 段移到 data 后)。"""
        data = bytearray()
        bmos = []
        for rows in bitmaps:
            bmos.append(len(data))
            rb = (len(rows[0]) + 7) // 8
            for r in rows:
                line = bytearray(rb)
                for c, v in enumerate(r):
                    if v:
                        line[c // 8] |= 0x80 >> (c % 8)
                data += line
        self.gcount = len(metas)
        meta_blob = bytearray()
        for m, bmo in zip(metas, bmos):
            meta_blob += struct.pack("<HHbbBBI", m["w"], m["h"], m["xo"], m["yo"], m["adv"], 0, bmo)
        cjk = [b for b in self.blocks[:self.u16(self.c_off)]]
        oth = [b for b in self.blocks[self.u16(self.c_off):]]
        hdr = bytearray(self.b[:34])
        pos = 34
        struct.pack_into("<I", hdr, 14, pos - ADJ)
        pos += 95 * 4
        struct.pack_into("<I", hdr, 18, pos - ADJ)
        cjk_blob = struct.pack("<H", len(cjk)) + b"".join(struct.pack("<3I", *b) for b in cjk)
        pos += len(cjk_blob)
        struct.pack_into("<I", hdr, 22, pos - ADJ)
        pos += len(meta_blob)
        struct.pack_into("<I", hdr, 26, pos - ADJ)
        pos += len(data)
        struct.pack_into("<I", hdr, 30, pos - ADJ)
        oth_blob = struct.pack("<H", len(oth)) + b"".join(struct.pack("<3I", *b) for b in oth)
        struct.pack_into("<H", hdr, 12, self.gcount)
        out = bytes(hdr) + self.b[self.a_off:self.c_off + len(cjk_blob)] + bytes(meta_blob) + bytes(data) + oth_blob
        # ascii 段原样(34..a_off+380), cjk 段原样, meta/data/other 重建
        self.b = bytearray(out)
        self.d_off = self.u32(26) + ADJ
        self.o_off = self.u32(30) + ADJ

    def save(self):
        self.path.write_bytes(self.b)


def resolve(f, key):
    cp = ord(key) if len(key) == 1 else int(key, 0)
    idx, g = f.meta(cp)
    if g is None:
        sys.exit(f"U+{cp:04X} 无字形")
    return cp, idx, g


def cmd_show(f, key):
    cp, idx, g = resolve(f, key)
    print(f"U+{cp:04X} '{chr(cp)}' idx={idx} w={g['w']} h={g['h']} xo={g['xo']} yo={g['yo']} adv={g['adv']} (line={f.line_h} asc={f.asc})")
    for r, row in enumerate(f.bitmap(g)):
        print(f"  {r:2d} " + "".join("#" if v else "." for v in row))


def read_art(path):
    lines = [l.rstrip("\n") for l in Path(path).read_text().splitlines()]
    while lines and not lines[-1].strip():
        lines.pop()
    w = max(len(l) for l in lines)
    rows = [[1 if c < len(l) and l[c] == "#" else 0 for c in range(w)] for l in lines]
    return rows


def cmd_apply(f, key, art, xo=None, yo=None, adv=None):
    cp, idx, g = resolve(f, key)
    rows = read_art(art)
    print(f"U+{cp:04X}: {g['w']}x{g['h']} -> {len(rows[0])}x{len(rows)}")
    metas, bitmaps = [], []
    for i in range(f.gcount):
        o = f.m_off + i * 12
        w, h = struct.unpack_from("<HH", f.b, o)
        x_, y_ = struct.unpack_from("<bb", f.b, o + 4)
        a_ = f.b[o + 6]
        if i == idx:
            w, h = len(rows[0]), len(rows)
            x_ = g["xo"] if xo is None else xo
            y_ = g["yo"] if yo is None else yo
            a_ = g["adv"] if adv is None else adv
            bitmaps.append(rows)
        else:
            bitmaps.append(f.bitmap(dict(w=w, h=h, bmo=f.u32(o + 8))))
        metas.append(dict(w=w, h=h, xo=x_, yo=y_, adv=a_))
    f.rebuild(bitmaps, metas)
    f.save()
    print("已重建并写回(位图段全部重排)")


def cmd_meta(f, key, xo=None, yo=None, adv=None):
    cp, idx, g = resolve(f, key)
    f.patch_meta(idx, xo, yo, adv)
    f.save()
    _, g2 = f.meta(cp)
    print(f"U+{cp:04X}: xo={g2['xo']} yo={g2['yo']} adv={g2['adv']}")


def cmd_export(f, key, art):
    cp, idx, g = resolve(f, key)
    with open(art, "w") as fp:
        fp.write(f"# U+{cp:04X} '{chr(cp)}' w={g['w']} h={g['h']} xo={g['xo']} yo={g['yo']} adv={g['adv']}\n")
        for row in f.bitmap(g):
            fp.write("".join("#" if v else "." for v in row) + "\n")
    print(f"已导出 {art} (改 '#'/'.' 画稿, 头部行可改 xo/yo/adv)")


# fixlatin: 0x21-0x7E 按类别锚定。源字库 ASCII 是在 18px 格内垂直居中的,
# 需统一到基线: 墨底边缘坐在基线行(15) → yo=0; 引号类顶到 cap 高(行4);
# 括号/斜杠类下探到行17; 下划线贴行18; 数学符居 x-height 中部。
TOP = set("'\"`*^~")
MID = set("-+=<>")
FULL = set("/\\|()[]{}")
UNDER = set("_")
DESC = set("gjpqy")


def fix_yo(cp, g):
    ch = chr(cp)
    if ch in UNDER:
        return -3
    if ch in FULL:
        return -(3 if g["h"] >= 15 else 2)
    if ch in TOP:
        return f_asc - 4 - g["h"]
    if ch in MID:
        return f_asc - (10 + g["h"]) + (1 if g["h"] % 2 else 0)
    return 0  # 基线类: 大写/数字/小写/句读/!?#%&$@


f_asc = 15


def cmd_fixlatin(f):
    global f_asc
    f_asc = f.asc
    changed = []
    for cp in range(0x21, 0x7F):
        idx, g = f.meta(cp)
        if g is None:
            continue
        yo = fix_yo(cp, g)
        if yo != g["yo"]:
            f.patch_meta(idx, yo=yo)
            changed.append((chr(cp), g["yo"], yo))
    f.save()
    print(f"调整 {len(changed)} 个字形: " + " ".join(f"{c}:{a}->{b}" for c, a, b in changed))


def main():
    a = sys.argv[1:]
    cmd, rest = a[0], a[1:]
    if cmd == "fixlatin":
        f = Fnt(rest[0])
        cmd_fixlatin(f)
        return
    f = Fnt(rest[0])
    if cmd == "show":
        cmd_show(f, rest[1])
    elif cmd == "export":
        cmd_export(f, rest[1], rest[2])
    elif cmd == "apply":
        args = [x for x in rest[3:]]
        kw = {}
        for name in ("--xo", "--yo", "--adv"):
            if name in args:
                i = args.index(name)
                kw[name[2:]] = int(args[i + 1])
                del args[i:i + 2]
        cmd_apply(f, rest[1], rest[2], **kw)
    elif cmd == "meta":
        args = rest[2:]
        kw = {}
        for name in ("--xo", "--yo", "--adv"):
            if name in args:
                i = args.index(name)
                kw[name[2:]] = int(args[i + 1])
                del args[i:i + 2]
        cmd_meta(f, rest[1], **kw)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
