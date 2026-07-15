#include "pjournal_app.h"
#include "font_renderer.h"
#include "bt_keyboard.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "journal_storage.h"
#include "webdav_client.h"
#include "flomo_client.h"
#include "deepseek_client.h"
#include "ime/IME.h"
#include "builtin_prompts.h"
#include "pcf85063.h"
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <cctype>
#include <vector>
#include <utility>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

extern void *g_u8g2;

extern "C" {
    extern void u8g2_ClearBuffer(void *u8g2);
    extern void u8g2_SendBuffer(void *u8g2);
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *u8g2, int x, int y, int w, int h);
}

#define STATUS_Y (SCREEN_H - FONT_H - 2)  // 270, separator + white bg below
#define IME_CODE_Y (STATUS_Y - 2*FONT_H + 22)  // 234, content fills to here
#define IME_CAND_Y (STATUS_Y - FONT_H + 19)     // 253

// Editor word wrap: max display cells per visual row (ASCII=1, CJK=2)
// ASCII=14px cell, CJK=28px. Screen=400px, start at x=4 → usable 396px.
// 396/14 ≈ 28 ASCII chars per line.
#define EDITOR_MAX_CELLS 28

// ── Battery ADC ──────────────────────────────────────────
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_battery_inited = false;

void battery_init() {
    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.atten = ADC_ATTEN_DB_12;
    cali_config.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali_handle) != ESP_OK) return;
    adc_oneshot_unit_init_cfg_t init_config1 = {};
    init_config1.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&init_config1, &s_adc_handle) != ESP_OK) return;
    adc_oneshot_chan_cfg_t config = {};
    config.bitwidth = ADC_BITWIDTH_12;
    config.atten = ADC_ATTEN_DB_12;
    if (adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_3, &config) != ESP_OK) return;
    s_battery_inited = true;
}

int battery_pct() {
    if (!s_battery_inited) return -1;
    static int64_t last_read_us = 0;
    static int cached = -1;
    int64_t now = esp_timer_get_time();
    if (last_read_us != 0 && (now - last_read_us) < 5000000)
        return cached;
    last_read_us = now;
    int raw;
    if (adc_oneshot_read(s_adc_handle, ADC_CHANNEL_3, &raw) != ESP_OK) return cached;
    int mv;
    if (adc_cali_raw_to_voltage(s_cali_handle, raw, &mv) != ESP_OK) return cached;
    float v = mv * 0.001f * 3.0f;
    // Li-ion discharge curve lookup (voltage → %)
    static const float lut[][2] = {
        {4.12f, 100}, {4.08f, 92}, {4.02f, 82},
        {3.96f, 70},  {3.90f, 58}, {3.84f, 47},
        {3.78f, 37},  {3.72f, 28}, {3.66f, 20},
        {3.60f, 14},  {3.54f, 9},  {3.48f, 6},
        {3.40f, 3},   {3.30f, 1},  {3.00f, 0},
    };
    if (v >= lut[0][0]) { cached = (int)lut[0][1]; }
    else if (v <= lut[14][0]) { cached = 0; }
    else {
        for (int i = 0; i < 14; i++) {
            if (v >= lut[i+1][0] && v < lut[i][0]) {
                float t = (v - lut[i+1][0]) / (lut[i][0] - lut[i+1][0]);
                cached = (int)(lut[i+1][1] + t * (lut[i][1] - lut[i+1][1]) + 0.5f);
                break;
            }
        }
    }
    return cached;
}

// ── Word wrap helpers ────────────────────────────────────
static int charCellWidth(unsigned char c) {
    return (c < 0x80) ? 1 : 2;
}

static int utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Convert byte offset to cell column position (ASCII=1, CJK=2)
static int byteToCells(const std::string &line, int byteOffset) {
    int cells = 0;
    for (int i = 0; i < byteOffset; ) {
        cells += charCellWidth((unsigned char)line[i]);
        i += utf8CharLen((unsigned char)line[i]);
    }
    return cells;
}

// Find byte position in line[start..end) closest to targetCells cell-column
static int cellsToByte(const std::string &line, int start, int end, int targetCells) {
    int cells = 0;
    for (int ci = start; ci < end; ) {
        unsigned char c = (unsigned char)line[ci];
        int cc = charCellWidth(c);
        if (cells + cc > targetCells) {
            return (targetCells - cells <= cells + cc - targetCells) ? ci : ci + utf8CharLen(c);
        }
        cells += cc;
        ci += utf8CharLen(c);
    }
    return end;
}

struct VRow { int lineIdx; int start; int end; };

static std::vector<VRow> buildVrows(const std::vector<std::string> &lines) {
    std::vector<VRow> vrows;
    for (int li = 0; li < (int)lines.size(); li++) {
        const auto &line = lines[li];
        int len = (int)line.length();
        if (len == 0) {
            vrows.push_back({li, 0, 0});
            continue;
        }
        int pos = 0;
        while (pos < len) {
            int cells = 0;
            int end = pos;
            int lastBreak = -1;
            while (end < len) {
                unsigned char c = (unsigned char)line[end];
                int cc = charCellWidth(c);
                if (cells + cc > EDITOR_MAX_CELLS) break;
                cells += cc;
                if (c == ' ') lastBreak = end + 1;
                if (c < 0x80) end++;
                else if ((c & 0xE0) == 0xC0) end += 2;
                else if ((c & 0xF0) == 0xE0) end += 3;
                else if ((c & 0xF8) == 0xF0) end += 4;
                else end++;
            }
            if (end >= len) {
                vrows.push_back({li, pos, len});
                break;
            }
            if (lastBreak > pos) {
                vrows.push_back({li, pos, lastBreak});
                pos = lastBreak;
                while (pos < len && line[pos] == ' ') pos++;
            } else {
                vrows.push_back({li, pos, end});
                pos = end;
            }
        }
    }
    return vrows;
}

// IME singleton
static IME &g_ime = IME::getInstance();

// ── Forward declarations of helpers ────────────────────────────────────
static void drawEditor();
static AppState finishEditor(ScreenContext &ctx);
static int countVisibleChars(const std::string &text);
static std::string extractBody(const std::string &content);

// ── Screen state ───────────────────────────────────────────────────────
static struct { std::vector<std::string> lines; int cx = 0, cy = 0; int scroll = 0;
    int targetCx = -1;
    std::string promptText; bool promptMode = false; bool imeActive = false;
    bool confirmSave = false; bool vrowsDirty = true;
    std::vector<VRow> cachedVrows;
    int cachedWordCount = 0; bool wordCountDirty = true; } g_editor;

// Cached vrows accessor for editor
static const std::vector<VRow>& getVrows() {
    if (g_editor.vrowsDirty) {
        g_editor.cachedVrows = buildVrows(g_editor.lines);
        g_editor.vrowsDirty = false;
    }
    return g_editor.cachedVrows;
}

// Cached word count accessor for editor
static int getWordCount() {
    if (g_editor.wordCountDirty) {
        std::string fullText;
        for (auto &l : g_editor.lines) { if (!fullText.empty()) fullText += '\n'; fullText += l; }
        g_editor.cachedWordCount = countVisibleChars(fullText);
        g_editor.wordCountDirty = false;
    }
    return g_editor.cachedWordCount;
}

static struct { int selection = 0; int scroll = 0; } g_browser;
static struct { int selection = 0; int scroll = 0; bool scanning = true;
    bool connecting = false; int64_t conn_start_ms = 0;
    char statusMsg[64]; } g_btState;
static struct { std::vector<std::string> lines; int scroll = 0; std::string filename;
    std::string dateStr;
    std::vector<VRow> cachedVrows; bool vrowsDirty = true; } g_viewer;
static struct { int selection = 0; int scroll = 0; bool editing = false;
    std::string editBuffer; int editCursor = 0; bool imeActive = false; } g_settingsState;

