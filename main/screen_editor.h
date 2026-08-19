#pragma once

#include "pjournal_app.h"

// Editor screen entry points
void screen_editor_init(ScreenContext &ctx);
AppState screen_editor_handle(int key, ScreenContext &ctx);

// IME state for global Ctrl+Space toggle
bool app_ime_active();
void app_toggle_ime();
bool app_ime_fullwidth();
void app_toggle_fullwidth();
void app_toggle_trad();
void app_toggle_english();

// Force editor re-initialization on next cycle
void app_editor_request_reinit();
bool app_editor_needs_reinit();

// Get editor text for Flomo sending
std::string app_get_editor_text();

// Insert text at the cursor (shared by IME commit and voice dictation)
void editorInsertText(const std::string &text);

// Replace the entire editor text (used by AI polish confirm). Cursor → end.
void editorReplaceAllText(const std::string &text);

// Currently selected text (empty when no selection).
std::string app_get_selected_text();

// Replace only the selected text (selection polish confirm). Cursor → end of
// the inserted text; selection cleared.
void editorReplaceSelection(const std::string &text);

// Draw only the editor content as a transparent background (used by voice screen)
void screen_editor_draw_voice_bg();
