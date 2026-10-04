#!/usr/bin/env python3
"""Build pjournal IME resource deltas from Wanxiang/Rime-style dictionaries.

The importer is intentionally conservative: by default it writes separate
delta files and leaves the firmware resources untouched. Use --apply after
reviewing the deltas.
"""

import argparse
import re
import unicodedata
from collections import OrderedDict, defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SEG_SOURCE = ROOT / "main" / "ime" / "seg_table_source.txt"
PREDICT_SOURCE = ROOT / "main" / "ime" / "predict_source.txt"
KAOMOJI_SOURCE = ROOT / "main" / "ime" / "kaomoji_source.txt"
DEFAULT_SEG_DELTA = ROOT / "main" / "ime" / "wanxiang_seg_delta.txt"
DEFAULT_PREDICT_DELTA = ROOT / "main" / "ime" / "wanxiang_predict_delta.txt"
DEFAULT_SYMBOL_DELTA = ROOT / "main" / "ime" / "wanxiang_symbol_delta.txt"

PINYIN_SYLLABLES = set("""
a ai an ang ao ba bai ban bang bao bei ben beng bi bian biao bie bin bing bo bu
ca cai can cang cao ce cen ceng cha chai chan chang chao che chen cheng chi chong
chou chu chua chuai chuan chuang chui chun chuo ci cong cou cu cuan cui cun cuo
da dai dan dang dao de dei den deng di dia dian diao die ding diu dong dou du duan
dui dun duo e ei en eng er fa fan fang fei fen feng fo fou fu ga gai gan gang gao
ge gei gen geng gong gou gu gua guai guan guang gui gun guo ha hai han hang hao
he hei hen heng hong hou hu hua huai huan huang hui hun huo ji jia jian jiang jiao
jie jin jing jiong jiu ju juan jue jun ka kai kan kang kao ke kei ken keng kong
kou ku kua kuai kuan kuang kui kun kuo la lai lan lang lao le lei leng li lia lian
liang liao lie lin ling liu lo long lou lu luan lun luo lv lue ma mai man mang mao
me mei men meng mi mian miao mie min ming miu mo mou mu na nai nan nang nao ne nei
nen neng ni nian niang niao nie nin ning niu nong nou nu nuan nuo nv nue o ou pa
pai pan pang pao pei pen peng pi pian piao pie pin ping po pou pu qi qia qian qiang
qiao qie qin qing qiong qiu qu quan que qun ran rang rao re ren reng ri rong rou ru
rua rui run ruo sa sai san sang sao se sen seng sha shai shan shang shao she shei
shen sheng shi shou shu shua shuai shuan shuang shui shun shuo si song sou su suan
sui sun suo ta tai tan tang tao te teng ti tian tiao tie ting tong tou tu tuan tui
tun tuo wa wai wan wang wei wen weng wo wu xi xia xian xiang xiao xie xin xing
xiong xiu xu xuan xue xun ya yan yang yao ye yi yin ying yo yong you yu yuan yue
yun za zai zan zang zao ze zei zen zeng zha zhai zhan zhang zhao zhe zhen zheng zhi
zhong zhou zhu zhua zhuai zhuan zhuang zhui zhun zhuo zi zong zou zu zuan zui zun
zuo lve nve n ng
""".split())

BASE_PUNCT = [
    "，", "。", "、", "？", "！", "：", "；", "…", "……", "—", "——", "·",
    "“", "”", "“”", "‘", "’", "‘’", "「", "」", "「」", "『", "』", "『』",
    "（", "）", "（）", "【", "】", "【】", "［", "］", "［］", "〔", "〕", "〔〕",
    "｛", "｝", "｛｝", "《", "》", "《》", "〈", "〉", "〈〉", "〖", "〗", "〖〗",
    "〝", "〞", "〝〞", "．", "／", "＼", "～", "｜", "＃", "＠", "＆", "％",
    "＊", "＋", "－", "＝", "＿", "＾", "＄", "＜", "＞",
]


def is_cjk_char(ch):
    cp = ord(ch)
    return (
        0x3400 <= cp <= 0x4DBF or
        0x4E00 <= cp <= 0x9FFF or
        0xF900 <= cp <= 0xFAFF
    )


def is_cjk_word(text):
    return bool(text) and all(is_cjk_char(ch) for ch in text)


def is_ascii_code(text):
    text = text.strip().lower().lstrip("/")
    return bool(text) and all("a" <= ch <= "z" for ch in text)


def normalize_symbol_code(code):
    code = code.strip().lower().lstrip("/")
    code = re.sub(r"[^a-z]+", "", code)
    return code


