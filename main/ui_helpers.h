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
// 只发送缓冲不更新快照(休眠提示用:唤醒后需按快照恢复,快照须保持提示前画面)
void ui_send_buffer();
// 用快照恢复缓冲并整屏发送(休眠唤醒后清除"休眠中"提示)
void ui_restore_snapshot();
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
                             const std::set<int> *foldedHeadings = nullptr,
                             int reserveCells = 0);

// ── Vertical placement for horizontal (横排) layout ───────────────────────
// 日记格式段落之间不加空行:每个硬换行就是一个段落。相邻两行都是非空普通
// 正文时,第二个段落在正常行距之外再多留 PARAGRAPH_GAP,否则(空行/Markdown
// 结构行/段内折行)保持原行距。以下函数把这一额外间距计入行位置与滚动。
#define PARAGRAPH_GAP ((g_font.lineHeight() + 1) / 2)

// 第 idx 个 vrow 前额外留出的像素(0 = 正常行距)。
int vrowGapBefore(const std::vector<VRow> &vrows, int idx,
                  const std::vector<MdLineInfo> *mdInfo);

// 从 slot `scroll` 起能完整放入 height 像素的 vrow 槽数。scroll 可为负
// (打字机模式顶部留白),负槽按固定行距占位。
int vrowFitCount(const std::vector<VRow> &vrows, int scroll, int height,
                 const std::vector<MdLineInfo> *mdInfo);

// 让第 idx 行成为可视区末行的最小起始 slot(末尾恰好填满 height)。
int vrowScrollToShow(const std::vector<VRow> &vrows, int idx, int height,
                     const std::vector<MdLineInfo> *mdInfo);

// 让第 idx 行尽量居于 height 像素正中,返回起始 slot(可为负,补足顶部留白)。
int vrowScrollCenter(const std::vector<VRow> &vrows, int idx, int height,
                     const std::vector<MdLineInfo> *mdInfo);

// 第 idx 行基线相对 slot scroll 的像素偏移(计入段落间距与负 scroll 留白)。
int vrowOffsetFrom(const std::vector<VRow> &vrows, int scroll, int idx,
                   const std::vector<MdLineInfo> *mdInfo);

// IME drawing helpers
int imeStatusPanelTopY();
int imeFullscreenPanelTopY();
int imeCandidateLineWidth();
std::string imeStatusLabel(bool active);
void drawIMEUI(int baseY, bool anchorBottom = false);
void drawIMEUIWithStatusBar();
void drawIMEUIFullscreen();

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
