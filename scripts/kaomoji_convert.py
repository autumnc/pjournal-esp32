#!/usr/bin/env python3
# 生成 main/ime/kaomoji_table.h(v键颜文字词库)。
#
# 数据源: 万象拼音颜文字词库(中文关键词\t颜文字)
#   https://raw.githubusercontent.com/Battery-rar/rime-wanxiang-kaomoji/main/data/kaomoji.txt
#
# 两种模式:
#   1) 全流程: python3 kaomoji_convert.py --raw kaomoji.txt
#      原始词库 -> 拼音编码 + 双字体(16/22pt)字形覆盖过滤 + 宽度过滤
#      -> 写精简源 main/ime/kaomoji_source.txt(编码\t颜文字)
#   2) 默认: python3 kaomoji_convert.py
#      从 main/ime/kaomoji_source.txt 重新生成头文件(无需 pypinyin/网络)
#
# 头文件内容: 词条表(常用块在前) + 裸v常用条数 + 拼音音节表(按长度降序,
# 固件端贪心切分后做 声母/全拼前缀匹配, kx 匹配 kaixin) + 中文标点(biaodian)。
import sys, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "main"
SRC = MAIN / "ime" / "kaomoji_source.txt"
OUT = MAIN / "ime" / "kaomoji_table.h"

# 裸 v 常用块: 关键词按此顺序取首个颜文字,去重后最多 18 条
HOT_KEYWORDS = ["微笑", "开心", "高兴", "快乐", "可爱", "卖萌", "喜欢", "爱心", "比心",
                "加油", "难过", "伤心", "生气", "哭泣", "委屈", "无语", "尴尬", "无奈",
                "疑惑", "惊讶", "害羞", "睡觉", "晚安", "再见", "谢谢", "吃饭", "哈哈"]
HOT_LIMIT = 18
PER_CODE_LIMIT = 15     # 每个编码最多词条数
TOTAL_LIMIT = 8000
MAX_BYTES = 32          # 颜文字 UTF-8 字节数上限
MAX_WIDTH = 300         # 渲染像素宽度上限(两种字号都要求)

# 中文标点(原 IME V_PUNCT_CANDIDATES, 编码 biaodian, v/bd/ 声母可搜)
PUNCT = ["，", "。", "、", "？", "！", "：", "；", "…", "……", "—", "——", "·",
         "“", "”", "“”", "‘", "’", "‘’", "「", "」", "「」", "『", "』", "『』",
         "（", "）", "（）", "【", "】", "【】", "［", "］", "［］", "〔", "〕", "〔〕",
         "｛", "｝", "｛｝", "《", "》", "《》", "〈", "〉", "〈〉", "〖", "〗", "〖〗",
         "〝", "〞", "〝〞",
         "．", "／", "＼", "～", "｜", "＃", "＠", "＆", "％", "＊", "＋", "－", "＝",
         "＿", "＾", "＄", "＜", "＞"]

# 词库中的特殊空格统一成 ASCII 空格(字体无这些字形, 全角空格 22pt 走 symbol 表但 16pt 缺)
NORM = {0x2002: 0x20, 0x2003: 0x20, 0x3000: 0x20}


def normalize(face):
    return "".join(chr(NORM.get(ord(c), ord(c))) for c in face)


class Font:
    """解析 PJFN 字体 blob, 提供 codepoint 覆盖检查与 advance 读取。
    与 font_renderer.cpp parseBlob/findGlyph 对齐(hdr_adj=10)。"""

    def __init__(self, path):
        b = path.read_bytes()
        assert b[:4] == b"PJFN", path
        self.b = b
        self.glyph_count = b[12] | (b[13] << 8)
        adj = 10
        self.meta_off = struct.unpack_from("<I", b, 22)[0] + adj
        self.ascii = struct.unpack_from("<95I", b, struct.unpack_from("<I", b, 14)[0] + adj)
        self.blocks = []
        for off_field in (18, 30):  # cjk_off, other_off
            off = struct.unpack_from("<I", b, off_field)[0] + adj
            n = struct.unpack_from("<H", b, off)[0]
            for i in range(n):
                self.blocks.append(struct.unpack_from("<3I", b, off + 2 + i * 12))

    def meta_idx(self, cp):
        if 0x20 <= cp <= 0x7E:
            idx = self.ascii[cp - 0x20]
            return None if idx == 0xFFFFFFFF else idx
        for s, e, fm in self.blocks:
            if s <= cp <= e:
                idx = fm + (cp - s)
                return idx if idx < self.glyph_count else None
        return None

    def has(self, cp):
        return self.meta_idx(cp) is not None

    def advance(self, cp):
        idx = self.meta_idx(cp)
        # GlyphMeta{u16 w, u16 h, i8 x, i8 y, u8 advance, u32 off} -> advance 偏移 6
        return self.b[self.meta_off + idx * 12 + 6] if idx is not None else 0


