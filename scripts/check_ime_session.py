#!/usr/bin/env python3
"""Check higher-level IME session invariants that query-only tests miss."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(name, ok):
    if not ok:
        raise SystemExit(f"FAIL {name}")


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
    print("OK: IME session checks passed")


if __name__ == "__main__":
    main()
