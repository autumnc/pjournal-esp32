#pragma once
#include <string>

// Global clipboard shared between editor, inspiration panel, etc.
extern std::string g_clipboard;
// 剪贴板内容是否为整行(vi linewise)。跨编辑器重开保留,由 clipboardSet() 统一维护。
extern bool g_clipboard_linewise;

// 设置当前剪贴板。recordHistory=true 时同时写入 PSRAM 剪贴板历史；
// 从历史菜单里选中条目时传 false，避免再次入栈/重排。
void clipboardSet(const std::string &text, bool linewise, bool recordHistory = true);

// 单行输入框用：把 \n \r \t 折成空格。
std::string clipboardFlattenSingleLine(const std::string &text);
