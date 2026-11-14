#pragma once

#include "pjournal_app.h"

void screen_outline_init();
AppState screen_outline_handle(int key, ScreenContext &ctx);
// 剪贴板历史菜单粘贴入口（标题/字段/筛选/备注）
bool screen_outline_paste_text(const std::string &text);