// Cached vrows accessor for viewer
static const std::vector<VRow>& getViewerVrows() {
    if (g_viewer.vrowsDirty) {
        g_viewer.cachedVrows = buildVrows(g_viewer.lines);
        g_viewer.vrowsDirty = false;
    }
    return g_viewer.cachedVrows;
}

// IME drawing helper - draws IME UI at specified Y position
static void drawIMEUI(int baseY) {
    if (!g_ime.composing()) return;

    std::string code = g_ime.displayCode();
    auto &cands = g_ime.candidates();
    int pageSize = g_ime.pageSize();
    int curPage = g_ime.currentPage();
    int total = g_ime.totalCandidates();
    int totalPages = (total + pageSize - 1) / pageSize;
    if (totalPages < 1) totalPages = 1;

    // Code line with page indicator
    char pageInfo[32];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);

    // Draw background
    u8g2_DrawBox(g_u8g2, 0, baseY, SCREEN_W, 67);
    u8g2_SetDrawColor(g_u8g2, 1);

    // Draw code (white box, black text)
    int cw = g_font.textWidth(code.c_str()) + 8;
    u8g2_DrawBox(g_u8g2, 4, baseY + 4, cw, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText(4, baseY + 4 + FONT_H - 6, code.c_str(), false);
    u8g2_SetDrawColor(g_u8g2, 1);

    // Draw page indicator
    int tw = g_font.textWidth(pageInfo);
    int pw = tw + 8;
    int px = SCREEN_W - pw - 4;
    u8g2_DrawBox(g_u8g2, px, baseY + 4, pw, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText(px + 4, baseY + 4 + FONT_H - 6, pageInfo, false);
    u8g2_SetDrawColor(g_u8g2, 1);

    // Separator line
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, baseY + FONT_H + 4, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);

    // Candidates inline (horizontal)
    std::string candLine;
    for (int i = 0; i < (int)cands.size(); i++) {
        char idx[16];
        snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);
        std::string part = std::string(" ") + idx + cands[i];
        int curW = g_font.textWidth(candLine.c_str());
        int partW = g_font.textWidth(part.c_str());
        if (curW + partW + 8 > SCREEN_W) break;
        candLine += part;
    }
    if (!candLine.empty()) {
        int candW = g_font.textWidth(candLine.c_str()) + 8;
        u8g2_DrawBox(g_u8g2, 4, baseY + FONT_H + 8, candW, FONT_H);
        u8g2_SetDrawColor(g_u8g2, 0);
        g_font.drawText(4, baseY + FONT_H + 8 + FONT_H - 6, candLine.c_str(), false);
        u8g2_SetDrawColor(g_u8g2, 0);
    }
}

// ── UI Helpers ─────────────────────────────────────────────────────────
void ui_clear() {
    if (g_u8g2) { u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawBox(g_u8g2, 0, 0, SCREEN_W, SCREEN_H);
        u8g2_SetDrawColor(g_u8g2, 0); }
}

void ui_commit() { if (g_u8g2) u8g2_SendBuffer(g_u8g2); }

int ui_text_width(const char *text) { return g_font.textWidth(text); }

void ui_draw_text(int x, int y, const char *text, bool invert, bool bold) {
    if (invert) {
        int w = g_font.textWidth(text);
        int bh = g_font.lineHeight();
        int asc = 22;
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y - asc, w, bh);
        u8g2_SetDrawColor(g_u8g2, 1);
        g_font.drawText(x, y, text, false);
        u8g2_SetDrawColor(g_u8g2, 0);
    } else {
        g_font.drawText(x, y, text, invert);
    }
}

void ui_draw_text_centered(int y, const char *text, bool invert, bool bold) {
    int w = g_font.textWidth(text);
    int x = (SCREEN_W - w) / 2; if (x < 0) x = 0;
    if (invert) {
        int bh = g_font.lineHeight();
        int asc = 22;
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, x, y - asc, w, bh);
        u8g2_SetDrawColor(g_u8g2, 1);
        g_font.drawText(x, y, text, false);
        u8g2_SetDrawColor(g_u8g2, 0);
    } else {
        g_font.drawText(x, y, text, invert);
    }
}

// Status bar: separator line, then white bg with black text
void ui_draw_status(const char *left, const char *right) {
    int y = STATUS_Y;
    // Separator line (black)
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
    // White background below separator
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, y + 1, SCREEN_W, FONT_H + 3);
    // Black text
    u8g2_SetDrawColor(g_u8g2, 0);
    if (left) g_font.drawText(4, y + 1 + 22, left, false);
    if (right) {
        int rw = g_font.textWidth(right);
        g_font.drawText(SCREEN_W - rw - 4, y + 1 + 22, right, false);
    }
    u8g2_SetDrawColor(g_u8g2, 1);
}

void ui_show_message_centered(const char *msg) {
    int mw = g_font.textWidth(msg);
    int mx = (SCREEN_W - mw) / 2 - 8; if (mx < 0) mx = 0;
    int my = (SCREEN_H - FONT_H - 28) / 2 + 28;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, mx, my, mw + 16, FONT_H + 8);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText(mx + 8, my + 4, msg, false);
    ui_commit();
}

// ── WiFi helper functions ─────────────────────────────────────────────
static bool ensure_wifi_connected() {
    if (g_wifi.isConnected()) return true;

    std::string ssid = g_settings.wifiSsid();
    std::string pass = g_settings.wifiPassword();
    if (ssid.empty()) return false;

    g_wifi.begin();
    if (!g_wifi.connect(ssid.c_str(), pass.c_str())) {
        return false;
    }

    // Wait up to 10s for connection
    for (int i = 0; i < 100; i++) {
        if (g_wifi.isConnected()) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static void restore_wifi_state(bool wasConnected) {
    if (!wasConnected) {
        g_wifi.disconnect();
    }
}

// ── Word/body helpers ──────────────────────────────────────────────────
// Count visible characters (non-whitespace). Each visible char = 1 count,
// suitable for Chinese text (字数统计).
static int countVisibleChars(const std::string &text) {
    int count = 0;
    for (size_t i = 0; i < text.length();) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            if (c > 0x20 && c < 0x7F) count++;
            i++;
        } else {
            count++;
            if ((c & 0xE0) == 0xC0) i += 2;
            else if ((c & 0xF0) == 0xE0) i += 3;
            else if ((c & 0xF8) == 0xF0) i += 4;
            else i++;
        }
    }
    return count;
}

