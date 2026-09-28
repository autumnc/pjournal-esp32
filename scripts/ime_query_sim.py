#!/usr/bin/env python3
import argparse
import bisect
import re
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TABLE = ROOT / "main" / "ime" / "ime_table_pinyin.bin"
DEFAULT_SEG = ROOT / "main" / "ime" / "seg_table_source.txt"
DEFAULT_PINYIN = ROOT / "main" / "ime" / "yong_pinyin.cpp"
INDEX_ENTRIES = 26 * 26 + 1
HEADER_SIZE = 12

# 整句覆盖的打分常量, 与 IME.cpp 里的 IME_SENTENCE_* 保持一致。
SENT_MIN_LEN = 6
SENT_MAX_LEN = 24
SENT_BEAM = 8
SENT_RESULTS = 3
SENT_BUCKET_SCAN = 3600
SENT_TOTAL_SCAN = 8000
SENT_WORD_ARCS = 3
SENT_SINGLE_ARCS = 2
SENT_WORD_UNIT = 4000
SENT_ARC_PENALTY = 500
SENT_SINGLE = -400
SENT_RANK_WORD = 300
SENT_RANK_SINGLE = 60
SENT_LEN_BIAS = 1
SENT_GROUP_STRIDE = 16


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def utf8_chars(text):
    return len(text)


def prefix_key(text):
    if not text:
        return 26 * 26
    c0 = ord(text[0]) - ord("a")
    if c0 < 0 or c0 >= 26:
        return 26 * 26
    if len(text) == 1:
        return c0 * 26
    c1 = ord(text[1]) - ord("a")
    if c1 < 0 or c1 >= 26:
        return 26 * 26
    return c0 * 26 + c1


