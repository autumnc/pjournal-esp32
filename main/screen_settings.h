#pragma once

#include "pjournal_app.h"

// Settings screen entry points
void screen_settings_init();
AppState screen_settings_handle(int key, ScreenContext &ctx);
// 剪贴板历史菜单粘贴入口（设置项编辑/词库搜索/词库添加）
bool screen_settings_paste_text(const std::string &text);
