#!/usr/bin/env python3
"""Check higher-level IME session invariants that query-only tests miss."""

import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from ime_query_sim import DEFAULT_PINYIN, DEFAULT_TABLE, Ime3, load_syllables


def require(name, ok):
    if not ok:
        raise SystemExit(f"FAIL {name}")


def syllable_tokens(code, syllables):
    """Greedy longest-match syllable split, mirroring the engine's primary split."""
    n, pos, tokens = len(code), 0, []
    while pos < n:
        for ln in range(min(6, n - pos), 0, -1):
            if code[pos:pos + ln] in syllables:
                tokens.append(code[pos:pos + ln])
                pos += ln
                break
        else:
            return None
    return tokens


class RecentCommits:
    def __init__(self):
        self.items = []

    def remember(self, code, word):
        self.items = [item for item in self.items if item != (code, word)]
        self.items.insert(0, (code, word))
        del self.items[32:]

    def boost(self, code, word):
        for i, (item_code, item_word) in enumerate(self.items):
            if item_word != word:
                continue
            boost = 6000 - i * 180
            if boost <= 0:
                return 0
            if item_code == code:
                return boost + 4000
            if item_code.startswith(code):
                return boost
        return 0

    def candidates(self, code):
        out = []
        for item_code, item_word in self.items:
            if item_code.startswith(code) and item_word not in out:
                out.append(item_word)
        return out


class CandidateSession:
    """Small host-side model for interaction rules that must stay cheap on-device."""

    def __init__(self, highlight=False):
        self.highlight = highlight
        self.code = ""
        self.page = []
        self.sel = 0
        self.predicting = False

    def compose(self, code, page):
        self.code = code
        self.page = list(page)
        self.sel = 0
        self.predicting = False

    def predict(self, page):
        self.code = ""
        self.page = list(page)
        self.sel = 0
        self.predicting = True

    def key(self, key):
        if self.predicting:
            if key in ("enter", "backspace"):
                self.predicting = False
                self.page = []
                return "pass"
            if key == "esc":
                self.predicting = False
                self.page = []
                return "handled"
            if key == "space" and self.page:
                out = self.page[0]
                self.predicting = False
                return out
            if len(key) == 1 and key.isalpha():
                self.compose(key, [])
                return "handled"
            return "handled"

        if key == "left" and self.highlight and self.page:
            self.sel = (self.sel + len(self.page) - 1) % len(self.page)
            return "handled"
        if key == "right" and self.highlight and self.page:
            self.sel = (self.sel + 1) % len(self.page)
            return "handled"
        if key == "space":
            if self.page:
                return self.page[self.sel if self.highlight else 0]
            return self.code + " "
        if key == "enter":
            if self.highlight and self.page:
                return self.page[self.sel]
            return self.code
        return "handled"


