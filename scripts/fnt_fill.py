#!/usr/bin/env python3
# 补全 PJFN 字体中颜文字词库缺失的字符。
# 来源优先级: 宋体矢量(中文风格符号) -> DejaVu/Noto 矢量栅格 -> 另一字库位图缩放(兜底)。
# 缺失集 = 万象颜文字词库(/tmp/kaomoji_raw.txt)中未被目标字库覆盖的字符。
#
# 用法:
#   python3 scripts/fnt_fill.py main/terminus22.fnt --donor main/fontlibrary18.fnt          # 按表情词库补
#   python3 scripts/fnt_fill.py main/fontlibrary18.fnt --donor main/terminus22.fnt
#   python3 scripts/fnt_fill.py main/terminus22.fnt --donor main/fontlibrary18.fnt --all    # 全量补(所有本机字体能覆盖的符号区)
#
# 实现说明: 全量归一化重建 data/meta/blocks —— 已有字形的位图按字节原样搬运
# (data 段本就是 row-major MSB, 不重编码), 仅新增字形走栅格化/缩放。重建后
# 逐字形校验旧内容未变。原图备份为 <fnt>.pre-fill(存在则顺延 .pre-fill2/3...)。
import struct, sys, glob
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fnt_glyph_edit import Fnt

ROOT = Path(__file__).resolve().parent.parent
CORPUS = Path("/tmp/kaomoji_raw.txt")
SIMSUN = "/media/sf_share/simsun.ttc"
THRESH = 128
NORM = {0x2002: 0x20, 0x2003: 0x20, 0x3000: 0x20}

PRIO = [
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSansSymbols-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMath-Regular.ttf",
]


def combining(cp):
    return (0x300 <= cp <= 0x36F or 0x1AB0 <= cp <= 0x1AFF or
            0x1DC0 <= cp <= 0x1DFF or 0x20D0 <= cp <= 0x20FF or
            0xFE20 <= cp <= 0xFE2F)


# --all 模式尝试覆盖的区间: 拉丁补充~CJK符号/部首、卦、彝文、兼容表意、
# 竖排/_COMPAT/全角、平面1封闭字母, 以及 CJK 扩展 B(仅取本机字体有 cmap 的)。
def want_all(cp):
    if 0xD800 <= cp <= 0xDFFF or 0xE000 <= cp <= 0xF8FF:
        return False
    if 0xA0 <= cp <= 0x33FF:
        return True
    if 0x4DC0 <= cp <= 0x4DFF or 0xA000 <= cp <= 0xA4CF:
        return True
    if 0xF900 <= cp <= 0xFAFF or 0xFE10 <= cp <= 0xFE6F or 0xFF00 <= cp <= 0xFFEE:
        return True
    if 0x1F100 <= cp <= 0x1F1FF or 0x20000 <= cp <= 0x2A6DF:
        return True
    return False


def corpus_cps():
    cps = set()
    for line in CORPUS.read_text(encoding="utf-8").splitlines():
        if "\t" not in line:
            continue
        face = line.split("\t", 1)[1].strip()
        cps.update(NORM.get(ord(c), ord(c)) for c in face)
    cps.discard(0x20)
    return cps


