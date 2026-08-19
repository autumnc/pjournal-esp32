#pragma once

#include "pjournal_app.h"

// Editor screen entry points
void screen_editor_init(ScreenContext &ctx);
AppState screen_editor_handle(int key, ScreenContext &ctx);

// Idle tick (no key): runs auto-save, repaints only if the screen is stale or
// forceRedraw is set. Returns true if a repaint happened.
bool screen_editor_idle(ScreenContext &ctx, bool forceRedraw);

// Mark the on-screen editor content stale (call when another screen painted
// over it, e.g. returning from inspiration/polish/voice).
void screen_editor_reset_drawn();

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
