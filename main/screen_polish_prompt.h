#pragma once

#include "pjournal_app.h"

// 润色提示词多行编辑面板(设置 → 润色提示词)。
// Ctrl+S 保存到 g_settings.polishPrompt(),Esc 取消,均返回 APP_SETTINGS。
void screen_polish_prompt_init();
AppState screen_polish_prompt_handle(int key, ScreenContext &ctx);
// 剪贴板历史菜单粘贴入口（提示词多行编辑区，保留换行）
bool screen_polish_prompt_paste_text(const std::string &text);
