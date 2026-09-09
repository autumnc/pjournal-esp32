#!/usr/bin/env python3
# 按白名单裁剪 PJFN 字体: 删除 中文/颜文字词库/固件界面 之外的字形, 节省 flash。
# 白名单 = KEEP_RANGES(整段) ∪ 固件源码全部非ASCII码点(含 \x/\u 转义解码) ∪
#          颜文字表 face 字符(main/ime/kaomoji_source.txt) ∪ 万象语料字符(/tmp/kaomoji_raw.txt)。
# 已有字形位图字节原样保留, 仅删 meta/索引; 重建后自校验保留字形零差异 + 词库全覆盖。
# 备份 <fnt>.pre-trim(存在则顺延 .pre-trim2/3...)。
import re, sys, glob
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fnt_glyph_edit import Fnt
from fnt_fill import rebuild, existing_glyphs

ROOT = Path(__file__).resolve().parent.parent

# 整段保留的区段(中文/标点/假名/全半角/固件 UI 符号, 单价低用途广)
KEEP_RANGES = [
    (0x20, 0x7E),      # ASCII
    (0xA0, 0xFF),      # Latin-1 (¥·× 与带音符字母)
    (0x300, 0x36F),    # 组合符号 (颜文字叠加)
    (0x2000, 0x206F),  # 一般标点 (…—''""•)
    (0x2190, 0x21FF),  # 箭头 (←↑→↓↔)
    (0x2500, 0x25FF),  # 制表+几何 (─│├└■▲▶▸▼▾◆○●◐)
    (0x2600, 0x26FF),  # 杂项符号 (★☐✓☆)
    (0x2700, 0x27BF),  # 丁贝符 (✎✧✦)
    (0x3000, 0x303F),  # CJK 符号 (、。〇《「」・)
    (0x3040, 0x30FF),  # 日文假名
    (0x31F0, 0x31FF),  # 假名补充
    (0x3400, 0x4DBF),  # CJK 扩展A
    (0x4E00, 0x9FFF),  # CJK 基本区
    (0xF900, 0xFAFF),  # 兼容表意 (繁体变体)
    (0xFE10, 0xFE4F),  # 竖排形式+中文兼容 (︑︱︵﹁ ﹃)
    (0xFF00, 0xFFEF),  # 半角/全角 (全角模式, v/bd, 半角假名)
]

JOIN = re.compile(r'"\s*"')
HEXRUN = re.compile(r'(?:\\x[0-9A-Fa-f]{2})+')
UESC = re.compile(r'\\u([0-9A-Fa-f]{4})')
NORM = {0x2002: 0x20, 0x2003: 0x20, 0x3000: 0x20}


def in_ranges(cp):
    return any(s <= cp <= e for s, e in KEEP_RANGES)


def firmware_cps():
    cps = set()
    for p in glob.glob(str(ROOT / "main" / "**" / "*.cpp"), recursive=True) + \
             glob.glob(str(ROOT / "main" / "**" / "*.h"), recursive=True):
        for line in open(p, encoding="utf-8", errors="replace"):
            merged = JOIN.sub('', line)
            for m in HEXRUN.finditer(merged):
                try:
                    cps.update(ord(c) for c in bytes.fromhex(m.group(0).replace("\\x", "")).decode("utf-8"))
                except Exception:
                    pass
            for m in UESC.finditer(merged):
                cps.add(int(m.group(1), 16))
            cps.update(ord(c) for c in line if ord(c) > 0x7E)
    return {c for c in cps if 0x20 < c <= 0x10FFFF and not 0xD800 <= c <= 0xDFFF}


def table_cps():
    cps = set()
    src = ROOT / "main" / "ime" / "kaomoji_source.txt"
    if src.exists():
        for line in src.read_text(encoding="utf-8").splitlines():
            if "\t" in line:
                cps.update(NORM.get(ord(c), ord(c)) for c in line.split("\t", 1)[1])
    return {c for c in cps if c != 0x20}


def corpus_cps():
    raw = Path("/tmp/kaomoji_raw.txt")
    if not raw.exists():
        return set()
    cps = set()
    for line in raw.read_text(encoding="utf-8").splitlines():
        if "\t" in line:
            cps.update(NORM.get(ord(c), ord(c)) for c in line.split("\t", 1)[1])
    return {c for c in cps if c != 0x20}


def main():
    keep_exact = firmware_cps() | table_cps() | corpus_cps()
    print(f"白名单: 区段内全部 + 精确 {len(keep_exact)} 个 (固件/词库/语料)")
    for path in sys.argv[1:]:
        target = Path(path)
        f = Fnt(target)
        old = existing_glyphs(f)
        n = 0
        while True:
            cand = target.with_suffix(target.suffix + (".pre-trim" if n == 0 else f".pre-trim{n + 1}"))
            if not cand.exists():
                bak = cand
                break
            n += 1
        bak.write_bytes(target.read_bytes())

        glyphs, dropped = {}, 0
        for cp, (w, h, xo, yo, adv, blob) in old.items():
            if in_ranges(cp) or cp in keep_exact:
                glyphs[cp] = (dict(w=w, h=h, xo=xo, yo=yo, adv=adv), blob)
            else:
                dropped += 1
        old_size = target.stat().st_size
        rebuild(f, glyphs)
        f.save()

        f2 = Fnt(target)
        new = existing_glyphs(f2)
        bad = sum(1 for cp, g in glyphs.items()
                  if cp not in new or new[cp][:5] != (g[0]["w"], g[0]["h"], g[0]["xo"], g[0]["yo"], g[0]["adv"]) or new[cp][5] != g[1])
        missing = [c for c in keep_exact if c not in new and not in_ranges(c)]
        comb_missing = [c for c in table_cps() if c not in new]
        print(f"{target.name}: {len(old)} -> {len(new)} (删 {dropped}), "
              f"{old_size} -> {target.stat().st_size} 字节 (省 {(old_size - target.stat().st_size) // 1024} KB), "
              f"校验不一致 {bad}")
        if comb_missing:
            print(f"  !! 词库字符仍缺 {len(comb_missing)}: " +
                  " ".join(f"U+{c:04X}" for c in sorted(comb_missing)[:40]))
        if missing:
            print(f"  白名单精确字符无字形(正常, 原本就没有): {len(missing)} 个")


if __name__ == "__main__":
    main()