def utf8_cps(s):
    return [ord(c) for c in s]


# 组合字符(叠字符号)在字库里 advance=0 属正常叠加行为, 不视为缺字形
COMBINING_RANGES = ((0x300, 0x36F), (0x1AB0, 0x1AFF), (0x1DC0, 0x1DFF),
                    (0x20D0, 0x20FF), (0xFE20, 0xFE2F))


def is_combining(cp):
    return any(s <= cp <= e for s, e in COMBINING_RANGES)


def width(font, face):
    w = 0
    for cp in utf8_cps(face):
        a = font.advance(cp)
        if a == 0 and not is_combining(cp):
            return -1
        w += a
    return w


def load_fonts():
    d = MAIN
    return (Font(d / "fontlibrary18.fnt"), Font(d / "terminus22.fnt"))


def build_entries():
    """读原始词库, 返回 [(code, face)] 有序词条 + 音节集合。"""
    from pypinyin import lazy_pinyin
    raw = Path(sys.argv[sys.argv.index("--raw") + 1]).read_text(encoding="utf-8").splitlines()
    f16, f22 = load_fonts()
    seen_pairs, entries, sylls = set(), [], set()
    hot_faces, hot_used = [], set()

    def code_of(kw):
        toks = ["".join(ch for ch in t if "a" <= ch <= "z") for t in lazy_pinyin(kw)]
        toks = [t for t in toks if t]
        code = "".join(toks).lower()
        return code, toks

    def pass_filter(face):
        if not face or len(face.encode("utf-8")) > MAX_BYTES:
            return False
        w16, w22 = width(f16, face), width(f22, face)
        return 0 < w16 <= MAX_WIDTH and 0 < w22 <= MAX_WIDTH

    def add(code, toks, face):
        if (code, face) in seen_pairs:
            return
        seen_pairs.add((code, face))
        entries.append((code, face))
        sylls.update(toks)

    for line in raw:
        if "\t" not in line:
            continue
        kw, face = line.split("\t", 1)
        kw, face = kw.strip(), normalize(face.strip())
        if not kw or not pass_filter(face):
            continue
        code, toks = code_of(kw)
        if not code or len(code) > 16:
            continue
        if kw in HOT_KEYWORDS and kw not in hot_used and len(hot_faces) < HOT_LIMIT \
                and face not in hot_faces:
            hot_used.add(kw)
            hot_faces.append(face)
        add(code, toks, face)

    # 常用块置顶(按 HOT_KEYWORDS 顺序)
    ordered = [(None, f) for f in hot_faces]
    per_code = {}
    for code, face in entries:
        if per_code.get(code, 0) >= PER_CODE_LIMIT:
            continue
        per_code[code] = per_code.get(code, 0) + 1
        ordered.append((code, face))
        if len(ordered) >= TOTAL_LIMIT + HOT_LIMIT:
            break
    return ordered, sylls


def load_compact():
    """从精简源重建词条; 音节表由词条贪心切分推导。"""
    entries = []
    for line in SRC.read_text(encoding="utf-8").splitlines():
        if "\t" not in line:
            continue
        code, face = line.split("\t", 1)
        entries.append((code, face))
    # 常用块无编码(None), 编码沿用词条; 贪心切分音节
    sylls = set()
    for code, _ in entries:
        if not code:
            continue
        i = 0
        while i < len(code):
            for L in (6, 5, 4, 3, 2, 1):
                if code[i:i + L] in KNOWN_SYLLS:
                    sylls.add(code[i:i + L])
                    i += L
                    break
            else:
                sylls.add(code[i])
                i += 1
    return entries, sylls


