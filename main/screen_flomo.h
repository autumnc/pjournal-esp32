#pragma once

#include "pjournal_app.h"

// Flomo 笔记（本地库 + 手动同步）。本机无触摸：新建/同步/编辑/删除全部走快捷键，
// 正文编辑借用本仓写作编辑器。
void screen_flomo_init(AppState returnTo);
AppState screen_flomo_handle(int key, ScreenContext &ctx);