def is_supported_symbol_char(ch):
    cp = ord(ch)
    if cp in (0xFE0E, 0xFE0F) or 0x0300 <= cp <= 0x036F:
        return True
    return (
        0x00A0 <= cp <= 0x00FF or
        0x2000 <= cp <= 0x206F or
        0x20A0 <= cp <= 0x20CF or
        0x2100 <= cp <= 0x214F or
        0x2190 <= cp <= 0x21FF or
        0x2200 <= cp <= 0x22FF or
        0x2300 <= cp <= 0x23FF or
        0x2460 <= cp <= 0x24FF or
        0x2500 <= cp <= 0x25FF or
        0x2600 <= cp <= 0x26FF or
        0x2700 <= cp <= 0x27BF or
        0x3000 <= cp <= 0x303F or
        0xFF00 <= cp <= 0xFFEF
    )


def is_symbol_text(text):
    text = text.strip()
    if not text:
        return False
    if is_ascii_code(text) or is_cjk_word(text):
        return False
    if len(text.encode("utf-8")) > 32:
        return False
    return all(is_supported_symbol_char(ch) for ch in text)


def strip_pinyin_tones(text):
    mapped = []
    for ch in text:
        if ch in "üǖǘǚǜÜǕǗǙǛ":
            mapped.append("v")
        else:
            mapped.append(ch)
    decomposed = unicodedata.normalize("NFD", "".join(mapped))
    return "".join(ch for ch in decomposed if unicodedata.category(ch) != "Mn")


def yaml_symbol_items(text):
    text = text.strip()
    if text.startswith("{commit:"):
        match = re.search(r"\{commit:\s*['\"]?([^,'\"} ]+)['\"]?\s*\}", text)
        return [match.group(1)] if match else []
    if text.startswith("{pair:"):
        return re.findall(r"['\"]([^'\"]+)['\"]", text)
    if text.startswith("[") and text.endswith("]"):
        body = text[1:-1]
        items = []
        for part in body.split(","):
            part = part.strip().strip("'\"")
            if part:
                items.append(part)
        return items
    return [text.strip("'\"")] if text else []


def normalize_code(code):
    code = strip_pinyin_tones(code)
    code = code.strip().lower().replace("ü", "v").replace("u:", "v")
    code = re.sub(r"[0-5]", "", code)
    code = re.sub(r"[^a-z' ]+", " ", code)
    return " ".join(code.split())


def split_joined_pinyin(code):
    if not code:
        return None
    if " " in code or "'" in code:
        parts = [p for p in re.split(r"[ ']+", code) if p]
        return parts if parts and all(p in PINYIN_SYLLABLES for p in parts) else None

    # Dynamic programming over syllables, preferring fewer/longer chunks.
    n = len(code)
    best = [None] * (n + 1)
    best[0] = []
    for i in range(n):
        if best[i] is None:
            continue
        for j in range(min(n, i + 6), i, -1):
            syl = code[i:j]
            if syl not in PINYIN_SYLLABLES:
                continue
            cand = best[i] + [syl]
            if best[j] is None or len(cand) < len(best[j]):
                best[j] = cand
    return best[n]


def parse_weight(text):
    try:
        return int(float(text))
    except ValueError:
        return 1


def parse_dict_line(raw):
    line = raw.strip()
    if not line or line.startswith("#") or line.startswith("---") or line.startswith("..."):
        return None
    if line.startswith(("name:", "version:", "sort:", "use_preset_vocabulary:",
                        "import_tables:", "columns:", "encoder:", "translator:")):
        return None

    parts = line.split("\t")
    if len(parts) < 2:
        parts = line.split()
    if len(parts) < 2:
        return None

    first, second = parts[0].strip(), parts[1].strip()
    weight = parse_weight(parts[2]) if len(parts) >= 3 else 1

    if is_cjk_word(first):
        word, code = first, second
    elif is_cjk_word(second):
        code, word = first, second
    else:
        return None

    code = normalize_code(code)
    syllables = split_joined_pinyin(code)
    if syllables is None:
        return None
    return " ".join(syllables), word, max(1, weight)


def parse_symbol_line(raw):
    line = raw.strip()
    if not line or line.startswith("#") or line.startswith("---") or line.startswith("..."):
        return None
    if line.startswith(("name:", "version:", "sort:", "use_preset_vocabulary:",
                        "import_tables:", "columns:", "encoder:", "translator:")):
        return None

    yaml_match = re.match(r"""^['"]?/([A-Za-z]+)['"]?\s*:\s*(.+)$""", line)
    if yaml_match:
        code = normalize_symbol_code(yaml_match.group(1))
        out = []
        for face in yaml_symbol_items(yaml_match.group(2)):
            if is_symbol_text(face):
                out.append((code, face))
        return out or None

    parts = line.split("\t")
    if len(parts) < 2:
        parts = line.split(None, 1)
    if len(parts) < 2:
        return None

    first, rest = parts[0].strip(), parts[1].strip()
    if is_ascii_code(first):
        code, faces = normalize_symbol_code(first), rest.split()
    elif is_symbol_text(first):
        code, faces = normalize_symbol_code(rest.split()[0]), [first]
    else:
        return None

    if not code:
        return None
    out = []
    for face in faces:
        if is_symbol_text(face):
            out.append((code, face))
    return out or None