static std::string extractBody(const std::string &content) {
    if (content.empty()) return "";
    std::string result;
    size_t pos = 0;
    bool inMeta = true;
    while (pos < content.length()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        if (inMeta) {
            std::string t = line;
            size_t f = t.find_first_not_of(" \t\r");
            if (f != std::string::npos) t = t.substr(f);
            if (t.empty() || t.find("日期:")==0 || t.find("字数:")==0 || t.find("提示词:")==0 || t=="自由写作") {}
            else { inMeta = false; if (!result.empty()) result += "\n"; result += line; }
        } else { if (!result.empty()) result += "\n"; result += line; }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return result;
}

// ── App API for Ctrl+Space global toggle ──────────────────────────────
bool app_ime_active() {
    return g_editor.imeActive;
}

void app_toggle_ime() {
    g_editor.imeActive = !g_editor.imeActive;
    g_ime.setActive(g_editor.imeActive);
}

// ── Editor drawing / finish ────────────────────────────────────────────
static void drawEditor() {
    int y = 28;

    // Prompt header (word-wrapped)
    if (g_editor.promptMode && !g_editor.promptText.empty()) {
        const int maxW = SCREEN_W - 8;
        const char *p = g_editor.promptText.c_str();
        while (*p) {
            const char *rowStart = p;
            int rowW = 0;
            while (*p) {
                const char *next = p;
                uint32_t cp = FontRenderer::utf8Decode(next);
                if (cp == 0) { p = next; continue; }
                int cw = g_font.charWidth(cp);
                if (rowW + cw > maxW && rowW > 0) break;
                rowW += cw;
                p = next;
            }
            std::string line(rowStart, p - rowStart);
            if (!line.empty()) {
                ui_draw_text(4, y, line.c_str(), false, true);
                y += FONT_H;
            }
        }
        u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
        y += FONT_H; // full line gap below separator
    }

    // (IME status indicator removed from edit area - shown in status bar)

    // Build visual rows
    const auto& vrows = getVrows();

    // Content area: content fills above IME area and status bar
    bool composing = g_ime.composing();
    int contentEndY = composing ? IME_CODE_Y : STATUS_Y;
    int visibleVrows = (contentEndY - y + FONT_H - 1) / FONT_H;
    if (visibleVrows < 1) visibleVrows = 1;

    // Find cursor visual row for scroll
    int cursorVR = -1;
    for (int vi = 0; vi < (int)vrows.size(); vi++) {
        if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
            cursorVR = vi;
            break;
        }
    }

    // Adjust scroll to keep cursor visible
    int normalVisibleVrows = (STATUS_Y - y + FONT_H - 1) / FONT_H;
    // When IME composing, bottom 2 lines are reserved for IME area
    int effectiveVisibleVrows = composing ? (normalVisibleVrows - 2) : normalVisibleVrows;
    if (effectiveVisibleVrows < 1) effectiveVisibleVrows = 1;

    if (cursorVR < g_editor.scroll) g_editor.scroll = cursorVR;
    if (cursorVR >= g_editor.scroll + effectiveVisibleVrows)
        g_editor.scroll = cursorVR - effectiveVisibleVrows + 1;
    if (g_editor.scroll < 0) g_editor.scroll = 0;

    // Draw visible visual rows
    for (int i = 0; i < visibleVrows && (g_editor.scroll + i) < (int)vrows.size(); i++) {
        auto &vr = vrows[g_editor.scroll + i];
        std::string text = g_editor.lines[vr.lineIdx].substr(vr.start, vr.end - vr.start);
        ui_draw_text(4, y + i * FONT_H, text.c_str());
    }

    // Draw cursor (underline at current position)
    if (cursorVR >= 0 && cursorVR >= g_editor.scroll && cursorVR < g_editor.scroll + visibleVrows) {
        auto &vr = vrows[cursorVR];
        const std::string &line = g_editor.lines[vr.lineIdx];
        std::string prefix = line.substr(vr.start, g_editor.cx - vr.start);
        int cx = 4 + g_font.textWidth(prefix.c_str());
        int cy_draw = y + (cursorVR - g_editor.scroll) * FONT_H;
        int cw = 14;
        if (g_editor.cx < (int)line.length()) {
            const char *cp = line.c_str() + g_editor.cx;
            unsigned char b = (unsigned char)*cp;
            std::string oneChar;
            if (b < 0x80) oneChar = line.substr(g_editor.cx, 1);
            else if ((b & 0xE0) == 0xC0) oneChar = line.substr(g_editor.cx, 2);
            else if ((b & 0xF0) == 0xE0) oneChar = line.substr(g_editor.cx, 3);
            else if ((b & 0xF8) == 0xF0) oneChar = line.substr(g_editor.cx, 4);
            if (!oneChar.empty()) cw = g_font.textWidth(oneChar.c_str());
        }
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, cx, cy_draw + 4, cw, 3);
        u8g2_SetDrawColor(g_u8g2, 1);
    }

    // IME area: code line + candidate line (compact)
    if (composing) {
        // Line 1: pinyin code + page indicator (white bg, black text)
        std::string code = g_ime.displayCode();
        int total = g_ime.totalCandidates();
        int pageSize = g_ime.pageSize();
        int curPage = g_ime.currentPage();
        int totalPages = (total + pageSize - 1) / pageSize;
        if (totalPages < 1) totalPages = 1;
        char pageInfo[32];
        snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
        int sepY = IME_CODE_Y - 4;
        int codeBaseline = sepY - 7;
        {
            int cw = g_font.textWidth(code.c_str()) + 8;
            u8g2_SetDrawColor(g_u8g2, 1);
            u8g2_DrawBox(g_u8g2, 4, codeBaseline - 22, cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, codeBaseline, code.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
        {
            int tw = g_font.textWidth(pageInfo);
            int pw = tw + 8;
            int px = SCREEN_W - pw - 4;
            u8g2_SetDrawColor(g_u8g2, 1);
            u8g2_DrawBox(g_u8g2, px, codeBaseline - 22, pw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(px + 4, codeBaseline, pageInfo, false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
        // Separator line drawn LAST so it's always visible on top of white boxes
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);
        u8g2_SetDrawColor(g_u8g2, 1);

        // Line 2: inline candidates (compact single row, white bg, black text)
        auto &cands = g_ime.candidates();
        std::string candLine;
        for (int i = 0; i < (int)cands.size(); i++) {
            char idx[16];
            snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);
            std::string part = std::string(" ") + idx + cands[i];
            int curW = g_font.textWidth(candLine.c_str());
            int partW = g_font.textWidth(part.c_str());
            if (curW + partW + 8 > SCREEN_W) break;
            candLine += part;
        }
        {
            int cw = g_font.textWidth(candLine.c_str()) + 8;
            u8g2_SetDrawColor(g_u8g2, 1);
            u8g2_DrawBox(g_u8g2, 4, IME_CAND_Y - 22, cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, IME_CAND_Y, candLine.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
    }

    // ── Status bar ──────────────────────────────────────────────────
    const char *mode = g_editor.promptMode ? "提示写作" : "自由写作";
    int wc = getWordCount();
    char left[48];
    snprintf(left, sizeof(left), "%s", mode);
    int bpct = battery_pct();
    const char *imeLabel = g_editor.imeActive ? "[中]" : "EN";
    char right[64];
    if (bpct >= 0)
        snprintf(right, sizeof(right), "%d字 %d%% %s", wc, bpct, imeLabel);
    else
        snprintf(right, sizeof(right), "%d字 -- %s", wc, imeLabel);

    ui_draw_status(left, right);
}

static void drawConfirmDialog() {
    int bw = 280, bh = 120;
    int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2 - 20;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);
    g_font.drawText(bx + 20, by + 28, "是否保存当前内容？");
    g_font.drawText(bx + 20, by + 58, "Enter=保存");
    g_font.drawText(bx + 20, by + 88, "ESC=放弃");
    u8g2_SetDrawColor(g_u8g2, 1);
}

static AppState finishEditor(ScreenContext &ctx) {
    std::string text;
    for (auto &l : g_editor.lines) { text += l; text += '\n'; }
    while (!text.empty() && text.back() == '\n') text.pop_back();
    if (text.empty()) return APP_MAIN;

    time_t now; time(&now); struct tm *tm = localtime(&now);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    int wc = getWordCount();  // Use cached word count
    std::string headerStr; headerStr.resize(128);
    int hlen = snprintf(&headerStr[0], 128, "日期: %s\n字数: %d\n\n", ts, wc);
    headerStr.resize(hlen);
    std::string fullText;
    if (g_editor.promptMode)
        fullText = headerStr + "提示词: " + g_editor.promptText + "\n\n" + text;
    else
        fullText = headerStr + "自由写作\n\n" + text;

    if (!g_journal.saveEntry(fullText)) {
        ctx.statusMessage = "保存失败，请检查SD卡";
    }
    ctx.nextState = APP_MAIN;
    return APP_MAIN;
}

// ── Main Screen ────────────────────────────────────────────────────────
void screen_main_init() {
    static bool seeded = false;
    if (!seeded) {
        srand(esp_random());
        seeded = true;
    }
}

AppState screen_main_handle(int key, ScreenContext &ctx) {
    ui_clear(); int y = 28;

    // Current date for title and week tracker
    time_t now_t; time(&now_t); struct tm *tm = localtime(&now_t);
    char dateStr[32]; strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", tm);
    char title[64]; snprintf(title, sizeof(title), "个人日记 %s", dateStr);
    ui_draw_text_centered(y, title, false, true); y += FONT_H;
    int daysSinceMon = (tm->tm_wday == 0) ? 6 : tm->tm_wday - 1;
    time_t monday = now_t - daysSinceMon * 86400;
    const char *dnames[7] = {"一","二","三","四","五","六","日"};
    const int colWidth = SCREEN_W / 7;
    const int colStartX = (SCREEN_W - colWidth * 7) / 2;
    for (int i = 0; i < 7; i++) {
        time_t d = monday + i * 86400; struct tm *dtm = localtime(&d);
        char ds[16]; strftime(ds, sizeof(ds), "%Y-%m-%d", dtm);
        bool isToday = (i == daysSinceMon);
        bool has = g_journal.hasEntry(ds);
        char dayStr[8];
        if (isToday) snprintf(dayStr, sizeof(dayStr), "[%s]", dnames[i]);
        else snprintf(dayStr, sizeof(dayStr), " %s ", dnames[i]);
        int cx = colStartX + i * colWidth + (colWidth - g_font.textWidth(dayStr)) / 2;
        g_font.drawText(cx, y, dayStr, false);
        const char *mark = has ? "✓" : (d <= now_t ? "·" : " ");
        int mx = colStartX + i * colWidth + (colWidth - g_font.textWidth(mark)) / 2;
        g_font.drawText(mx, y + FONT_H, mark, false);
    }
    y += FONT_H * 2;

    char buf[48]; snprintf(buf, sizeof(buf), "连续:%d天 总计:%d篇", g_journal.getStreak(), g_journal.totalEntries());
    ui_draw_text_centered(y, buf); y += FONT_H;
    int tc = g_journal.countToday();
    if (tc > 0) { snprintf(buf, sizeof(buf), "✓ 今日已写%d篇", tc); ui_draw_text_centered(y, buf, false, true); }
    else ui_draw_text_centered(y, "今日尚未写日记");
    y += FONT_H;
    ui_draw_text_centered(y, "[p] 提示写作"); y += FONT_H;
    ui_draw_text_centered(y, "[f] 自由写作"); y += FONT_H;
    if (g_journal.totalEntries() > 0) { ui_draw_text_centered(y, "[v] 查看过往日记"); y += FONT_H; }
    ui_draw_text_centered(y, "[w] 同步WebDAV"); y += FONT_H;
    ui_draw_text_centered(y, "[s] 设置"); y += FONT_H;

    // Battery at bottom right
    int bpct = battery_pct();
    if (bpct >= 0) {
        char pctStr[16]; snprintf(pctStr, sizeof(pctStr), "%d%%", bpct);
        int pw = g_font.textWidth(pctStr);
        g_font.drawText(SCREEN_W - pw - 4, STATUS_Y + 22, pctStr, false);
    }
    ui_commit();

    if (key == 'p'||key=='P') { ctx.promptMode=true; ctx.promptText=BUILTIN_PROMPTS[rand()%BUILTIN_PROMPT_COUNT]; ctx.nextState=APP_EDITOR; }
    else if (key == 'f'||key=='F') { ctx.promptMode=false; ctx.promptText=""; ctx.nextState=APP_EDITOR; }
    else if ((key=='v'||key=='V') && g_journal.totalEntries()>0) ctx.nextState=APP_BROWSER;
    else if (key=='w'||key=='W') ctx.nextState=APP_SYNC_WEBDAV;
    else if (key=='s'||key=='S') ctx.nextState=APP_SETTINGS;
    else if (key=='q'||key=='Q') ctx.nextState=APP_QUIT;
    return ctx.nextState;
}

// ── Editor Screen ──────────────────────────────────────────────────────
void screen_editor_init(const ScreenContext &ctx) {
    g_editor.lines.clear(); g_editor.lines.push_back("");
    g_editor.cx = g_editor.cy = g_editor.scroll = 0;
    g_editor.targetCx = -1;
    g_editor.confirmSave = false;
    g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    g_editor.promptText = ctx.promptText;
    g_editor.promptMode = ctx.promptMode;
}

AppState screen_editor_handle(int key, ScreenContext &ctx) {
    const auto& vrows = getVrows();

    // Confirm save dialog mode: only Enter=save, ESC=discard
    if (g_editor.confirmSave) {
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            g_editor.confirmSave = false;
            return finishEditor(ctx);
        }
        if (key == 0x1B || key == 'n' || key == 'N') {
            g_editor.confirmSave = false;
            ctx.nextState = APP_MAIN;
            return APP_MAIN;
        }
        ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
        return APP_EDITOR;
    }

    if (g_editor.imeActive && key != 0) {
        std::string imeOut;
        if (g_ime.handleKey(key, imeOut)) {
            if (!imeOut.empty()) {
                g_editor.lines[g_editor.cy].insert(g_editor.cx, imeOut);
                g_editor.cx += (int)imeOut.length();
                g_editor.targetCx = -1;
                g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
            }
            ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
        }
    }

    // Ctrl+Space / KEY_IME_TOGGLE is handled globally in main loop

    // Tab: insert 4 spaces
    if (key == 9) {
        g_editor.lines[g_editor.cy].insert(g_editor.cx, 4, ' ');
        g_editor.cx += 4;
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x10) { // Ctrl+P - AI generate prompt via Deepseek
        bool wifiWasConnected = g_wifi.isConnected();
        if (ensure_wifi_connected()) {
            g_editor.promptMode = true;
            ui_clear();
            ui_show_message_centered("AI生成提示中...");
            std::string context;
            std::string exp = g_settings.personalExperience();
            std::string hob = g_settings.personalHobbies();
            if (!exp.empty()) context += "我的经历:" + exp + ";";
            if (!hob.empty()) context += "我的爱好:" + hob + ";";
            if (context.empty()) context = "一个普通用户";
            auto result = g_deepseek.generatePrompt(context);
            if (result.success && !result.content.empty()) {
                g_editor.promptText = result.content;
            } else if (g_editor.promptText.empty()) {
                g_editor.promptText = "今天发生了什么？";
            }
        } else {
            ui_clear();
            ui_show_message_centered("WiFi连接失败");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        restore_wifi_state(wifiWasConnected);
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == 0x1B) {
        // Check if editor has any text content
        bool hasContent = g_editor.lines.size() > 1 ||
            (g_editor.lines.size() == 1 && !g_editor.lines[0].empty());
        if (hasContent) {
            g_editor.confirmSave = true;
            ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
            return APP_EDITOR;
        }
        ctx.nextState = APP_MAIN; return APP_MAIN;
    }
    if (key == 0x13) return finishEditor(ctx);  // Ctrl+S → save & exit
    if (key == 0x11) { ctx.nextState = APP_MAIN; return APP_MAIN; }  // Ctrl+Q → quit
    if (key == 0x06) {  // Ctrl+F → send to Flomo
        ctx.nextState = APP_SYNC_SEND_FLOMO;
        return APP_SYNC_SEND_FLOMO;
    }

    // Navigation & editing
    if (key == 0x0A || key == 0x0D) { // Enter
        std::string rest = g_editor.lines[g_editor.cy].substr(g_editor.cx);
        g_editor.lines[g_editor.cy] = g_editor.lines[g_editor.cy].substr(0, g_editor.cx);
        g_editor.cx = 0; g_editor.cy++;
        g_editor.lines.insert(g_editor.lines.begin() + g_editor.cy, rest);
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    } else if (key == 0x7F || key == 0x08) { // Backspace (UTF-8 aware)
        if (g_editor.cx > 0) {
            int prev = g_editor.cx - 1;
            while (prev > 0 && ((unsigned char)g_editor.lines[g_editor.cy][prev] & 0xC0) == 0x80) prev--;
            g_editor.lines[g_editor.cy].erase(prev, g_editor.cx - prev);
            g_editor.cx = prev;
        }
        else if (g_editor.cy > 0) {
            g_editor.cx = (int)g_editor.lines[g_editor.cy-1].length();
            g_editor.lines[g_editor.cy-1] += g_editor.lines[g_editor.cy];
            g_editor.lines.erase(g_editor.lines.begin() + g_editor.cy);
            g_editor.cy--;
        }
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    } else if (key >= 0x20 && key <= 0x7E) { // ASCII printable
        g_editor.lines[g_editor.cy].insert(g_editor.cx, 1, (char)key);
        g_editor.cx++;
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    } else if (key == KEY_LEFT) {
        if (g_editor.cx > 0) {
            g_editor.cx--;
            while (g_editor.cx > 0 && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx--;
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_RIGHT) {
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            g_editor.cx++;
            while (g_editor.cx < (int)g_editor.lines[g_editor.cy].length() && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx++;
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_UP) {
        // Find current visual row
        // vrows from outer scope is still valid (no content change)
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR > 0) {
            auto &prev = vrows[curVR - 1];
            // Save target column (in cell units) before changing cy
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            g_editor.cy = prev.lineIdx;
            g_editor.cx = cellsToByte(g_editor.lines[prev.lineIdx], prev.start, prev.end, g_editor.targetCx);
        }
    } else if (key == KEY_DOWN) {
        // vrows from outer scope is still valid (no content change)
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR >= 0 && curVR < (int)vrows.size() - 1) {
            auto &next = vrows[curVR + 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            g_editor.cy = next.lineIdx;
            g_editor.cx = std::min(cellsToByte(g_editor.lines[next.lineIdx], next.start, next.end, g_editor.targetCx),
                                   (int)g_editor.lines[g_editor.cy].length());
        }
    }

    ui_clear(); drawEditor(); ui_commit();
    return APP_EDITOR;
}

// ── Browser Screen ─────────────────────────────────────────────────────
void screen_browser_init() { g_browser.selection = g_browser.scroll = 0; }

AppState screen_browser_handle(int key, ScreenContext &ctx) {
    auto entries = g_journal.listEntries();
    if (entries.empty()) { ctx.nextState = APP_MAIN; return APP_MAIN; }
    if (g_browser.selection >= (int)entries.size()) g_browser.selection = (int)entries.size() - 1;

    if (key == 'q' || key == 'Q' || key == 0x1B) { ctx.nextState = APP_MAIN; return APP_MAIN; }
    if (key == 'j' || key == KEY_DOWN) { g_browser.selection++; if (g_browser.selection>=(int)entries.size()) g_browser.selection=(int)entries.size()-1; }
    if (key == 'k' || key == KEY_UP) { g_browser.selection--; if (g_browser.selection<0) g_browser.selection=0; }
    if (key == 0x0A || key == 0x0D) { ctx.selectedEntry = entries[g_browser.selection].filename; ctx.nextState = APP_VIEWER; return APP_VIEWER; }
    if (key == 'd' || key == 'D') {
        g_journal.deleteEntry(entries[g_browser.selection].filename);
        entries = g_journal.listEntries();
        if (entries.empty()) { ctx.nextState=APP_MAIN; return APP_MAIN; }
        if (g_browser.selection>=(int)entries.size()) g_browser.selection=(int)entries.size()-1;
    }
    if (key == 0x13) {
        auto content = g_journal.readEntry(entries[g_browser.selection].filename);
        if (!content.empty()) { auto body = extractBody(content);
            if (!body.empty()) ctx.statusMessage = g_flomo.send(body).message; }
    }

    ui_clear(); int y = 28;
    ui_draw_text(4, y, "过往日记", false, true);
    // Separator directly below the text (at descender bottom)
    u8g2_DrawHLine(g_u8g2, 0, y + 7, SCREEN_W);
    y = y + 7 + 24; // 24px gap, list clearly below separator
    int visible = (SCREEN_H - y + FONT_H - 1) / FONT_H;
    if (g_browser.selection < g_browser.scroll) g_browser.scroll = g_browser.selection;
    if (g_browser.selection >= g_browser.scroll + visible)
        g_browser.scroll = g_browser.selection - visible + 1;

    for (int i = 0; i < visible && (g_browser.scroll + i) < (int)entries.size(); i++) {
        auto &e = entries[g_browser.scroll + i]; bool sel = (g_browser.scroll + i == g_browser.selection);
        std::string dateDisplay;
        if (e.filename.length() >= 10) dateDisplay = e.filename.substr(0, 10);
        else dateDisplay = e.date;
        std::string preview = e.preview.empty() ? e.title : e.preview;
        char buf[80];
        snprintf(buf, sizeof(buf), "%s %s", dateDisplay.c_str(), preview.c_str());
        ui_draw_text(8, y + i * FONT_H, buf, sel);
    }
    ui_commit();
    return APP_BROWSER;
}

// ── Viewer Screen ──────────────────────────────────────────────────────
void screen_viewer_init(const std::string &filename) {
    g_viewer.filename = filename; g_viewer.scroll = 0; g_viewer.lines.clear();
    g_viewer.vrowsDirty = true;  // Mark vrows as dirty
    if (filename.length() >= 15)
        g_viewer.dateStr = filename.substr(0,10) + " " + filename.substr(11,2) + ":" + filename.substr(13,2);
    else g_viewer.dateStr = filename;
    std::string content = g_journal.readEntry(filename);
    if (content.empty()) return;
    // Split content into lines (for word-wrap builder)
    size_t pos = 0;
    while (pos < content.length()) {
        size_t nl = content.find('\n', pos);
        g_viewer.lines.push_back((nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

AppState screen_viewer_handle(int key, ScreenContext &ctx) {
    if (key == 'q' || key == 'Q' || key == 0x1B) { ctx.nextState = APP_BROWSER; return APP_BROWSER; }
    if (key == 'j' || key == KEY_DOWN) g_viewer.scroll++;
    if (key == 'k' || key == KEY_UP) { if (g_viewer.scroll > 0) g_viewer.scroll--; }

    // Build word-wrapped visual rows from content lines (cached)
    const auto& vrows = getViewerVrows();

    // Layout constants
    const int headerY = 28;
    const int sepY = headerY + FONT_H - 22;
    const int contentY = sepY + 26;
    const int contentMaxY = STATUS_Y;
    int visible = (contentMaxY - contentY + FONT_H - 1) / FONT_H;
    if (visible < 1) visible = 1;
    int maxScroll = (int)vrows.size() - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (g_viewer.scroll > maxScroll) g_viewer.scroll = maxScroll;

    ui_clear();

    // Header: filename/date (inverted = black bg, white text)
    std::string header = g_viewer.dateStr;
    if (header.empty()) header = g_viewer.filename;
    ui_draw_text(4, headerY, header.c_str(), true);

    // Separator line below header
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 4, sepY, SCREEN_W - 8);

    // Word-wrapped content (black text on white bg)
    for (int i = 0; i < visible && (g_viewer.scroll + i) < (int)vrows.size(); i++) {
        auto &vr = vrows[g_viewer.scroll + i];
        std::string text = g_viewer.lines[vr.lineIdx].substr(vr.start, vr.end - vr.start);
        ui_draw_text(4, contentY + i * FONT_H, text.c_str(), false);
    }

    // Scroll indicator at top-right (if enough room)
    if (g_viewer.scroll > 0 && maxScroll > 0) {
        int pct = (g_viewer.scroll * 100) / maxScroll;
        char pctStr[16];
        snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        int headerW = g_font.textWidth(header.c_str());
        int pctW = g_font.textWidth(pctStr);
        if (headerW + pctW + 12 < SCREEN_W) {
            ui_draw_text(SCREEN_W - pctW - 4, headerY, pctStr);
        }
    }

    ui_commit();
    return APP_VIEWER;
}

// ── BT Manage Screen ─────────────────────────────────────────────────
void screen_bt_manage_init() {
    g_btState.selection = 0;
    g_btState.scroll = 0;
    g_btState.scanning = true;
    g_btState.connecting = false;
    g_btState.conn_start_ms = 0;
    g_btState.statusMsg[0] = '\0';
    g_bt.scanDevices();
}

AppState screen_bt_manage_handle(int key, ScreenContext &ctx) {
    if (g_btState.scanning && !g_bt.isScanning()) {
        g_btState.scanning = false;
    }

    if (g_bt.isConnected()) {
        ctx.nextState = APP_MAIN;
        return APP_MAIN;
    }

    const int64_t CONN_TIMEOUT_MS = 5000;
    if (g_btState.connecting && g_btState.conn_start_ms > 0) {
        int64_t elapsed = (esp_timer_get_time() / 1000) - g_btState.conn_start_ms;
        if (elapsed > CONN_TIMEOUT_MS) {
            g_btState.connecting = false;
            g_btState.statusMsg[0] = '\0';  // clear stale "正在连接..." message
        }
    }

    if (key == 'q' || key == 'Q' || key == 0x1B) {
        if (g_bt.isScanning()) return APP_BT_MANAGE;
        if (g_btState.connecting) {
            g_btState.connecting = false;
            g_bt.disconnect();
        }
        ctx.nextState = APP_MAIN; return APP_MAIN;
    }
    if (g_btState.scanning || g_btState.connecting) {
        // Don't navigate while scanning/connecting
    } else if (key == KEY_UP) {
        if (g_btState.selection > 0) g_btState.selection--;
    } else if (key == KEY_DOWN) {
        int n = g_bt.deviceCount();
        if (g_btState.selection < n - 1) g_btState.selection++;
    } else if (key == 0x0A || key == 0x0D) {
        int n = g_bt.deviceCount();
        if (n > 0 && g_btState.selection < n) {
            g_btState.connecting = true;
            g_btState.conn_start_ms = esp_timer_get_time() / 1000;
            snprintf(g_btState.statusMsg, sizeof(g_btState.statusMsg),
                     "正在连接 %s...", g_bt.getDevice(g_btState.selection)->name);
            g_bt.connectDevice(g_btState.selection);
        }
    }

    ui_clear(); int y = 28;
    ui_draw_text_centered(y, "蓝牙键盘管理", false, true); y += FONT_H;

    if (g_btState.scanning) {
        ui_draw_text_centered(y, "正在扫描蓝牙键盘..."); y += FONT_H;
        if (y + FONT_H <= SCREEN_H)
            ui_draw_text_centered(y, "请确保键盘处于配对模式");
    } else {
        if (g_btState.connecting) {
            snprintf(g_btState.statusMsg, sizeof(g_btState.statusMsg),
                     "正在连接 %s...", g_bt.getDevice(g_btState.selection)->name);
        }
        if (g_btState.statusMsg[0]) {
            ui_draw_text_centered(y, g_btState.statusMsg, true); y += FONT_H;
        }

        int n = g_bt.deviceCount();
        if (n == 0) {
            ui_draw_text_centered(y, "未找到蓝牙键盘"); y += FONT_H;
            ui_draw_text_centered(y, "双击 USER 按钮返回后重试");
        } else {
            int visible = (SCREEN_H - y + FONT_H - 1) / FONT_H;
            if (g_btState.selection < g_btState.scroll) g_btState.scroll = g_btState.selection;
            if (g_btState.selection >= g_btState.scroll + visible)
                g_btState.scroll = g_btState.selection - visible + 1;

            for (int i = 0; i < visible && (g_btState.scroll + i) < n; i++) {
                int idx = g_btState.scroll + i;
                auto *dev = g_bt.getDevice(idx);
                bool sel = (idx == g_btState.selection);
                char buf[48];
                snprintf(buf, sizeof(buf), "  %s", dev->name);
                ui_draw_text(8, y + i * FONT_H, buf, sel);
            }
        }
    }

    ui_commit();
    return APP_BT_MANAGE;
}

// ── WiFi helper ─────────────────────────────────────────────────────────
static bool connect_wifi_from_settings() {
    std::string ssid = g_settings.wifiSsid();
    if (ssid.empty()) return false;
    std::string pass = g_settings.wifiPassword();
    g_wifi.begin();
    g_wifi.connect(ssid.c_str(), pass.c_str());
    for (int i = 0; i < 100; i++) {
        if (g_wifi.isConnected()) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return g_wifi.isConnected();
}

// ── Settings Screen ────────────────────────────────────────────────────
struct SettingField { const char *key; const char *label; bool masked; bool action; };
static const SettingField SETTINGS_FIELDS[] = {
    {"deepseek_key", "Deepseek Key", false, false},
    {"flomo_email", "Flomo 邮箱", false, false},
    {"flomo_pass", "Flomo 密码", false, false},
    {"_flomo_token", "生成Flomo Token", false, true},
    {"webdav_url", "WebDAV URL", false, false},
    {"webdav_user", "WebDAV 用户", false, false},
    {"webdav_pass", "WebDAV 密码", false, false},
    {"personal_exp", "个人经历", false, false},
    {"personal_hob", "个人爱好", false, false},
    {"wifi_ssid", "WiFi SSID", false, false},
    {"wifi_pass", "WiFi 密码", false, false},
    {"timezone", "时区(如CST-8)", false, false},
    {"ntp_server", "NTP服务器", false, false},
    {"_sync_time", "网络同步时间", false, true},
};
static const int NUM_SETTINGS = sizeof(SETTINGS_FIELDS) / sizeof(SETTINGS_FIELDS[0]);

void screen_settings_init() {
    g_settingsState.selection = g_settingsState.scroll = 0;
    g_settingsState.editing = false;
    g_settingsState.editBuffer.clear();
    g_settingsState.editCursor = 0;
    g_settingsState.imeActive = false;
}

AppState screen_settings_handle(int key, ScreenContext &ctx) {
    // ── Edit mode ──────────────────────────────────────────────────────
    if (g_settingsState.editing) {
        // Handle IME input
        if (g_settingsState.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g_settingsState.editBuffer.insert(g_settingsState.editCursor, imeOut);
                    g_settingsState.editCursor += (int)imeOut.length();
                }
                // Redraw with IME status
                ui_clear();
                auto &f = SETTINGS_FIELDS[g_settingsState.selection];
                ui_draw_text_centered(28, f.label, false, true);
                u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

                // Draw edit buffer
                std::string display = g_settingsState.editBuffer;
                if (f.masked) display = std::string(display.length(), '*');
                if (display.empty()) display = " ";
                int textY = FONT_H + 32;
                int cx = g_font.textWidth(display.substr(0, g_settingsState.editCursor).c_str());
                ui_draw_text(4, textY, display.c_str());

                // Draw cursor
                std::string cursorChar = display.substr(g_settingsState.editCursor, 1);
                int cw = cursorChar.empty() || cursorChar == " " ? 8 : g_font.textWidth(cursorChar.c_str());
                u8g2_SetDrawColor(g_u8g2, 0);
                u8g2_DrawBox(g_u8g2, 4 + cx, textY + 4, cw, 3);
                u8g2_SetDrawColor(g_u8g2, 1);

                // Draw IME status if active (horizontal layout like editor)
                if (g_ime.composing()) {
                    std::string code = g_ime.displayCode();
                    auto &cands = g_ime.candidates();
                    int pageSize = g_ime.pageSize();
                    int curPage = g_ime.currentPage();
                    int total = g_ime.totalCandidates();
                    int totalPages = (total + pageSize - 1) / pageSize;
                    if (totalPages < 1) totalPages = 1;

                    // Code line with page indicator
                    char pageInfo[32];
                    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
                    int imeY = SCREEN_H - 67;  // Moved up 7px
                    u8g2_DrawBox(g_u8g2, 0, imeY, SCREEN_W, 67);
                    u8g2_SetDrawColor(g_u8g2, 1);

                    // Draw code (white box, black text)
                    int cw = g_font.textWidth(code.c_str()) + 8;
                    u8g2_DrawBox(g_u8g2, 4, imeY + 4, cw, FONT_H);
                    u8g2_SetDrawColor(g_u8g2, 0);
                    g_font.drawText(4, imeY + 4 + FONT_H - 6, code.c_str(), false);
                    u8g2_SetDrawColor(g_u8g2, 1);

                    // Draw page indicator
                    int tw = g_font.textWidth(pageInfo);
                    int pw = tw + 8;
                    int px = SCREEN_W - pw - 4;
                    u8g2_DrawBox(g_u8g2, px, imeY + 4, pw, FONT_H);
                    u8g2_SetDrawColor(g_u8g2, 0);
                    g_font.drawText(px + 4, imeY + 4 + FONT_H - 6, pageInfo, false);
                    u8g2_SetDrawColor(g_u8g2, 1);

                    // Separator line
                    u8g2_SetDrawColor(g_u8g2, 0);
                    u8g2_DrawHLine(g_u8g2, 0, imeY + FONT_H + 4, SCREEN_W);
                    u8g2_SetDrawColor(g_u8g2, 1);

                    // Candidates inline (horizontal)
                    std::string candLine;
                    for (int i = 0; i < (int)cands.size(); i++) {
                        char idx[16];
                        snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);
                        std::string part = std::string(" ") + idx + cands[i];
                        int curW = g_font.textWidth(candLine.c_str());
                        int partW = g_font.textWidth(part.c_str());
                        if (curW + partW + 8 > SCREEN_W) break;
                        candLine += part;
                    }
                    if (!candLine.empty()) {
                        int candW = g_font.textWidth(candLine.c_str()) + 8;
                        u8g2_DrawBox(g_u8g2, 4, imeY + FONT_H + 8, candW, FONT_H);
                        u8g2_SetDrawColor(g_u8g2, 0);
                        g_font.drawText(4, imeY + FONT_H + 8 + FONT_H - 6, candLine.c_str(), false);
                        u8g2_SetDrawColor(g_u8g2, 0);
                    }
                }

                ui_commit();
                return APP_SETTINGS;
            }
        }

        if (key == KEY_IME_TOGGLE) {
            g_settingsState.imeActive = !g_settingsState.imeActive;
            g_ime.setActive(g_settingsState.imeActive);
            key = 0;
        }

        if (key == 0x1B) {
            g_settingsState.editing = false;
            g_settingsState.editBuffer.clear();
            g_settingsState.editCursor = 0;
            g_settingsState.imeActive = false;
            g_ime.setActive(false);
        } else if (key == 0x0A || key == 0x0D) {
            auto &f = SETTINGS_FIELDS[g_settingsState.selection];
            g_settings.setString(f.key, g_settingsState.editBuffer);
            g_settingsState.editing = false;
            g_settingsState.editBuffer.clear();
            g_settingsState.editCursor = 0;
            g_settingsState.imeActive = false;
            g_ime.setActive(false);
        } else if (key == 0x7F || key == 0x08) {
            // UTF-8 aware backspace
            if (g_settingsState.editCursor > 0) {
                int prev = g_settingsState.editCursor - 1;
                // Move back over continuation bytes
                while (prev > 0 && ((unsigned char)g_settingsState.editBuffer[prev] & 0xC0) == 0x80)
                    prev--;
                // Delete the whole UTF-8 character
                g_settingsState.editBuffer.erase(prev, g_settingsState.editCursor - prev);
                g_settingsState.editCursor = prev;
            }
        } else if (key == KEY_LEFT) {
            if (g_settingsState.editCursor > 0) {
                // UTF-8 aware: move back over continuation bytes
                g_settingsState.editCursor--;
                while (g_settingsState.editCursor > 0 &&
                       ((unsigned char)g_settingsState.editBuffer[g_settingsState.editCursor] & 0xC0) == 0x80)
                    g_settingsState.editCursor--;
            }
        } else if (key == KEY_RIGHT) {
            if (g_settingsState.editCursor < (int)g_settingsState.editBuffer.length()) {
                g_settingsState.editCursor++;
                while (g_settingsState.editCursor < (int)g_settingsState.editBuffer.length() &&
                       ((unsigned char)g_settingsState.editBuffer[g_settingsState.editCursor] & 0xC0) == 0x80)
                    g_settingsState.editCursor++;
            }
        } else if (key >= 0x20 && key <= 0x7E) {
            g_settingsState.editBuffer.insert(g_settingsState.editCursor, 1, (char)key);
            g_settingsState.editCursor++;
        }

        ui_clear();
        auto &f = SETTINGS_FIELDS[g_settingsState.selection];
        ui_draw_text_centered(28, f.label, false, true);
        u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

        std::string display = g_settingsState.editBuffer;
        if (f.masked) display = std::string(display.length(), '*');

        // Word-wrap display text
        int textY = FONT_H + 32;
        int maxW = SCREEN_W - 8;  // 4px margins on each side
        std::vector<std::pair<int,int>> lines;  // start, end in display string

        int lineStart = 0;
        while (lineStart < (int)display.length()) {
            int lineEnd = lineStart;
            int lastGood = lineStart;
            // Find how many chars fit
            while (lineEnd <= (int)display.length()) {
                int w = g_font.textWidth(display.substr(lineStart, lineEnd - lineStart).c_str());
                if (w > maxW) {
                    lineEnd = lastGood;
                    break;
                }
                lastGood = lineEnd;
                // UTF-8: move to next char
                lineEnd++;
                while (lineEnd < (int)display.length() &&
                       ((unsigned char)display[lineEnd] & 0xC0) == 0x80)
                    lineEnd++;
            }
            if (lineEnd <= lineStart) lineEnd = lastGood;
            if (lineEnd <= lineStart) lineEnd = display.length();
            lines.push_back({lineStart, lineEnd});
            lineStart = lineEnd;
        }

        // Draw all lines
        for (int i = 0; i < (int)lines.size(); i++) {
            std::string lineText = display.substr(lines[i].first, lines[i].second - lines[i].first);
            if (lineText.empty() && i == 0) lineText = " ";
            ui_draw_text(4, textY + i * FONT_H, lineText.c_str());
        }

        // Find cursor position (which line, which x offset)
        int cursorLine = 0;
        int cursorX = 0;
        for (int i = 0; i < (int)lines.size(); i++) {
            if (g_settingsState.editCursor >= lines[i].first && g_settingsState.editCursor <= lines[i].second) {
                cursorLine = i;
                std::string beforeCursor = display.substr(lines[i].first, g_settingsState.editCursor - lines[i].first);
                cursorX = g_font.textWidth(beforeCursor.c_str());
                break;
            }
        }

        // Draw cursor
        std::string cursorChar = display.substr(g_settingsState.editCursor, 1);
        int cw = cursorChar.empty() || cursorChar == " " ? 8 : g_font.textWidth(cursorChar.c_str());
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, 4 + cursorX, textY + cursorLine * FONT_H + 4, cw, 3);
        u8g2_SetDrawColor(g_u8g2, 1);

        // Draw IME status if active (horizontal layout like editor)
        if (g_settingsState.imeActive && g_ime.composing()) {
            std::string code = g_ime.displayCode();
            auto &cands = g_ime.candidates();
            int pageSize = g_ime.pageSize();
            int curPage = g_ime.currentPage();
            int total = g_ime.totalCandidates();
            int totalPages = (total + pageSize - 1) / pageSize;
            if (totalPages < 1) totalPages = 1;

            // Code line with page indicator
            char pageInfo[32];
            snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
            int imeY = SCREEN_H - 67;  // Moved up 7px
            u8g2_DrawBox(g_u8g2, 0, imeY, SCREEN_W, 67);
            u8g2_SetDrawColor(g_u8g2, 1);

            // Draw code (white box, black text)
            int cw = g_font.textWidth(code.c_str()) + 8;
            u8g2_DrawBox(g_u8g2, 4, imeY + 4, cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, imeY + 4 + FONT_H - 6, code.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);

            // Draw page indicator
            int tw = g_font.textWidth(pageInfo);
            int pw = tw + 8;
            int px = SCREEN_W - pw - 4;
            u8g2_DrawBox(g_u8g2, px, imeY + 4, pw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(px + 4, imeY + 4 + FONT_H - 6, pageInfo, false);
            u8g2_SetDrawColor(g_u8g2, 1);

            // Separator line
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawHLine(g_u8g2, 0, imeY + FONT_H + 4, SCREEN_W);
            u8g2_SetDrawColor(g_u8g2, 1);

            // Candidates inline (horizontal)
            std::string candLine;
            for (int i = 0; i < (int)cands.size(); i++) {
                char idx[16];
                snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);
                std::string part = std::string(" ") + idx + cands[i];
                int curW = g_font.textWidth(candLine.c_str());
                int partW = g_font.textWidth(part.c_str());
                if (curW + partW + 8 > SCREEN_W) break;
                candLine += part;
            }
            if (!candLine.empty()) {
                int candW = g_font.textWidth(candLine.c_str()) + 8;
                u8g2_DrawBox(g_u8g2, 4, imeY + FONT_H + 8, candW, FONT_H);
                u8g2_SetDrawColor(g_u8g2, 0);
                g_font.drawText(4, imeY + FONT_H + 8 + FONT_H - 6, candLine.c_str(), false);
                u8g2_SetDrawColor(g_u8g2, 0);
            }
        }

        ui_commit();
        return APP_SETTINGS;
    }

    // ── Browse mode ────────────────────────────────────────────────────
    if (key == 'q' || key == 'Q' || key == 0x1B) { ctx.nextState = APP_MAIN; return APP_MAIN; }
    if (key == 'k' || key == KEY_UP) { if (g_settingsState.selection > 0) g_settingsState.selection--; }
    if (key == 'j' || key == KEY_DOWN) { if (g_settingsState.selection < NUM_SETTINGS-1) g_settingsState.selection++; }
    if (key == 'd' || key == 'D') {
        auto &f = SETTINGS_FIELDS[g_settingsState.selection];
        if (!f.action) g_settings.erase(SETTINGS_FIELDS[g_settingsState.selection].key);
    }
    if (key == 0x0A || key == 0x0D) {
        auto &f = SETTINGS_FIELDS[g_settingsState.selection];
        if (f.action) {
            // ── Execute action ────────────────────────────────────────
            if (strcmp(f.key, "_flomo_token") == 0) {
                std::string email = g_settings.flomoEmail();
                std::string pass = g_settings.flomoPassword();
                if (email.empty() || pass.empty()) {
                    ui_clear(); ui_show_message_centered("请先设置Flomo邮箱和密码");
                    vTaskDelay(pdMS_TO_TICKS(2000)); return APP_SETTINGS;
                }
                bool wifiWas = g_wifi.isConnected();
                if (!wifiWas) {
                    ui_clear(); ui_show_message_centered("正在连接WiFi..."); ui_commit();
                    if (!connect_wifi_from_settings()) {
                        ui_clear(); ui_show_message_centered("WiFi连接失败");
                        vTaskDelay(pdMS_TO_TICKS(2000)); return APP_SETTINGS;
                    }
                }
                ui_clear(); ui_show_message_centered("正在生成Token..."); ui_commit();
                g_flomo.configure(email, pass);
                std::string token = g_flomo.login();
                if (!token.empty()) {
                    g_flomo.setCachedToken(token);
                    ui_clear(); ui_show_message_centered("Token生成成功 ✓");
                } else {
                    ui_clear(); ui_show_message_centered("Flomo登录失败");
                }
                if (!wifiWas) g_wifi.disconnect();
                vTaskDelay(pdMS_TO_TICKS(2000)); return APP_SETTINGS;
            }
            if (strcmp(f.key, "_sync_time") == 0) {
                bool wifiWas = g_wifi.isConnected();
                if (!wifiWas) {
                    ui_clear(); ui_show_message_centered("正在连接WiFi..."); ui_commit();
                    if (!connect_wifi_from_settings()) {
                        ui_clear(); ui_show_message_centered("WiFi连接失败");
                        vTaskDelay(pdMS_TO_TICKS(2000)); return APP_SETTINGS;
                    }
                    vTaskDelay(pdMS_TO_TICKS(500)); // settle for DHCP/DNS
                }
                std::string ntp = g_settings.ntpServer();
                std::string tz = g_settings.timezone();
                if (tz.empty()) tz = "CST-8";
                if (ntp.empty()) {
                    ui_clear(); ui_show_message_centered("请先设置NTP服务器");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                } else {
                    esp_sntp_stop();
                    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                    esp_sntp_setservername(0, ntp.c_str());
                    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
                    esp_sntp_init();
                    setenv("TZ", tz.c_str(), 1);
                    tzset();
                    time_t now = 0;
                    for (int i = 0; i < 100; i++) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                            time(&now);
                            break;
                        }
                    }
                    if (now > 1704067200) {
                        struct tm *tm = localtime(&now);
                        char ts[64];
                        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
                        // Sync to RTC
                        g_rtc.setTime(now);
                        // Stop SNTP polling to save power
                        esp_sntp_stop();
                        char msg[80];
                        snprintf(msg, sizeof(msg), "同步成功: %s", ts);
                        ui_clear(); ui_show_message_centered(msg);
                    } else {
                        esp_sntp_stop();
                        ui_clear(); ui_show_message_centered("时间同步失败");
                    }
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                if (!wifiWas) g_wifi.disconnect();
                return APP_SETTINGS;
            }
        } else {
            g_settingsState.editBuffer = g_settings.getString(f.key);
            g_settingsState.editCursor = (int)g_settingsState.editBuffer.length();
            g_settingsState.editing = true;
            g_settingsState.imeActive = false;  // Start with IME off, user can toggle with Ctrl+Space
        }
    }

    ui_clear(); int y = 28;
    ui_draw_text_centered(y, "设置", false, true); y += FONT_H;
    int visible = (SCREEN_H - y + FONT_H - 1) / FONT_H;
    if (g_settingsState.selection < g_settingsState.scroll) g_settingsState.scroll = g_settingsState.selection;
    if (g_settingsState.selection >= g_settingsState.scroll + visible)
        g_settingsState.scroll = g_settingsState.selection - visible + 1;

    for (int i = 0; i < visible && (g_settingsState.scroll + i) < NUM_SETTINGS; i++) {
        int idx = g_settingsState.scroll + i; auto &f = SETTINGS_FIELDS[idx];
        bool sel = (idx == g_settingsState.selection);
        char buf[80];
        if (f.action) {
            snprintf(buf, sizeof(buf), "▶ %s", f.label);
        } else {
            std::string value = g_settings.getString(f.key);
            std::string display;
            if (value.empty()) display = "(未设置)";
            else if (f.masked) display = "********";
            else display = value;
            snprintf(buf, sizeof(buf), "%s:%s", f.label, display.c_str());
        }
        ui_draw_text(8, y + i * FONT_H, buf, sel);
    }
    ui_commit();
    return APP_SETTINGS;
}

// Export editor text for Flomo sending from main loop
std::string app_get_editor_text() {
    std::string text;
    for (auto &l : g_editor.lines) { text += l; text += '\n'; }
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}
