#!/usr/bin/env python3
# 用宋体(simsun.ttc)的基线数据校正 main/fontlibrary18.fnt 中非汉字字形的位置。
# 方式: 保留 FL18 原位图(与 CJK 笔画风格一致), 仅把宋体栅格化得到的基线
# 位置(yo, 墨底边缘相对基线)写进 meta —— 原位补丁, 不重建位图段。
# 背景: FontLibrary18.bin 的 ASCII/标点在格内垂直居中, 基线信息已丢。
import struct, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fnt_glyph_edit import Fnt

FONT = "/media/sf_share/simsun.ttc"
FNT = Path(__file__).resolve().parent.parent / "main" / "fontlibrary18.fnt"
SIZE = 18
THRESH = 128

KEEP_RANGES = [
    (0x2E80, 0x2FFF),   # 部首/康熙部首(字形如汉字)
    (0x3190, 0x319F),   # 汉字注记
    (0x31C0, 0x31EF),   # CJK 笔画
    (0x3400, 0x4DBF),   # CJK 扩展 A
    (0x4E00, 0x9FFF),   # CJK 统一汉字
    (0xA000, 0xA4CF),   # 彝文
    (0xF900, 0xFAFF),   # 兼容汉字
    (0xE000, 0xF8FF),   # PUA
]


def want(cp):
    if any(s <= cp <= e for s, e in KEEP_RANGES):
        return False
    return cp < 0x2E80 or 0x3000 <= cp <= 0x31BF or 0x3220 <= cp <= 0x33FF or cp >= 0xFE10


def baseline_yo(cps):
    """宋体栅格化, 返回 {cp: yo}。yo = 墨底边缘相对基线的偏移(坐基线=0, 下探为负)。"""
    f = ImageFont.truetype(FONT, SIZE, index=0)
    asc, desc = f.getmetrics()
    print(f"simsun metrics @ {SIZE}px: asc={asc} desc={desc}")
    out = {}
    for cp in cps:
        ch = chr(cp)
        img = Image.new("L", (SIZE * 3, asc + desc + 8), 0)
        d = ImageDraw.Draw(img)
        d.text((SIZE, asc), ch, font=f, fill=255, anchor="ls")
        px = img.load()
        ys = [y for y in range(img.height) for x in range(img.width) if px[x, y] >= THRESH]
        if not ys:
            continue
        y1 = max(ys) - asc            # 墨底行相对基线(向上为正)
        out[cp] = -(y1 + 1)           # 贴基线 yo=0; 下探1px yo=-1
    return out


def enumerate_cps(f):
    cps = set(range(0x21, 0x7F))
    for s, e, fm in f.blocks:
        for cp in range(s, e + 1):
            if fm + (cp - s) < f.gcount:
                cps.add(cp)
    return cps


def main():
    f = Fnt(FNT)
    tt = TTFont(FONT, fontNumber=0)
    cmap = set(tt.getBestCmap().keys())
    cps = sorted(cp for cp in enumerate_cps(f) if want(cp) and cp in cmap)
    print(f"候选: {len(cps)} 码点")
    yo_map = baseline_yo(cps)
    changed, unchanged, noink = 0, 0, 0
    for cp, yo in yo_map.items():
        idx = f.meta_idx(cp)
        if idx is None:
            continue
        o = f.m_off + idx * 12
        w, h = struct.unpack_from("<HH", f.b, o)
        if w == 0 or h == 0:
            noink += 1
            continue
        cur = struct.unpack_from("<b", f.b, o + 5)[0]
        if cur != yo:
            f.patch_meta(idx, yo=yo)
            changed += 1
        else:
            unchanged += 1
    f.save()
    print(f"基线校正 {changed} 个, 已正确 {unchanged} 个, 空字形跳过 {noink} 个")
    print(f"已写回 {FNT}")


if __name__ == "__main__":
    main()
