#pragma once

#include "pjournal_app.h"

// 使用说明（主界面 [?] 帮助）。目录 + 分章节阅读，支持上下/翻页滚动。
void screen_help_init();
AppState screen_help_handle(int key, ScreenContext &ctx);