class Ime3:
    def __init__(self, path):
        self.blob = Path(path).read_bytes()
        if len(self.blob) < HEADER_SIZE or self.blob[:4] != b"IME3":
            raise SystemExit(f"{path}: not an IME3 table")
        self.code_len = self.blob[5]
        self.single_count = u32(self.blob, 8)
        self.record_size = self.code_len + 4
        self.single_base = HEADER_SIZE + INDEX_ENTRIES * 4
        self.index = [u32(self.blob, HEADER_SIZE + i * 4) for i in range(INDEX_ENTRIES)]
        single_end = self.single_base + self.single_count * self.record_size
        self.word_count = u32(self.blob, single_end)
        word_index_base = single_end + 4
        self.word_index = [u32(self.blob, word_index_base + i * 4) for i in range(INDEX_ENTRIES)]
        self.word_base = word_index_base + INDEX_ENTRIES * 4
        self.word_size = self.word_index[-1]
        self.word_data = self.blob[self.word_base:self.word_base + self.word_size]
        self._group_offsets_cache = {}

    def single_window(self, code):
        k = prefix_key(code)
        if k >= 26 * 26:
            return 0, self.single_count
        if len(code) == 1:
            return self.index[k], self.index[k + 26]
        return self.index[k], self.index[k + 1]

    def single_record(self, i):
        off = self.single_base + i * self.record_size
        code = self.blob[off:off + self.code_len].split(b"\0", 1)[0].decode()
        text = self.blob[off + self.code_len:off + self.code_len + 3].decode()
        flag = self.blob[off + self.code_len + 3]
        return code, text, flag

    def singles(self, code, limit):
        lo, hi = self.single_window(code)
        keys = [self.single_record(i)[0] for i in range(lo, hi)]
        pos = bisect.bisect_left(keys, code)
        out = []
        for i in range(lo + pos, hi):
            rec_code, text, flag = self.single_record(i)
            if not rec_code.startswith(code):
                break
            if flag & 0x02:
                continue
            if text not in out:
                out.append(text)
                if len(out) >= limit:
                    break
        return out

    def word_window(self, code):
        k = prefix_key(code)
        if k >= 26 * 26:
            return 0, self.word_size
        if len(code) == 1:
            return self.word_index[k], self.word_index[k + 26]
        return self.word_index[k], self.word_index[k + 1]

    def phrases(self, code, exact=False, limit=40):
        lo, hi = self.word_window(code)
        out = []
        pos = lo
        safety = 0
        while pos < hi and safety < 20000 and len(out) < limit:
            safety += 1
            cl = self.word_data[pos]
            if cl == 0 or pos + 1 + cl > hi:
                break
            rec_code = self.word_data[pos + 1:pos + 1 + cl].decode()
            pos += 1 + cl
            n = self.word_data[pos]
            pos += 1
            group_match = rec_code == code if exact else rec_code.startswith(code)
            for _ in range(n):
                wl = self.word_data[pos]
                pos += 1
                word = self.word_data[pos:pos + wl].decode()
                flag = self.word_data[pos + wl]
                pos += wl + 1
                if group_match and not (flag & 0x02) and word not in out:
                    out.append(word)
                    if len(out) >= limit:
                        break
            if rec_code > code and not rec_code.startswith(code):
                break
        return out

    # 桶内所有组的字节偏移。只在主机侧用: 设备上没有这个数组, 靠按组解析前进(等价偏移)。
    def _group_offsets(self, lo, hi):
        key = (lo, hi)
        cached = self._group_offsets_cache.get(key)
        if cached is not None:
            return cached
        wd = self.word_data
        offs = []
        p = lo
        while p < hi:
            cl = wd[p]
            if cl == 0 or p + 1 + cl > hi:
                break
            offs.append(p)
            p += 1 + cl
            if p >= hi:
                break
            n = wd[p]
            p += 1
            for _ in range(n):
                if p + 1 > hi:
                    p = hi
                    break
                wl = wd[p]
                if wl == 0 or p + 1 + wl + 1 > hi:
                    p = hi
                    break
                p += 1 + wl + 1
        self._group_offsets_cache[key] = offs
        return offs

    def group_code(self, p):
        cl = self.word_data[p]
        return self.word_data[p + 1:p + 1 + cl]

    # 桶内第一个码 >= target(strcmp 序)的组偏移; 没有则 hi。词表只有 2 字前缀粒度的桶索引,
    # 桶内组记录变长(没有长度字段), 定位只能从桶首逐组走过去, 而每个组还要跨过它自己的
    # 每一条词, 所以代价正比于桶的字节数(实测最坏的 sh 桶 113KB)。整句相位每个 pos 都要
    # 定位一次, 长全拼串每敲一键就重扫一遍。这里每 SENT_GROUP_STRIDE 组记一个检查点,
    # 二分到目标所在区间后只线性走至多 STRIDE 组。
    def seek_group(self, lo, hi, target, reads):
        offs = self._group_offsets(lo, hi)
        if not offs:
            return hi
        ck = offs[::SENT_GROUP_STRIDE] + [None]  # None 当 +inf 哨兵
        a, b = 0, len(ck)
        while a < b:
            mid = (a + b) // 2
            cp = ck[mid]
            if cp is None or self.group_code(cp) >= target:
                b = mid
            else:
                a = mid + 1
        idx = max(0, a - 1) * SENT_GROUP_STRIDE
        while idx < len(offs):
            reads[0] += 1
            if self.group_code(offs[idx]) >= target:
                return offs[idx]
            idx += 1
        return hi

    def word_group_codes(self):
        """真实词库里所有组码。"""
        out = []
        for i in range(INDEX_ENTRIES - 1):
            lo, hi = self.word_index[i], self.word_index[i + 1]
            if hi > lo:
                out += [self.group_code(p) for p in self._group_offsets(lo, hi)]
        return out

    def sentence_index_failures(self, codes, syllables):
        """校验整句的词桶组偏移索引, 返回失败描述(空 = 通过)。
        两层: ① seek_group 对每个桶的每一组(及两个扩展变体)穷举比对线性参照;
              ② 整串候选 indexed 路径必须与桶首顺扫路径完全一致。"""
        bad = []
        for i in range(INDEX_ENTRIES - 1):
            lo, hi = self.word_index[i], self.word_index[i + 1]
            if hi <= lo:
                continue
            offs = self._group_offsets(lo, hi)
            ref_codes = [self.group_code(x) for x in offs]
            for k, p in enumerate(offs):
                # 精确命中每一组都验; 两个扩展变体抽样, 覆盖"落在组与组之间"的定位。
                targets = [ref_codes[k]]
                if k % 8 == 0:
                    targets += [ref_codes[k] + b"0", ref_codes[k] + b"z"]
                for target in targets:
                    reads = [0]
                    got = self.seek_group(lo, hi, target, reads)
                    ref = bisect.bisect_left(ref_codes, target)
                    want = offs[ref] if ref < len(offs) else hi
                    if got != want:
                        return [f"seek {target!r} bucket {lo}: {got} != {want}"]
        for code in codes:
            a, b = self.sentence(code, syllables), self.sentence_indexed(code, syllables)
            if a != b:
                bad.append(f"sentence {code}: {a} != {b}")
        return bad

    # 长全拼串的词图 beam search, 镜像 IME.cpp 的 collectSentenceArcs/appendSentenceCandidates。
    # 用户词库弧与搭配分不在这里复现: 那是设备上的增量状态, 主机侧没有等价来源。
    # indexed=True 走词桶组偏移索引, False 走原来的桶首顺扫; 预算没吃满时两者必须同弧。
    def sentence(self, code, syllables):
        return self._sentence(code, syllables, False)

    def sentence_indexed(self, code, syllables):
        return self._sentence(code, syllables, True)

    def _sentence(self, code, syllables, indexed):
        n = len(code)
        if n < SENT_MIN_LEN or n > SENT_MAX_LEN:
            return []
        wd = self.word_data
        groups = [0]

        def collect(pos):
            remain = n - pos
            arcs = []
            total_left = SENT_TOTAL_SCAN - groups[0]
            if total_left <= 0:
                return arcs
            budget = min(SENT_BUCKET_SCAN, total_left)
            # 位置长度偏置: 词库没有词频, 同分路径靠"长弧优先"定序。
            bias = lambda cl: SENT_LEN_BIAS * cl * (n - pos)
            wlo, whi = self.word_window(code[pos:pos + 2] if remain >= 2 else code[pos:pos + 1])
            reads = [0]
            p = wlo
            if indexed:
                # 匹配的组就是"码恰好是 T 前缀"的词条, 组按码升序即按长度升序, 所以按长度
                # 递增探测, 第一个命中的就是词条顺序里的第一个匹配。一个都没命中说明桶里
                # 没有词弧, 连桶都不用走。
                start = None
                for ln in range(1, remain + 1):
                    if reads[0] >= budget:
                        break
                    cand = code[pos:pos + ln].encode()
                    off = self.seek_group(wlo, whi, cand, reads)
                    if off < whi and self.group_code(off) == cand:
                        start = off
                        break
                p = start if start is not None else whi
            scanned = reads[0]
            while p < whi and scanned < budget:
                cl = wd[p]
                if cl == 0 or p + 1 + cl > whi:
                    break
                wc = wd[p + 1:p + 1 + cl]
                q = p + 1 + cl
                if q >= whi:
                    break
                cnt = wd[q]
                q += 1
                scanned += 1
                cmplen = min(cl, remain)
                tgt = code[pos:pos + cmplen].encode()
                seg = wc[:cmplen]
                if seg > tgt:
                    break
                if seg == tgt and cl > remain:
                    break
                matched = cl <= remain and seg == tgt
                taken = 0
                for _ in range(cnt):
                    if q + 1 > whi:
                        q = whi
                        break
                    wl = wd[q]
                    if wl == 0 or q + 1 + wl + 1 > whi:
                        q = whi
                        break
                    if matched and taken < SENT_WORD_ARCS:
                        word = wd[q + 1:q + 1 + wl].decode()
                        if not (wd[q + 1 + wl] & 0x02):
                            arcs.append((pos, pos + cl, word,
                                         SENT_WORD_UNIT * (len(word) - 1)
                                         - SENT_RANK_WORD * taken - SENT_ARC_PENALTY + bias(cl)))
                            taken += 1
                    q += 1 + wl + 1
                p = q
            groups[0] += scanned
            # 单字弧: 段必须恰好是一个合法音节, 否则 "xian" 会被拆成 "xi"+"an"。
            for cl in range(1, min(6, remain) + 1):
                syl = code[pos:pos + cl]
                if syl not in syllables:
                    continue
                for r, txt in enumerate(self.singles(syl, SENT_SINGLE_ARCS)):
                    arcs.append((pos, pos + cl, txt, SENT_SINGLE - SENT_RANK_SINGLE * r + bias(cl)))
            return arcs

        by_hi = [[] for _ in range(n + 1)]
        for pos in range(n):
            for arc in collect(pos):
                by_hi[arc[1]].append(arc)
        nodes = [(-1, None, 0)]  # (parent, arc, score); 索引 0 = 根
        nstart = [0] * (n + 1)
        nend = [0] * (n + 1)
        nstart[0], nend[0] = 0, 1
        for hi in range(1, n + 1):
            cands = []
            for arc in by_hi[hi]:
                for ni in range(nstart[arc[0]], nend[arc[0]]):
                    cands.append((ni, arc, nodes[ni][2] + arc[3]))
            cands.sort(key=lambda c: -c[2])
            cands = cands[:SENT_BEAM]
            nstart[hi] = len(nodes)
            nend[hi] = nstart[hi] + len(cands)
            nodes = nodes + cands
        out = []
        for ni in range(nstart[n], nend[n]):
            parts = []
            cur = ni
            while cur > 0:
                nd = nodes[cur]
                if nd[1] is None:
                    break
                parts.insert(0, nd[1][2])
                cur = nd[0]
            text = "".join(parts)
            if text and text not in out:
                out.append(text)
            if len(out) >= SENT_RESULTS:
                break
        return out