def existing_glyphs(f):
    """{cp: (w,h,xo,yo,adv, bitmap_bytes)} —— 全部已有有效字形。"""
    out = {}
    for cp in range(0x20, 0x7F):
        idx = f.u32(f.a_off + (cp - 0x20) * 4)
        if idx != 0xFFFFFFFF:
            o = f.m_off + idx * 12
            w, h = struct.unpack_from("<HH", f.b, o)
            xo, yo = struct.unpack_from("<bb", f.b, o + 4)
            adv = f.b[o + 6]
            out[cp] = (w, h, xo, yo, adv, bytes(f.b[f.d_off + f.u32(o + 8):][: (w + 7) // 8 * h]))
    for s, e, fm in f.blocks:
        for cp in range(s, e + 1):
            idx = fm + (cp - s)
            if idx >= f.gcount:
                continue
            o = f.m_off + idx * 12
            w, h = struct.unpack_from("<HH", f.b, o)
            xo, yo = struct.unpack_from("<bb", f.b, o + 4)
            adv = f.b[o + 6]
            out[cp] = (w, h, xo, yo, adv, bytes(f.b[f.d_off + f.u32(o + 8):][: (w + 7) // 8 * h]))
    return out


def pack_rows(rows):
    w = len(rows[0])
    rb = (w + 7) // 8
    buf = bytearray()
    for r in rows:
        line = bytearray(rb)
        for c, v in enumerate(r):
            if v:
                line[c // 8] |= 0x80 >> (c % 8)
        buf += line
    return bytes(buf)


_font_cache = {}


def get_font(path, px):
    key = (str(path), px)
    if key not in _font_cache:
        _font_cache[key] = ImageFont.truetype(str(path), px)
    return _font_cache[key]


def raster(cp, fnt, px, asc):
    """矢量栅格化: 返回 (meta dict, rows) 或 None(无墨/渲染失败)。"""
    try:
        img = Image.new("L", (px * 2 + 8, px * 2 + 8), 0)
        d = ImageDraw.Draw(img)
        d.text((px, px * 2), chr(cp), font=fnt, fill=255, anchor="ls")
    except Exception:
        return None
    p = img.load()
    pts = [(x, y) for y in range(img.height) for x in range(img.width) if p[x, y] >= THRESH]
    if not pts:
        return None
    x0 = min(q[0] for q in pts); x1 = max(q[0] for q in pts)
    y0 = min(q[1] for q in pts); y1 = max(q[1] for q in pts)
    w, h = x1 - x0 + 1, y1 - y0 + 1
    rows = [[1 if p[x0 + c, y0 + r] >= THRESH else 0 for c in range(w)] for r in range(h)]
    try:
        adv = int(round(fnt.getlength(chr(cp))))
    except Exception:
        adv = w
    if combining(cp):
        adv = 0
    else:
        adv = max(w, min(px, adv))
    return dict(w=w, h=h, xo=x0 - px, yo=px * 2 - y1 - 1, adv=adv), rows


class VecSources:
    """候选矢量字体, 惰性加载 cmap(谁先覆盖用谁)。"""

    def __init__(self):
        paths = [p for p in PRIO if Path(p).exists()]
        seen = set(paths)
        for pat in ("/usr/share/fonts/noto/*.ttf", "/usr/share/fonts/TTF/*.ttf",
                    "/usr/share/fonts/OTF/*.otf"):
            for p in sorted(glob.glob(pat)):
                if p not in seen:
                    seen.add(p)
                    paths.append(p)
        for p in ("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                  "/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc"):
            if Path(p).exists() and p not in seen:
                paths.append(p)
        self.paths = paths
        self.cmaps = {}
        print(f"矢量候选 {len(paths)} 个字体")

    def cmap(self, p):
        cm = self.cmaps.get(p)
        if cm is None:
            try:
                cm = set(TTFont(p, fontNumber=0, lazy=True).getBestCmap().keys())
            except Exception:
                cm = set()
            self.cmaps[p] = cm
        return cm

    def pick(self, cp):
        for p in self.paths:
            if cp in self.cmap(p):
                return p
        return None

    def raster_first(self, cp, px, asc):
        """按优先级逐字体尝试栅格化, 返回第一个有墨的 (meta, rows, 源路径)。"""
        for p in self.paths:
            if cp not in self.cmap(p):
                continue
            try:
                fobj = get_font(p, px)
            except Exception:
                continue
            g = raster(cp, fobj, px, asc)
            if g:
                return g[0], g[1], p
        return None


def donor_glyph(cp, donor, t_px, t_asc):
    idx, g = donor.meta(cp)
    if g is None:
        return None
    rows = donor.bitmap(g)
    s = t_px / donor.line_h
    nw, nh = max(1, round(g["w"] * s)), max(1, round(g["h"] * s))
    out = [[rows[min(len(rows) - 1, int((r + 0.5) * len(rows) / nh))][min(len(rows[0]) - 1, int((c + 0.5) * len(rows[0]) / nw))]
            for c in range(nw)] for r in range(nh)]
    meta = dict(w=nw, h=nh, xo=round(g["xo"] * s), yo=round(g["yo"] * s),
                adv=max(1, round(g["adv"] * s)))
    return meta, out


def rebuild(f, glyphs):
    """glyphs: {cp: (meta_dict, bitmap_bytes)} -> 全量重建并写回。"""
    cps = sorted(glyphs)
    data = bytearray()
    bmos = []
    for cp in cps:
        bmos.append(len(data))
        data += glyphs[cp][1]
    idx_of = {cp: i for i, cp in enumerate(cps)}
    groups = []
    prev = None
    for cp in cps:
        if prev is not None and cp == prev + 1:
            groups[-1][1] = cp
        else:
            groups.append([cp, cp])
        prev = cp
    cjk = [(s, e, idx_of[s]) for s, e in groups if s >= 0x3400 and e <= 0x9FFF]
    oth = [(s, e, idx_of[s]) for s, e in groups if not (s >= 0x3400 and e <= 0x9FFF)]
    cjk_blob = struct.pack("<H", len(cjk)) + b"".join(struct.pack("<3I", *b) for b in cjk)
    oth_blob = struct.pack("<H", len(oth)) + b"".join(struct.pack("<3I", *b) for b in oth)
    meta_blob = b"".join(
        struct.pack("<HHbbBBI", glyphs[cp][0]["w"], glyphs[cp][0]["h"], glyphs[cp][0]["xo"],
                    glyphs[cp][0]["yo"], glyphs[cp][0]["adv"], 0, bmos[i])
        for i, cp in enumerate(cps))
    ascii_blob = b"".join(struct.pack("<I", idx_of.get(cp, 0xFFFFFFFF)) for cp in range(0x20, 0x7F))
    hdr = bytearray(f.b[:34])
    struct.pack_into("<H", hdr, 12, len(cps))
    pos = 34
    struct.pack_into("<I", hdr, 14, pos - 10); pos += len(ascii_blob)
    struct.pack_into("<I", hdr, 18, pos - 10); pos += len(cjk_blob)
    struct.pack_into("<I", hdr, 22, pos - 10); pos += len(meta_blob)
    struct.pack_into("<I", hdr, 26, pos - 10); pos += len(data)
    struct.pack_into("<I", hdr, 30, pos - 10)
    f.b = bytearray(bytes(hdr) + ascii_blob + cjk_blob + bytes(meta_blob) + bytes(data) + oth_blob)


def main():
    target = Path(sys.argv[1])
    donor_path = None
    if "--donor" in sys.argv:
        donor_path = Path(sys.argv[sys.argv.index("--donor") + 1])
    all_mode = "--all" in sys.argv
    replace_file = Path(sys.argv[sys.argv.index("--replace") + 1]) if "--replace" in sys.argv else None
    prefer = sys.argv[sys.argv.index("--prefer") + 1].split(",") if "--prefer" in sys.argv else []
    f = Fnt(target)
    px, asc = f.line_h, f.asc
    old = existing_glyphs(f)

    simsun_cmap = None
    if Path(SIMSUN).exists():
        simsun_cmap = set(TTFont(SIMSUN, fontNumber=0).getBestCmap().keys())
    donor = Fnt(donor_path) if donor_path and donor_path.exists() else None
    vec = VecSources()
    for p in prefer:
        vec.paths.insert(0, p)

    if all_mode:
        union = set(simsun_cmap) if simsun_cmap else set()
        for i, p in enumerate(vec.paths):
            union |= vec.cmap(p)
            if i % 100 == 0:
                print(f"cmap {i}/{len(vec.paths)} 并集 {len(union)}", flush=True)
        missing = sorted(cp for cp in union if want_all(cp) and cp not in old)
        print(f"{target.name}: line={f.line_h} asc={asc} 已有 {len(old)} 字形, 全量缺失 {len(missing)}")
    else:
        missing = sorted(cp for cp in corpus_cps() if cp not in old)
        print(f"{target.name}: line={f.line_h} asc={asc} 已有 {len(old)} 字形, 词库缺失 {len(missing)}")
    if not missing:
        print("无缺失, 退出")
        return
    if len(old) + len(missing) > 65000:
        before = len(missing)
        missing = [cp for cp in missing if cp < 0x20000]
        print(f"超 65000 上限, 去掉扩展B后缺失 {len(missing)} (原 {before})")
        if len(old) + len(missing) > 65000:
            print("!! 仍超上限, 退出")
            return

    bak = target
    n = 0
    while True:
        cand = target.with_suffix(target.suffix + (".pre-fill" if n == 0 else f".pre-fill{n + 1}"))
        if not cand.exists():
            bak = cand
            break
        n += 1
    bak.write_bytes(target.read_bytes())
    print(f"备份 -> {bak.name}")

    glyphs = {cp: (dict(w=w, h=h, xo=xo, yo=yo, adv=adv), blob)
              for cp, (w, h, xo, yo, adv, blob) in old.items()}

    if replace_file:
        r_cps = [int(t.lstrip("Uu+"), 16) if not t.isdigit() else int(t, 16)
                 for t in replace_file.read_text().split()]
        print(f"--replace: 重做 {len(r_cps)} 个码点(旧占位乱块先移除, 无源者删除)")
        for cp in r_cps:
            glyphs.pop(cp, None)
        replaced, dropped = 0, []
        for cp in r_cps:
            g = None
            for p in prefer:
                try:
                    if cp in vec.cmap(p):
                        g = raster(cp, get_font(p, px), px, asc)
                        if g:
                            break
                except Exception:
                    pass
            if g is None and simsun_cmap and cp in simsun_cmap:
                g = raster(cp, get_font(SIMSUN, px), px, asc)
            if g is None:
                rg = vec.raster_first(cp, px, asc)
                g = (rg[0], rg[1]) if rg else None
            if g is None and donor:
                g = donor_glyph(cp, donor, px, asc)
            if g is None:
                dropped.append(cp)
            else:
                glyphs[cp] = (g[0], pack_rows(g[1]))
                replaced += 1
        print(f"替换 {replaced} 个, 删除 {len(dropped)} 个: {' '.join(f'U+{c:04X}' for c in dropped)}")

    tally = {"simsun": 0, "vector": 0, "donor": 0}
    skipped = []
    for cp in missing:
        g = None
        if simsun_cmap and cp in simsun_cmap:
            g = raster(cp, get_font(SIMSUN, px), px, asc)
            if g:
                tally["simsun"] += 1
        if g is None:
            g = vec.raster_first(cp, px, asc)
            if g:
                g = (g[0], g[1])
                tally["vector"] += 1
        if g is None and donor:
            g = donor_glyph(cp, donor, px, asc)
            if g:
                tally["donor"] += 1
        if g is None:
            skipped.append(cp)
            continue
        meta, rows = g
        glyphs[cp] = (meta, pack_rows(rows))
    print(f"补全: 宋体 {tally['simsun']}, 矢量 {tally['vector']}, 位图缩放 {tally['donor']}, 跳过 {len(skipped)}")
    if skipped:
        if len(skipped) <= 60:
            print("跳过: " + " ".join(f"{chr(c)}(U+{c:04X})" for c in skipped))
        else:
            print(f"跳过(前60): " + " ".join(f"{chr(c)}(U+{c:04X})" for c in skipped[:60]))

    rebuild(f, glyphs)
    f.save()

    # 校验: 重新加载, 旧字形逐一比对位图与 meta
    f2 = Fnt(target)
    new = existing_glyphs(f2)
    bad = 0
    for cp, (w, h, xo, yo, adv, blob) in old.items():
        cur = new.get(cp)
        if cur is None or cur[:5] != (w, h, xo, yo, adv) or cur[5] != blob:
            bad += 1
            if bad <= 5:
                print(f"  校验失败 U+{cp:04X}")
    print(f"校验: 旧字形 {len(old)} 个, 不一致 {bad} 个; 新总数 {len(new)} (文件 {target.stat().st_size} 字节)")
    if bad:
        print("!! 有不一致, 请检查备份", target.with_suffix(target.suffix + ".pre-fill"))


if __name__ == "__main__":
    main()
