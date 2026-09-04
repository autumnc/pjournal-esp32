#pragma once

#include <string>
#include <vector>
#include <set>
#include "font_renderer.h"

class IME;

struct MdLineInfo;

// Forward declaration matching u8g2.h so extern declarations can use u8g2_t*
struct u8g2_struct;
typedef struct u8g2_struct u8g2_t;

// VRow structure for word-wrap rendering
struct VRow { int lineIdx; int start; int end; int indentCells = 0; };

// UI constants - screen dimensions are fixed
#define SCREEN_W 400
#define SCREEN_H 300
#define STATUS_BAR_FONT_SIZE 22
#define STATUS_BAR_H 22
#define STATUS_BAR_Y (SCREEN_H - STATUS_BAR_H - 2)

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
void ui_invalidate_snapshot();
int  ui_text_width(const char *text);
void ui_draw_text(int x, int y, const char *text, bool invert = false, bool bold = false);
void ui_draw_text_centered(int y, const char *text, bool invert = false, bool bold = false);
void ui_show_message(const char *msg, int duration = 2000);
void ui_show_message_centered(const char *msg);

// Battery ADC
void battery_init();
int battery_pct();
// 设备电池文本:"[电池图标] 电量";电量未知时返回空串
std::string battery_text();
// 设备电量 + 有蓝牙键盘时追加 " [蓝牙图标] 键盘电量"
std::string battery_status_text();
// 纯图标电量:设备电平图标(放电) + 蓝牙键盘电平图标,无数字
std::string battery_icon_text();
std::string battery_icon_status_text();

// Word-wrap builder. mdInfoIn: precomputed per-line markdown info (skips
// internal mdClassifyLines). foldedHeadings: heading line indices whose body
// is collapsed — their vrows are omitted (editor view-state, viewer passes
// defaults).
std::vector<VRow> buildVrows(const std::vector<std::string> &lines,
                             const std::vector<MdLineInfo> *mdInfoIn = nullptr,
                             const std::set<int> *foldedHeadings = nullptr);

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
