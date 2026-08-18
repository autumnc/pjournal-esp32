#pragma once

#include "pjournal_app.h"

// AI polish panel: editor BOOT double-click → DeepSeek text polish.
// Result page with confirm / re-polish / cancel.
void screen_polish_init();
AppState screen_polish_handle(int key, ScreenContext &ctx);