def load_seg(path):
    entries = []
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "\t" not in line:
            continue
        syllables, word = line.split("\t", 1)
        parts = syllables.split()
        code = "".join(parts)
        initial = "".join((s[:2] if s[:2] in ("zh", "ch", "sh") else s[:1]) for s in parts)
        compact = "".join(s[:1] for s in parts)
        entries.append((code, initial, compact, word.strip()))
    return entries


def seg_matches(entries, code, limit):
    scored = []
    for idx, (full, initial, compact, word) in enumerate(entries):
        if full.startswith(code):
            scored.append((0 if len(full) == len(code) else 2, len(full), idx, word))
        elif len(code) >= 2 and (initial.startswith(code) or compact.startswith(code)):
            m = min(len(x) for x in (initial, compact) if x.startswith(code))
            scored.append((1 if m == len(code) else 3, m, idx, word))
    out = []
    for _, _, _, word in sorted(scored):
        if word not in out:
            out.append(word)
            if len(out) >= limit:
                break
    return out


def query(table, seg_entries, code, limit):
    code = "".join(ch.lower() for ch in code if ch.isalpha())
    if not code:
        return []
    out = []
    def add_many(items):
        for item in items:
            if item not in out:
                out.append(item)
                if len(out) >= limit:
                    return
    if len(code) >= 4:
        add_many(table.phrases(code, exact=True, limit=8))
    add_many(table.singles(code, limit=16))
    add_many(seg_matches(seg_entries, code, limit=24))
    add_many(table.phrases(code, exact=False, limit=limit))
    return out[:limit]