KNOWN_SYLLS = set("""a ai an ang ao ba bai ban bang bao bei ben beng bi bian biao bie bin bing bo bu
ca cai can cang cao ce cen ceng cha chai chan chang chao che chen cheng chi chong chou chu chua
chuai chuan chuang chui chun chuo ci cong cou cu cuan cui cun cuo da dai dan dang dao de dei den
deng di dia dian diao die ding diu dong dou du duan dui dun duo e ei en eng er fa fan fang fei fen
feng fo fou fu ga gai gan gang gao ge gei gen geng gong gou gu gua guai guan guang gui gun guo ha
hai han hang hao he hei hen heng hong hou hu hua huai huan huang hui hun huo ji jia jian jiang
jiao jie jin jing jiong jiu ju juan jue jun ka kai kan kang kao ke kei ken keng kong kou ku kua
kuai kuan kuang kui kun kuo la lai lan lang lao le lei leng li lia lian liang liao lie lin ling
liu lo long lou lu luan lun luo lv lue ma mai man mang mao me mei men meng mi mian miao mie min
ming miu mo mou mu mu na nai nan nang nao ne nei nen neng ni nian niang niao nie nin ning niu nong
nou nu nuan nuo nv nue o ou pa pai pan pang pao pei pen peng pi pian piao pie pin ping po pou pu
qi qia qian qiang qiao qie qin qing qiong qiu qu quan que qun ran rang rao re ren reng ri rong rou
ru rua rui run ruo sa sai san sang sao se sen seng sha shai shan shang shao she shei shen sheng
shi shou shu shua shuai shuan shuang shui shun shuo si song sou su suan sui sun suo ta tai tan
tang tao te teng ti tian tiao tie ting tong tou tu tuan tui tun tuo wa wai wan wang wei wen weng
wo wu xi xia xian xiang xiao xie xin xing xiong xiu xu xuan xue xun ya yan yang yao ye yi yin ying
yo yong you yu yuan yue yun za zai zan zang zao ze zei zen zeng zha zhai zhan zhang zhao zhe zhen
zheng zhi zhong zhou zhu zhua zhuai zhuan zhuang zhui zhun zhuo zi zong zou zu zuan zui zun zuo
n ng er y w ei ou""".split())


def emit(entries, sylls, hot_count):
    # 顺序: 常用块(无编码, 搜索跳过) -> 标点块(v/bd/ 等声母查询时标点排最前) -> 其余
    hot = [e for e in entries if not e[0]]
    rest = [e for e in entries if e[0]]
    punct = [("biaodian", p) for p in PUNCT]
    all_entries = hot + punct + rest
    syll_list = sorted(sylls, key=lambda s: (-len(s), s))

    def cstr(s):
        out = []
        for ch in s.encode("utf-8"):
            if ch in (0x22, 0x5C):
                out.append("\\" + chr(ch))
            elif 0x20 <= ch < 0x7F:
                out.append(chr(ch))
            else:
                out.append("\\%03o" % ch)
        return '"%s"' % "".join(out)

    lines = [
        "// 由 scripts/kaomoji_convert.py 生成 — 勿手改",
        "// 数据源: 万象拼音颜文字词库(中文关键词转拼音编码, 已按 16/22pt 字形覆盖过滤)",
        "#pragma once",
        "struct KaomojiEntry { const char *code; const char *face; };",
        "static const KaomojiEntry K_KAOMOJI_TABLE[] = {",
    ]
    for code, face in all_entries:
        lines.append("    {%s, %s}," % (cstr(code or ""), cstr(face)))
    lines += [
        "};",
        "static const unsigned K_KAOMOJI_COUNT = %d;" % len(all_entries),
        "static const unsigned K_KAOMOJI_HOT = %d;  // 裸v直接展示的常用条数" % hot_count,
        "static const char *K_KAOMOJI_SYLLS[] = {",
    ]
    lines.append("    " + ", ".join(cstr(s) for s in syll_list) + ",")
    lines += [
        "};",
        "static const unsigned K_KAOMOJI_SYLL_COUNT = %d;" % len(syll_list),
        "",
    ]
    OUT.write_text("\n".join(lines), encoding="utf-8")
    print("wrote %s: %d entries (hot %d, %d syllables)" %
          (OUT, len(all_entries), hot_count, len(syll_list)))


def main():
    if "--raw" in sys.argv:
        entries, sylls = build_entries()
        hot = sum(1 for c, _ in entries if not c)
        with SRC.open("w", encoding="utf-8") as f:
            for code, face in entries:
                f.write("%s\t%s\n" % (code or "", face))
        print("wrote %s: %d entries" % (SRC, len(entries)))
    else:
        entries, sylls = load_compact()
        hot = sum(1 for c, _ in entries if not c)
    emit(entries, sylls, hot)


if __name__ == "__main__":
    main()