def load_existing_seg(path):
    existing = set()
    if not path.exists():
        return existing
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "\t" not in line:
            continue
        syllables, word = line.split("\t", 1)
        existing.add((" ".join(syllables.split()), word.strip()))
    return existing


def load_existing_predict(path):
    existing = defaultdict(set)
    if not path.exists():
        return existing
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "\t" in line:
            key, rest = line.split("\t", 1)
            words = rest.replace("\t", " ").split()
        else:
            parts = line.split()
            key, words = parts[0], parts[1:]
        for word in words:
            existing[key].add(word)
    return existing


def load_existing_symbols(path):
    existing = {("biaodian", face) for face in BASE_PUNCT}
    if not path.exists():
        return existing
    for raw in path.read_text(encoding="utf-8").splitlines():
        if "\t" not in raw:
            continue
        code, face = raw.rstrip("\n").split("\t", 1)
        code = normalize_symbol_code(code)
        face = face.strip()
        if code and face:
            existing.add((code, face))
    return existing


def read_entries(paths):
    merged = {}
    skipped = 0
    for path in paths:
        for raw in path.read_text(encoding="utf-8-sig").splitlines():
            parsed = parse_dict_line(raw)
            if parsed is None:
                skipped += 1
                continue
            syllables, word, weight = parsed
            key = (syllables, word)
            merged[key] = max(merged.get(key, 0), weight)
    entries = [(syllables, word, weight) for (syllables, word), weight in merged.items()]
    return entries, skipped


def read_symbol_entries(paths):
    seen = set()
    entries = []
    skipped = 0
    for path in paths:
        for raw in path.read_text(encoding="utf-8-sig").splitlines():
            parsed = parse_symbol_line(raw)
            if parsed is None:
                skipped += 1
                continue
            for code, face in parsed:
                key = (code, face)
                if key in seen:
                    continue
                seen.add(key)
                entries.append(key)
    return entries, skipped


def build_seg_delta(entries, existing, min_chars, max_chars, per_code, limit):
    buckets = defaultdict(list)
    for syllables, word, weight in entries:
        chars = len(word)
        if chars < min_chars or chars > max_chars:
            continue
        if not is_cjk_word(word):
            continue
        key = (syllables, word)
        if key in existing:
            continue
        code = "".join(syllables.split())
        buckets[code].append((weight, syllables, word))

    out = []
    for code in sorted(buckets):
        ranked = sorted(buckets[code], key=lambda item: (-item[0], len(item[2]), item[2]))
        for weight, syllables, word in ranked[:per_code]:
            out.append((weight, syllables, word))
    out.sort(key=lambda item: (-item[0], item[1], item[2]))
    return out[:limit]


def build_predict_delta(entries, existing, max_groups, candidates_per_key):
    ranked = sorted(entries, key=lambda item: (-item[2], len(item[1]), item[1]))
    groups = OrderedDict()
    for _syllables, word, weight in ranked:
        if not is_cjk_word(word) or len(word) < 2:
            continue
        max_key_len = min(4, len(word) - 1)
        for key_len in range(max_key_len, 0, -1):
            key = word[:key_len]
            tail = word[key_len:key_len + 4]
            if not tail or tail in existing.get(key, set()):
                continue
            if key not in groups:
                if len(groups) >= max_groups:
                    continue
                groups[key] = []
            if tail not in groups[key] and len(groups[key]) < candidates_per_key:
                groups[key].append((weight, tail))
    return OrderedDict(
        (key, [tail for _weight, tail in tails])
        for key, tails in groups.items()
        if tails
    )


def build_symbol_delta(entries, existing, per_code, limit):
    buckets = defaultdict(list)
    for code, face in entries:
        if (code, face) in existing:
            continue
        buckets[code].append(face)

    out = []
    for code in sorted(buckets):
        seen_faces = set()
        for face in buckets[code]:
            if face in seen_faces:
                continue
            seen_faces.add(face)
            out.append((code, face))
            if len(seen_faces) >= per_code:
                break
            if len(out) >= limit:
                return out
    return out[:limit]


