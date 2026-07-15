#pragma once

#include "pjournal_app.h"

// Editor screen entry points
void screen_editor_init(ScreenContext &ctx);
AppState screen_editor_handle(int key, ScreenContext &ctx);

// IME state for global Ctrl+Space toggle
bool app_ime_active();
void app_toggle_ime();

// Get editor text for Flomo sending
std::string app_get_editor_text();
