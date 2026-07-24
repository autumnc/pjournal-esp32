#pragma once

#include <string>
#include <vector>
#include "font_renderer.h"

class IME;

// VRow structure for word-wrap rendering
struct VRow { int lineIdx; int start; int end; };

// UI constants - screen dimensions are fixed
#define SCREEN_W 400
#define SCREEN_H 300

// Font-dependent metrics (dynamic via g_font)
#define FONT_H (g_font.lineHeight())
#define STATUS_H (g_font.lineHeight())
#define VISIBLE_LINES ((SCREEN_H - STATUS_H) / FONT_H)
#define LINE_SPACING (g_font.lineHeight() + 4)
#define STATUS_Y (SCREEN_H - FONT_H - 2)

// IME singleton (defined in ui_helpers.cpp)
extern IME &g_ime;

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

// Battery ADC
void battery_init();
int battery_pct();

// Word-wrap builder
std::vector<VRow> buildVrows(const std::vector<std::string> &lines);

// IME drawing helper
void drawIMEUI(int baseY);

// WiFi helper functions
bool ensure_wifi_connected();
void restore_wifi_state(bool wasConnected);

// NTP time sync helper
bool syncNtpTime(const std::string &ntpServer, const std::string &timezone);

// Word-wrap cell conversion helpers
int byteToCells(const std::string &line, int byteOffset);
int cellsToByte(const std::string &line, int start, int end, int targetCells);

// Word/body helpers
int countVisibleChars(const std::string &text);
std::string extractBody(const std::string &content);