def load_syllables(path):
    text = Path(path).read_text(encoding="utf-8")
    block = text.split("kSyllables[] = {", 1)[1].split("};", 1)[0]
    return set(re.findall(r'"([a-z]+)"', block))


def main():
    ap = argparse.ArgumentParser(description="Offline approximate IME candidate query.")
    ap.add_argument("codes", nargs="+", help="pinyin codes to inspect")
    ap.add_argument("--table", default=str(DEFAULT_TABLE))
    ap.add_argument("--seg", default=str(DEFAULT_SEG))
    ap.add_argument("--pinyin", default=str(DEFAULT_PINYIN))
    ap.add_argument("--limit", type=int, default=20)
    ap.add_argument("--sentence", action="store_true",
                    help="long-pinyin whole-sentence candidates (word-graph beam search)")
    args = ap.parse_args()

    table = Ime3(args.table)
    if args.sentence:
        syllables = load_syllables(args.pinyin)
        for code in args.codes:
            code = "".join(ch.lower() for ch in code if ch.isalpha())
            print(f"{code}: {' '.join(table.sentence(code, syllables))}")
        return
    seg_entries = load_seg(args.seg)
    for code in args.codes:
        cands = query(table, seg_entries, code, args.limit)
        print(f"{code}: {' '.join(cands)}")


if __name__ == "__main__":
    main()
