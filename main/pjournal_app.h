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

// Screen entry points (screens that remain in pjournal_app.cpp)
void screen_main_init();
AppState screen_main_handle(int key, ScreenContext &ctx);

void screen_browser_init();
AppState screen_browser_handle(int key, ScreenContext &ctx);

void screen_viewer_init(const std::string &filename);
AppState screen_viewer_handle(int key, ScreenContext &ctx);