def main():
    recent = RecentCommits()
    recent.remember("shurutiyan", "输入体验")
    recent.remember("shurufa", "输入法")
    require("exact recent boost", recent.boost("shurufa", "输入法") > recent.boost("shuru", "输入法"))
    require("prefix recent candidate", recent.candidates("shuru")[:2] == ["输入法", "输入体验"])
    for i in range(40):
        recent.remember(f"code{i}", f"词{i}")
    require("recent ring cap", len(recent.items) == 32)

    ime_h = (ROOT / "main" / "ime" / "IME.h").read_text(encoding="utf-8")
    ime_cpp = (ROOT / "main" / "ime" / "IME.cpp").read_text(encoding="utf-8")
    editor_cpp = (ROOT / "main" / "screen_editor.cpp").read_text(encoding="utf-8")
    settings_cpp = (ROOT / "main" / "screen_settings.cpp").read_text(encoding="utf-8")
    settings_mgr = (ROOT / "main" / "settings_manager.cpp").read_text(encoding="utf-8")
    require("host backspace api", "handleHostBackspace" in ime_h)
    require("host backspace hook", "handleHostBackspace" in editor_cpp)
    require("recent boost engine", "recentCommitBoost" in ime_cpp)
    require("predict mode setting", "_ime_predict_mode" in settings_cpp)
    require("space-only prediction gate", 'mode == "space"' in ime_cpp)

    # Host-side input session checks: these mirror the high-level key semantics
    # without executing firmware code. They guard quiet typing behavior, not ranking.
    sess = CandidateSession(highlight=False)
    sess.compose("shuru", ["输入", "输入法"])
    sess.key("right")
    require("highlight off ignores right", sess.sel == 0)
    require("highlight off space commits first", sess.key("space") == "输入")

    sess = CandidateSession(highlight=True)
    sess.compose("shuru", ["输入", "输入法", "输入体验"])
    sess.key("right")
    require("highlight right moves selection", sess.sel == 1)
    require("highlight space commits selected", sess.key("space") == "输入法")
    sess.compose("shuru", ["输入", "输入法"])
    sess.key("left")
    require("highlight left wraps", sess.sel == 1)
    require("highlight enter commits selected", sess.key("enter") == "输入法")

    sess = CandidateSession(highlight=False)
    sess.compose("zzzz", [])
    require("empty candidate space preserves code", sess.key("space") == "zzzz ")

    sess = CandidateSession()
    sess.predict(["们", "的"])
    require("prediction enter passes through", sess.key("enter") == "pass")
    sess.predict(["们", "的"])
    require("prediction backspace passes through", sess.key("backspace") == "pass")
    sess.predict(["们", "的"])
    require("prediction space accepts first", sess.key("space") == "们")

    require("candidate highlight setting", "imeCandidateHighlight" in settings_mgr)
    require("candidate highlight snapshot", "_highlightSelectMode = g_settings.imeCandidateHighlight()" in ime_cpp)
    require("candidate highlight not default", 'getString("ime_candidate_highlight", "0")' in settings_mgr)
    require("candidate highlight left key", "key == IME_KEY_LEFT" in ime_cpp and "_sel =" in ime_cpp)
    require("prediction enter pass-through", "key == '\\n'" in ime_cpp and "return false;" in ime_cpp)
    require("empty candidate space source", 'out = _code + " "' in ime_cpp)
    append_candidate_body = ime_cpp.split("bool IME::appendCandidate", 1)[1].split("\n}\n", 1)[0]
    require("recent delete not global candidate filter",
            "recentlyDeletedWord" not in append_candidate_body)
    remember_deleted_body = ime_cpp.split("void IME::rememberDeletedWord", 1)[1].split("\n}\n", 1)[0]
    require("recent delete clears recent boost",
            "_recentCommittedWords.erase" in remember_deleted_body and
            "rebuildRecentBoostIndex()" in remember_deleted_body)

    # #9: 删词改 swap-and-pop + 索引下标就地改写, 不再整表重建。penalize 在退格撤销刚上屏
    # 学到的词时必然触发, 动态词到 470 条时整表重建要 40ms(实测 perf rebuild by=penalize)。
    require("penalize swap-and-pop", "removeDynamicUserWordAt" in ime_h)
    require("penalize uses swap-and-pop",
            ime_cpp.count("removeDynamicUserWordAt(") >= 3)  # 定义 + penalize + delete
    require("penalize no full rebuild",
            'markUserWordIndexesDirty("penalize")' not in ime_cpp)
    require("delete no full rebuild",
            'markUserWordIndexesDirty("commitDelete")' not in ime_cpp)

    # 上面那两条只证明"没走整表重建"; 算法本身在这里复现一遍, 以"从头重建"为参照物。
    # 桶按 count 降序, 去掉一个元素、再把末尾那条的下标就地改写成 i(它的 count 没变,
    # 原来占的位次本来就正确), 剩下的序列仍应有序——所以不需要重排任何桶。
    def bucket_build(entries):
        buckets = {}
        for idx, entry in enumerate(entries):
            buckets.setdefault(entry[0][:2], []).append(idx)
        for bucket in buckets.values():
            bucket.sort(key=lambda i: -entries[i][1])
        return {k: v for k, v in buckets.items() if v}

    def swap_remove(entries, buckets, i):
        last = len(entries) - 1
        for bucket in buckets.values():
            bucket[:] = [i if v == last else v for v in bucket if v != i]
        if i != last:
            entries[i] = entries[last]
        entries.pop()

    rng = random.Random(7)
    pool = ["ji", "jin", "jint", "jinx", "sh", "shi", "shur", "a", "ab", "abz", "zh"]
    for _ in range(300):
        entries = [(rng.choice(pool), rng.randint(0, 3))
                   for _ in range(rng.randint(1, 14))]
        buckets = bucket_build(entries)
        while entries:
            swap_remove(entries, buckets, rng.randrange(len(entries)))
            ref = bucket_build(entries)
            require("swap_remove membership",
                    {k: sorted(v) for k, v in buckets.items() if v} ==
                    {k: sorted(v) for k, v in ref.items()})
            for bucket in buckets.values():
                counts = [entries[v][1] for v in bucket]
                require("swap_remove keeps buckets ordered",
                        counts == sorted(counts, reverse=True))

    # 整句覆盖(词图 beam search): C++ 侧接线 + 主机镜像的行为回归。
    require("sentence snapshot in begin", "_sentenceMode = g_settings.imeSentence()" in ime_cpp)
    require("sentence arc collection", "collectSentenceArcs" in ime_cpp)
    require("sentence dp append", "appendSentenceCandidates" in ime_cpp)
    require("sentence longest-match bias", "IME_SENTENCE_LEN_BIAS" in ime_cpp)
    settings_mgr = (ROOT / "main" / "settings_manager.cpp").read_text(encoding="utf-8")
    require("sentence setting default on", 'getString("ime_sentence", "1")' in settings_mgr)
    require("sentence settings toggle", '"ime_sentence"' in settings_cpp)

    table = Ime3(DEFAULT_TABLE)
    syllables = load_syllables(DEFAULT_PINYIN)
    # 词库没有词频, 同分路径靠位置长度偏置定序; 这几个用例是打分参数的标定基准,
    # 改动 IME_SENTENCE_* 打分常量会让主机镜像先退红。
    for code, want in [
        ("womenxianzaiqu", "我们现在去"),
        ("jintiantianqihenhao", "今天天气很好"),
        ("zhongguorenmin", "中国人民"),
        ("shurufazhendehenhaoyong", "输入法真的很好用"),
    ]:
        got = table.sentence(code, syllables)
        require(f"sentence top1 {code}={want} (got {got[:1]})", got[:1] == [want])
    require("sentence len floor", table.sentence("women", syllables) == [])
    # IME_SENTENCE_MIN_TOKENS 的标定依据: 四个回归用例都够长, 三个音节的短码不该进
    # (词库里已有现成词, 拼句会切错还抢首位)。整句相位排在首位, 这两边都要卡住。
    for code in ("womenxianzaiqu", "jintiantianqihenhao", "zhongguorenmin",
                 "shurufazhendehenhaoyong"):
        toks = syllable_tokens(code, syllables)
        require(f"sentence gate admits {code} ({toks})", toks is not None and len(toks) >= 4)
    for code in ("nihaoma", "renminbi", "wanshanghao"):
        require(f"sentence gate excludes {code}",
                len(syllable_tokens(code, syllables) or []) == 3)

    # 整句的词桶组偏移索引: seek_group 对真实词典每个桶的每一组(及两个扩展变体)穷举
    # 比对线性参照, 且 indexed 路径与桶首顺扫路径的整串候选必须逐字一致。
    all_codes = table.word_group_codes()
    sample = []
    rng2 = random.Random(11)
    for _ in range(60):
        base = b"".join(rng2.sample(all_codes, rng2.randint(1, 3))).decode()
        for cut in (len(base), max(6, len(base) - rng2.randint(1, 4))):
            cand = base[:cut]
            if 6 <= len(cand) <= 24 and syllable_tokens(cand, syllables):
                sample.append(cand)
    probe_codes = ["womenxianzaiqu", "jintiantianqihenhao", "zhongguorenmin",
                   "shurufazhendehenhaoyong", "shengrikuaile", "wanshangyiqichifan"] + sample
    bad = table.sentence_index_failures(probe_codes, syllables)
    require("sentence bucket index" + (f" ({bad[0]})" if bad else ""), not bad)

    # 词相位(精确/前缀)改 seek 后不再从桶首顺扫: 与桶首顺扫参照逐字一致, 且大桶深处
    # 的目标不再被 900 组扫描预算截断。
    bad = table.phrase_index_failures()
    require("phrase bucket index" + (f" ({bad[0]})" if bad else ""), not bad)

    # 切分 DFS 的 exact 标记必须复用: isValidSyllable 是对 kSyllables 的二分, 在 409 条
    # flash rodata 上每键要跑上千次, 第二轮重算纯属浪费(主机基准 0.84x, 3 万随机码等价)。
    pinyin_cpp = (ROOT / "main" / "ime" / "yong_pinyin.cpp").read_text(encoding="utf-8")
    require("split dfs caches exact flag", "bestExact[bestCount] = exact;" in pinyin_cpp)
    require("split dfs reuses exact flag", "bool exact = bestExact[i];" in pinyin_cpp)
    # isValidSyllable(part) 只该剩两处: splitExplicit 一处 + enumerateSplits 第一轮一处。
    require("split dfs no exact recompute",
            pinyin_cpp.count("PinyinEngine::isValidSyllable(part)") == 2)

    # 文档级上下文(#13): IME 侧固定槽位计数表 + 编辑器喂入钩子。
    require("doc context api", "setDocumentContext" in ime_h)
    require("doc context table", "documentContextBoost" in ime_cpp)
    require("doc context folded into ctx boost", "documentContextBoost(word)" in ime_cpp)
    require("doc context setting default on", 'getString("ime_doc_context", "1")' in settings_mgr)
    require("doc context settings toggle", '"ime_doc_context"' in settings_cpp)
    require("doc context editor feed", "setDocumentContext(editorImeContextText())" in editor_cpp)

    # 实用快捷输入(#14): 无前缀符号按精确整码匹配。行内英文候选(中文模式里插英文词)已
    # 整块移除, 所以这里不再要求它排在 Phase 7 之后 —— 只钉住"它没有偷偷回来"。
    require("shortcut symbol table", "K_SHORTCUT_SYMBOLS" in ime_cpp)
    require("shortcut symbol api", "appendShortcutSymbol" in ime_h)
    require("shortcut exact full-code match", "appendShortcutSymbol(q, qlen)" in ime_cpp)
    require("english inline stays removed",
            "appendEnglishInlineCandidates" not in ime_cpp)
    # 这两个相位必须排在 Phase 1 之前。它们的候选是靠"先追加"占住首屏的, 一旦退回
    # 末尾追加, Phase 8 的逐字匹配会先用首字母单字填满, 用户就看不到整句/符号了
    # (v4.8.x 设备上实测到的就是这个)。
    phase1 = ime_cpp.find("Phase 1: user dict single chars")
    require("shortcut phase before phase 1",
            ime_cpp.find("appendShortcutSymbol(q, qlen)") < phase1)
    require("sentence phase before phase 1",
            ime_cpp.find("appendSentenceCandidates(q, qlen)") < phase1)
    require("sentence min tokens gate", "IME_SENTENCE_MIN_TOKENS" in ime_cpp)
    require("liangfen usd fallback", "appendShortcutSymbol(fullCode.c_str()" in ime_cpp)
    # Phase 0 的安全性来自"表里没有能完整切分成音节的码": 有的话符号会抢到那个码的候选
    # 前面。往表里加新码时这条会立刻退红。
    import re
    block = ime_cpp.split("K_SHORTCUT_SYMBOLS[] = {", 1)[1].split("};", 1)[0]
    shortcut_codes = re.findall(r'\{"([a-z]+)"', block)
    require("shortcut table parsed", len(shortcut_codes) >= 10)
    for sc in shortcut_codes:
        require(f"shortcut code {sc} is not valid pinyin",
                syllable_tokens(sc, syllables) is None)
    print("OK: IME session checks passed")


if __name__ == "__main__":
    main()
