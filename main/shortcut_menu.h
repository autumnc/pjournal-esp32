#pragma once

// 编辑器 Win 快捷菜单（单按左 Win 弹出，显示快捷键 + 功能）。
//
// 只在 APP_EDITOR 下使用；打开条件见 screen_editor.cpp 的
// app_editor_shortcut_menu_allowed()。菜单项按当前模式动态隐藏。

enum class ShortcutMenuAction {
    None = 0,
    Save,
    Clipboard,
    Inspiration,
    Flomo,
    Search,
    Outline,
    MarkdownToggle,
    Weather,
    History,
    Prompt,
    PolishSelection,
    PolishAll,
    FileManager,
    Voice,
    Help,
    Exit,
};

bool shortcut_menu_active();

// 打开失败（无 PSRAM 底板）返回 false，调用方按原按键继续处理。
bool shortcut_menu_open();

// restore=true 时把底板整帧写回屏幕。总会让编辑器下个空闲节拍重绘。
void shortcut_menu_close(bool restore);

// 返回 None 表示菜单保持打开；否则返回要执行的动作（菜单已关闭）。
ShortcutMenuAction shortcut_menu_handle(int key);
