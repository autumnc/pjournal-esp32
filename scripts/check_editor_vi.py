#!/usr/bin/env python3
"""编辑器行号 / vi 模式的源码不变量检查。

这些约束改坏了很难在实机上第一时间发现(vi 键被 IME 吃掉、Esc 直接退出编辑器、
行号槽与折行宽度不一致、多行粘贴塞回单行),所以用静态检查兜底。
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EDITOR = ROOT / "main" / "screen_editor.cpp"
EDITOR_H = ROOT / "main" / "screen_editor.h"
UI_H = ROOT / "main" / "ui_helpers.h"
UI_CPP = ROOT / "main" / "ui_helpers.cpp"
SETTINGS_H = ROOT / "main" / "settings_manager.h"
SETTINGS_CPP = ROOT / "main" / "settings_manager.cpp"
SETTINGS_SCREEN = ROOT / "main" / "screen_settings.cpp"
MAIN_CPP = ROOT / "main" / "main.cpp"

failures = []


def read(path):
    return path.read_text(encoding="utf-8")


def check(cond, msg):
    if not cond:
        failures.append(msg)


def between(text, start, end):
    """从 start 标记到 end 标记之间的子串; 找不到返回 None。"""
    i = text.find(start)
    if i < 0:
        return None
    j = text.find(end, i + len(start))
    if j < 0:
        return None
    return text[i:j]


editor = read(EDITOR)
ui_h = read(UI_H)
ui_cpp = read(UI_CPP)
settings_h = read(SETTINGS_H)
settings_cpp = read(SETTINGS_CPP)
settings_screen = read(SETTINGS_SCREEN)
main_cpp = read(MAIN_CPP)

# ── 1. 分发顺序 ───────────────────────────────────────────────────────────
handle = between(editor, "AppState screen_editor_handle(", "bool screen_editor_idle(")
check(handle is not None, "找不到 screen_editor_handle 函数体")
if handle is not None:
    vi_pos = handle.find("screen_editor_vi_handle(key, ctx, viHandled)")
    help_pos = handle.find("if (key == KEY_HELP)")
    esc_pos = handle.find("if (key == 0x1B)")
    ime_pos = handle.find("if (g_editor.imeActive && key != 0)")
    check(vi_pos >= 0, "screen_editor_handle 未调用 screen_editor_vi_handle")
    check(vi_pos > help_pos, "vi 分发必须在 KEY_HELP 处理之后(帮助模态优先)")
    check(esc_pos >= 0 and vi_pos < esc_pos, "vi 分发必须在裸 Esc 退出之前")
    check(ime_pos >= 0 and vi_pos < ime_pos, "vi 分发必须在 IME 门之前")
    check(handle.count("screen_editor_vi_handle(") == 1,
          "screen_editor_handle 中 vi 分发只应有一个入口")

# ── 2. 初始化 ─────────────────────────────────────────────────────────────
init = between(editor, "void screen_editor_init(", "AppState ")
check(init is not None and "g_editor.vi = ViState{}" in init,
      "screen_editor_init 必须重置 g_editor.vi = ViState{}")
check(init is not None and "g_settings.viMode()" in init,
      "screen_editor_init 必须根据 g_settings.viMode() 选择初始模式")

# ── 3. 行号槽与折行缓存 ───────────────────────────────────────────────────
check("cachedGutterCells" in editor, "getVrows 缓存必须包含 cachedGutterCells")
check("g_editor.cachedGutterCells != gutter" in editor,
      "getVrows 必须在行号槽宽度变化时失效缓存")
check("editorGutterCells()" in editor, "编辑器折行必须按 editorGutterCells() 预留行号槽")
check("buildVrows(g_editor.lines" in editor, "编辑器必须通过 buildVrows 排版")
check("reserveCells" in ui_h and "int reserveCells = 0" in ui_h,
      "buildVrows 声明必须包含默认 0 的 reserveCells 参数")
check("SCREEN_W / g_font.halfAdvance() - reserveCells" in ui_cpp,
      "buildVrows 必须从 maxc 中减去 reserveCells")

# ── 4. 多行插入路径 ───────────────────────────────────────────────────────
def definition(text, sig):
    """取签名最后一次出现(定义)到下一个顶格右花括号的函数体。"""
    i = text.rfind(sig)
    if i < 0:
        return None
    j = text.find("\n}\n", i + len(sig))
    return None if j < 0 else text[i:j]


for fn, name in (("static void insertMultilineAt(", "insertMultilineAt"),
                 ("static void insertLinesAt(", "insertLinesAt")):
    body = definition(editor, fn)
    check(body is not None and "foldLinesInserted" in body,
          f"{name} 必须维护折叠行号 (foldLinesInserted)")

insert_text = between(editor, "void editorInsertText(", "\n}\n")
check(insert_text is not None and "insertMultilineAt(" in insert_text,
      "editorInsertText 必须走 insertMultilineAt 以支持多行上屏")
replace_sel = between(editor, "void editorReplaceSelection(", "\n}\n")
check(replace_sel is not None and "insertMultilineAt(" in replace_sel,
      "editorReplaceSelection 必须走 insertMultilineAt")
check(editor.count("insertMultilineAt(") >= 5,
      "vi 粘贴/替换等多处必须复用 insertMultilineAt")
check(editor.count("insertLinesAt(") >= 3,
      "整行插入(vi 行粘贴 / visual 替换)必须复用 insertLinesAt")

# ── 5. ViState 不含每行容器 ──────────────────────────────────────────────
vi_state = between(editor, "struct ViState {", "};")
check(vi_state is not None, "找不到 ViState 定义")
if vi_state is not None:
    for bad in ("vector<", "std::map", "unordered_map"):
        check(bad not in vi_state, f"ViState 不应包含每行容器: {bad}")

# ── 6. 设置 ──────────────────────────────────────────────────────────────
check("bool showLineNumbers();" in settings_h, "settings_manager.h 缺少 showLineNumbers()")
check("bool viMode();" in settings_h, "settings_manager.h 缺少 viMode()")
check('"show_line_numbers"' in settings_cpp, "settings_manager.cpp 缺少 show_line_numbers getter")
check('"vi_mode"' in settings_cpp, "settings_manager.cpp 缺少 vi_mode getter")
check('{"show_line_numbers", "显示行号"' in settings_screen,
      "设置页缺少 显示行号 开关")
check('{"vi_mode", "Vi 模式"' in settings_screen, "设置页缺少 Vi 模式 开关")
for key in ("show_line_numbers", "vi_mode"):
    check(key in between(settings_screen, "isToggleField", "\n}"),
          f"isToggleField 必须包含 {key}")

# ── 7. main.cpp 全局键守卫 ───────────────────────────────────────────────
check("app_editor_vi_ime_blocked()" in main_cpp,
      "main.cpp 必须用 app_editor_vi_ime_blocked() 拦住 Ctrl+Space")
check(main_cpp.count("app_editor_vi_cmdline_active()") >= 2,
      "main.cpp 必须用 app_editor_vi_cmdline_active() 拦住 Ctrl+I 与物理按键浮层")
check("app_editor_vi_ime_blocked()" in read(EDITOR_H), "screen_editor.h 未导出 vi 守卫")
check("app_editor_vi_cmdline_active()" in read(EDITOR_H),
      "screen_editor.h 未导出 cmdline 守卫")

# ── 结果 ─────────────────────────────────────────────────────────────────
if failures:
    print("check_editor_vi: FAILED")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)

print("check_editor_vi: OK")