def write_seg_delta(items, path):
    lines = [
        "# Generated by scripts/wanxiang_ime_import.py.",
        "# Review, then rerun with --apply to merge.",
    ]
    for _weight, syllables, word in items:
        lines.append(f"{syllables}\t{word}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_predict_delta(groups, path):
    lines = [
        "# Generated by scripts/wanxiang_ime_import.py.",
        "# Review, then rerun with --apply to merge.",
    ]
    for key, words in groups.items():
        lines.append(f"{key}\t{' '.join(words)}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_symbol_delta(items, path):
    lines = [
        "# Generated by scripts/wanxiang_ime_import.py.",
        "# These entries are for v/encoded symbol search via kaomoji_source.txt.",
        "# Review, then rerun with --apply to merge.",
    ]
    for code, face in items:
        lines.append(f"{code}\t{face}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def append_delta(target, delta, marker):
    body = []
    for raw in delta.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        body.append(line)
    if not body:
        return 0
    current = target.read_text(encoding="utf-8") if target.exists() else ""
    addition = "\n# " + marker + "\n" + "\n".join(body) + "\n"
    target.write_text(current.rstrip() + addition, encoding="utf-8")
    return len(body)


def main():
    ap = argparse.ArgumentParser(
        description="Generate pjournal IME seg/predict deltas from Wanxiang/Rime dictionaries."
    )
    ap.add_argument("inputs", nargs="+", type=Path, help="Wanxiang/Rime dictionary files")
    ap.add_argument("--seg-output", type=Path, default=DEFAULT_SEG_DELTA)
    ap.add_argument("--predict-output", type=Path, default=DEFAULT_PREDICT_DELTA)
    ap.add_argument("--symbol-output", type=Path, default=DEFAULT_SYMBOL_DELTA)
    ap.add_argument("--apply", action="store_true",
                    help="append generated deltas to IME source files")
    ap.add_argument("--min-seg-chars", type=int, default=2)
    ap.add_argument("--max-seg-chars", type=int, default=4)
    ap.add_argument("--seg-per-code", type=int, default=6)
    ap.add_argument("--seg-limit", type=int, default=4000)
    ap.add_argument("--predict-groups", type=int, default=2500)
    ap.add_argument("--predict-per-key", type=int, default=16)
    ap.add_argument("--symbol-per-code", type=int, default=32)
    ap.add_argument("--symbol-limit", type=int, default=2000)
    args = ap.parse_args()

    entries, skipped = read_entries(args.inputs)
    symbol_entries, symbol_skipped = read_symbol_entries(args.inputs)
    existing_seg = load_existing_seg(SEG_SOURCE)
    existing_predict = load_existing_predict(PREDICT_SOURCE)
    existing_symbols = load_existing_symbols(KAOMOJI_SOURCE)

    seg_delta = build_seg_delta(
        entries, existing_seg, args.min_seg_chars, args.max_seg_chars,
        args.seg_per_code, args.seg_limit,
    )
    predict_delta = build_predict_delta(
        entries, existing_predict, args.predict_groups, args.predict_per_key,
    )
    symbol_delta = build_symbol_delta(
        symbol_entries, existing_symbols, args.symbol_per_code, args.symbol_limit,
    )

    args.seg_output.parent.mkdir(parents=True, exist_ok=True)
    args.predict_output.parent.mkdir(parents=True, exist_ok=True)
    args.symbol_output.parent.mkdir(parents=True, exist_ok=True)
    write_seg_delta(seg_delta, args.seg_output)
    write_predict_delta(predict_delta, args.predict_output)
    write_symbol_delta(symbol_delta, args.symbol_output)

    print(f"read {len(entries)} usable entries ({skipped} skipped/non-dict lines)")
    print(f"read {len(symbol_entries)} usable symbol entries ({symbol_skipped} skipped/non-symbol lines)")
    print(f"wrote {args.seg_output}: {len(seg_delta)} seg entries")
    print(f"wrote {args.predict_output}: {len(predict_delta)} predict groups")
    print(f"wrote {args.symbol_output}: {len(symbol_delta)} symbol entries")

    if args.apply:
        seg_n = append_delta(SEG_SOURCE, args.seg_output, "Wanxiang seg imports")
        pred_n = append_delta(PREDICT_SOURCE, args.predict_output, "Wanxiang predict imports")
        sym_n = append_delta(KAOMOJI_SOURCE, args.symbol_output, "Wanxiang symbol imports")
        print(f"applied {seg_n} seg entries to {SEG_SOURCE}")
        print(f"applied {pred_n} predict groups to {PREDICT_SOURCE}")
        print(f"applied {sym_n} symbol entries to {KAOMOJI_SOURCE}")
        print("next: run scripts/generate_seg_table.py, scripts/add_ime_predictions.py, and scripts/kaomoji_convert.py")


if __name__ == "__main__":
    main()
