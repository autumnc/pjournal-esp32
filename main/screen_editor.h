#pragma once

#include "pjournal_app.h"

// Editor screen entry points
void screen_editor_init(ScreenContext &ctx);
AppState screen_editor_handle(int key, ScreenContext &ctx);

// Idle tick (no key): runs auto-save, repaints only if the screen is stale or
// forceRedraw is set. Returns true if a repaint happened.
bool screen_editor_idle(ScreenContext &ctx, bool forceRedraw);

// Mark the on-screen editor content stale (call when another screen painted
// over it, e.g. returning from inspiration/polish/voice).
void screen_editor_reset_drawn();

// IME state for global Ctrl+Space toggle
bool app_ime_active();
void app_toggle_ime();
bool app_ime_fullwidth();
void app_toggle_fullwidth();
void app_toggle_trad();
void app_toggle_english();
void app_toggle_ime_delete_mode();

// Force editor re-initialization on next cycle
void app_editor_request_reinit();
bool app_editor_needs_reinit();

// Park/restore the current editor while a modal flow reuses the editor screen.
void app_editor_stash_session();
void app_editor_restore_stashed_session();
bool app_editor_has_stashed_session();

// Win 快捷菜单(shortcut_menu.cpp)的可见性与打开条件
bool app_editor_shortcut_menu_allowed();
bool app_editor_prompt_mode();
bool app_editor_template_journal();
// 天气请求进行中：屏蔽会并发使用 WiFi 的物理按键入口（语音听写/AI 润色）。
bool app_editor_weather_fetching();
bool app_editor_in_quick_file_session();
bool app_editor_in_file_edit_session();
bool app_editor_has_selection();
bool app_editor_is_empty();

// 查找/替换对话框是否打开(供 main.cpp 屏蔽全局按键/物理按键)
bool app_editor_search_active();

// 快捷键帮助对话框是否打开(供 main.cpp 屏蔽全局按键/物理按键)
bool app_editor_help_active();

// vi normal/visual 或命令行激活时, 屏蔽全局输入法切换
bool app_editor_vi_ime_blocked();
// vi 命令行(: / ?)是否激活, 屏蔽全局 Ctrl+I 与物理按键浮层
bool app_editor_vi_cmdline_active();

// Get editor text for Flomo sending
std::string app_get_editor_text();

// Insert text at the cursor (shared by IME commit and voice dictation)
void editorInsertText(const std::string &text);

// 剪贴板历史菜单粘贴入口：把 text 插到当前输入焦点（插入模式/查找替换/
// 文件改名/vi 搜索命令行，vi normal/visual 按 p/P 语义）。返回是否插入。
bool app_editor_paste_text(const std::string &text);

// Replace the entire editor text (used by AI polish confirm). Cursor → end.
void editorReplaceAllText(const std::string &text);

// Currently selected text (empty when no selection).
std::string app_get_selected_text();

// Replace only the selected text (selection polish confirm). Cursor → end of
// the inserted text; selection cleared.
void editorReplaceSelection(const std::string &text);

// Draw only the editor content as a transparent background (used by voice screen)
void screen_editor_draw_voice_bg();
