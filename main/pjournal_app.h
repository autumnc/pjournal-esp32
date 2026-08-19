#pragma once

#include <string>
#include "ui_helpers.h"

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
    APP_FILE_MANAGER,
    APP_GTD,
    APP_OUTLINE,
    APP_INSPIRATION,
    APP_VOICE,
    APP_POLISH,
    APP_POLISH_PROMPT,
    APP_QUIT,
};

// Screen context passed between screens
struct ScreenContext {
    AppState nextState = APP_MAIN;
    std::string selectedEntry;    // for viewer
    std::string promptText;       // for editor
    bool promptMode = false;      // true = prompt writing, false = free writing
    std::string editContent;      // body text to load into editor (from browser)
    std::string editFilename;     // original filename when editing existing entry
    std::string statusMessage;    // one-shot status message to show
    int statusDuration = 0;       // ticks to show status message
    AppState prevState = APP_MAIN;
};

// Arrow key codes (must match bt_keyboard.cpp)
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_IME_TOGGLE 0x84
#define KEY_CTRL_ENTER 0x85
#define KEY_SHIFT_UP    0x86
#define KEY_SHIFT_DOWN  0x87
#define KEY_SHIFT_LEFT  0x88
#define KEY_SHIFT_RIGHT 0x89
#define KEY_CTRL_I      0x8A
#define KEY_FULLWIDTH_TOGGLE 0x8B
#define KEY_TRAD_TOGGLE 0x8C
#define KEY_LSHIFT_TAP 0x8D
// Ctrl+0-9 → 快捷编辑文件切换 (0x90-0x99)
#define KEY_FILE_BASE 0x90

// Screen entry points (screens that remain in pjournal_app.cpp)
void screen_main_init();
AppState screen_main_handle(int key, ScreenContext &ctx);

void screen_browser_init();
AppState screen_browser_handle(int key, ScreenContext &ctx);

void screen_viewer_init(const std::string &filename);
AppState screen_viewer_handle(int key, ScreenContext &ctx);

// GTD / Outline screens
void screen_gtd_init();
AppState screen_gtd_handle(int key, ScreenContext &ctx);

void screen_outline_init();
AppState screen_outline_handle(int key, ScreenContext &ctx);

// Inspiration screen
void screen_inspiration_init(AppState returnTo);
AppState screen_inspiration_handle(int key, ScreenContext &ctx);

// Voice dictation screen
void screen_voice_init();
AppState screen_voice_handle(int key, ScreenContext &ctx);

// Flomo send text (set by browser/viewer before entering APP_SYNC_SEND_FLOMO)
extern std::string g_flomoPendingText;
extern AppState g_flomoReturnTo;
