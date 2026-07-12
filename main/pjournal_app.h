#pragma once

#include <string>
#include <vector>
#include "font_renderer.h"
#include "journal_storage.h"

// App state enumeration
enum AppState {
    APP_MAIN,
    APP_EDITOR,
    APP_BROWSER,
    APP_VIEWER,
    APP_SETTINGS,
    APP_PROMPT_SEL,
    APP_SYNC_WEBDAV,
    APP_SYNC_SEND_FLOMO,
    APP_BT_MANAGE,
    APP_QUIT,
};

// Screen context passed between screens
struct ScreenContext {
    AppState nextState = APP_MAIN;
    std::string selectedEntry;    // for viewer
    std::string promptText;       // for editor
    bool promptMode = false;      // true = prompt writing, false = free writing
    std::string statusMessage;    // one-shot status message to show
    int statusDuration = 0;       // ticks to show status message
};

// Shared UI constants
#define SCREEN_W 400
#define SCREEN_H 300
#define FONT_H 28
#define STATUS_H 28
#define VISIBLE_LINES ((SCREEN_H - STATUS_H) / FONT_H)  // ~9 lines
#define LINE_SPACING 28

// Arrow key codes (must match bt_keyboard.cpp)
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_IME_TOGGLE 0x84

// UI helper functions
void ui_draw_status(const char *left, const char *right);
void ui_draw_title(const char *title);
void ui_clear();
void ui_commit();
int  ui_text_width(const char *text);
void ui_draw_text(int x, int y, const char *text, bool invert = false, bool bold = false);
void ui_draw_text_centered(int y, const char *text, bool invert = false, bool bold = false);
void ui_show_message(const char *msg, int duration = 2000);
void ui_show_message_centered(const char *msg);

// Screen entry points called from app loop
void screen_main_init();
AppState screen_main_handle(int key, ScreenContext &ctx);

void screen_editor_init(const ScreenContext &ctx);
AppState screen_editor_handle(int key, ScreenContext &ctx);

void screen_browser_init();
AppState screen_browser_handle(int key, ScreenContext &ctx);

void screen_viewer_init(const std::string &filename);
AppState screen_viewer_handle(int key, ScreenContext &ctx);

void screen_settings_init();
AppState screen_settings_handle(int key, ScreenContext &ctx);

void screen_bt_manage_init();
AppState screen_bt_manage_handle(int key, ScreenContext &ctx);

// Get editor text for Flomo sending
std::string app_get_editor_text();

// IME state for global Ctrl+Space toggle
bool app_ime_active();
void app_toggle_ime();

// Battery ADC
void battery_init();
int battery_pct();
