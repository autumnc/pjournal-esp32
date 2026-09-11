#include "screen_voice.h"
#include "voice_input.h"
#include "screen_editor.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "font_renderer.h"

#include <cstring>
#include <string>

#include <esp_timer.h>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
}

// True when g_voice.start() failed (a previous session is still stopping).
static bool s_start_failed = false;
static int64_t s_start_failed_until_us = 0;
static int64_t s_last_start_retry_us = 0;

// Truncate a UTF-8 string so its rendered width (at the strip's 22pt font)
// fits within maxW. Whole code points are dropped so no partial glyphs.
static void fitText(std::string &s, int maxW) {
    if (g_font.textWidth(s.c_str()) <= maxW) return;
    while (!s.empty()) {
        int len = (int)s.size();
        unsigned char b = (unsigned char)s[len - 1];
        int drop;
        if (b < 0x80) drop = 1;
        else if ((b & 0xE0) == 0xC0) drop = 2;
        else if ((b & 0xF0) == 0xE0) drop = 3;
        else drop = 4;
        s.erase(len - drop);
        if (g_font.textWidth(s.c_str()) <= maxW) break;
    }
}

// Status-bar-height strip at the status bar position. The editor content
// (already drawn as the transparent background) stays visible around it.
static void drawVoiceStrip(const std::string &left, const std::string &right) {
    int prev = g_font.fontSize();
    g_font.setSize(22);
    std::string l = left, r = right;
    int rw = g_font.textWidth(r.c_str());
    fitText(l, SCREEN_W - rw - 12);
    ui_draw_status(l.c_str(), r.empty() ? nullptr : r.c_str());
    if (g_font.fontSize() != prev) g_font.setSize(prev);
}

void screen_voice_init() {
    s_start_failed = false;
    s_last_start_retry_us = 0;
    if (!g_voice.start()) {
        // A previous session is still stopping; keep retrying in the handle loop.
        s_start_failed = true;
        s_start_failed_until_us = esp_timer_get_time() + 15000 * 1000;
    }
}

AppState screen_voice_handle(int key, ScreenContext &ctx) {
    // USER double-click (ESC injected by main) → stop & return to the editor.
    if (key == 0x1B) {
        if (!s_start_failed) {
            g_voice.requestStop();
            std::string t;
            while (g_voice.popStt(t)) editorInsertText(t);
            if (g_settings.voiceAsrService() == "baidu" && g_voice.isActive()) {
                return APP_VOICE;
            }
        }
        s_start_failed = false;
        return APP_EDITOR;
    }

    if (s_start_failed) {
        // Retry start() every ~200ms until the previous session fully exits.
        int64_t now = esp_timer_get_time();
        if (now - s_last_start_retry_us > 200 * 1000) {
            s_last_start_retry_us = now;
            if (g_voice.start()) s_start_failed = false;
        }
        if (s_start_failed && esp_timer_get_time() >= s_start_failed_until_us) {
            s_start_failed = false;
            return APP_EDITOR;
        }
        ui_clear();
        screen_editor_draw_voice_bg();
        drawVoiceStrip("语音模块忙,请稍候...", "双击退出");
        ui_commit();
        return APP_VOICE;
    }

    // Drain recognized text into the editor as it arrives.
    std::string t;
    while (g_voice.popStt(t)) editorInsertText(t);

    // Session ended on its own (clean stop or error hold expired).
    if (!g_voice.isActive()) return APP_EDITOR;

    std::string left, right;
    switch (g_voice.state()) {
    case VOICE_CONNECTING_WIFI:
        left = "连接WiFi中...";
        break;
    case VOICE_CONNECTING_SERVER:
        left = "连接服务器...";
        break;
    case VOICE_WAIT_ACTIVATE:
    case VOICE_ACTIVATING: {
        std::string code = g_voice.activationCode();
        if (!code.empty()) left = "绑定码: " + code;
        else left = "绑定中...";
        right = "xiaozhi.me";
        break;
    }
    case VOICE_LISTENING:
        left = "语音识别中...";
        right = "双击退出";
        break;
    case VOICE_STOPPING:
        left = "正在停止...";
        break;
    case VOICE_ERROR:
        left = g_voice.errorMessage();
        right = "双击退出";
        break;
    default:
        left = "...";
        break;
    }

    ui_clear();
    screen_editor_draw_voice_bg();
    drawVoiceStrip(left, right);
    ui_commit();
    return APP_VOICE;
}
