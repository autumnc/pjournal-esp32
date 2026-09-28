#!/usr/bin/env python3
"""Check higher-level IME session invariants that query-only tests miss."""

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
    require("host backspace api", "handleHostBackspace" in ime_h)
    require("host backspace hook", "handleHostBackspace" in editor_cpp)
    require("recent boost engine", "recentCommitBoost" in ime_cpp)
    require("predict mode setting", "_ime_predict_mode" in settings_cpp)
    require("space-only prediction gate", 'mode == "space"' in ime_cpp)

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

    # 文档级上下文(#13): IME 侧固定槽位计数表 + 编辑器喂入钩子。
    require("doc context api", "setDocumentContext" in ime_h)
    require("doc context table", "documentContextBoost" in ime_cpp)
    require("doc context folded into ctx boost", "documentContextBoost(word)" in ime_cpp)
    require("doc context setting default on", 'getString("ime_doc_context", "1")' in settings_mgr)
    require("doc context settings toggle", '"ime_doc_context"' in settings_cpp)
    require("doc context editor feed", "setDocumentContext(editorImeContextText())" in editor_cpp)

    # 实用快捷输入(#14): 无前缀符号按精确整码匹配 + 行内英文候选排到中文路径之后。
    require("shortcut symbol table", "K_SHORTCUT_SYMBOLS" in ime_cpp)
    require("shortcut symbol api", "appendShortcutSymbol" in ime_h)
    require("shortcut exact full-code match", "appendShortcutSymbol(q, qlen)" in ime_cpp)
    require("english inline after phase 7",
            ime_cpp.find("appendEnglishInlineCandidates(pinyinCode)")
            > ime_cpp.find("// Phase 7: shorthand + tail match"))
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
