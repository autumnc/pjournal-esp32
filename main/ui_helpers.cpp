#include "ui_helpers.h"
#include "font_renderer.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "ime/IME.h"
#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern void *g_u8g2;

extern "C" {
    extern void u8g2_ClearBuffer(void *u8g2);
    extern void u8g2_SendBuffer(void *u8g2);
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *u8g2, int x, int y, int w, int h);
}

#define STATUS_Y (SCREEN_H - FONT_H - 2)

// ── Battery ADC ──────────────────────────────────────────────────────────
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

// ── Word wrap helpers ────────────────────────────────────────────────────
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

int byteToCells(const std::string &line, int byteOffset) {
    int cells = 0;
    for (int i = 0; i < byteOffset; ) {
        cells += charCellWidth((unsigned char)line[i]);
        i += utf8CharLen((unsigned char)line[i]);
    }
    return cells;
}

int cellsToByte(const std::string &line, int start, int end, int targetCells) {
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

std::vector<VRow> buildVrows(const std::vector<std::string> &lines) {
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
                if (cells + cc > 28) break;
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

// ── IME singleton ─────────────────────────────────────────────────────────
IME &g_ime = IME::getInstance();

// ── IME drawing helper ────────────────────────────────────────────────────
void drawIMEUI(int baseY) {
    if (!g_ime.composing()) return;

    std::string code = g_ime.displayCode();
    auto &cands = g_ime.candidates();
    int pageSize = g_ime.pageSize();
    int curPage = g_ime.currentPage();
    int total = g_ime.totalCandidates();
    int totalPages = (total + pageSize - 1) / pageSize;
    if (totalPages < 1) totalPages = 1;

    char pageInfo[32];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);

    u8g2_DrawBox(g_u8g2, 0, baseY, SCREEN_W, 67);
    u8g2_SetDrawColor(g_u8g2, 1);

    int cw = g_font.textWidth(code.c_str()) + 8;
    u8g2_DrawBox(g_u8g2, 4, baseY + 4, cw, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText(4, baseY + 4 + FONT_H - 6, code.c_str(), false);
    u8g2_SetDrawColor(g_u8g2, 1);

    int tw = g_font.textWidth(pageInfo);
    int pw = tw + 8;
    int px = SCREEN_W - pw - 4;
    u8g2_DrawBox(g_u8g2, px, baseY + 4, pw, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText(px + 4, baseY + 4 + FONT_H - 6, pageInfo, false);
    u8g2_SetDrawColor(g_u8g2, 1);

    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, baseY + FONT_H + 4, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);

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

// ── UI Helpers ────────────────────────────────────────────────────────────
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

void ui_draw_status(const char *left, const char *right) {
    int y = STATUS_Y;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, y + 1, SCREEN_W, FONT_H + 3);
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

// ── WiFi helper functions ────────────────────────────────────────────────
bool ensure_wifi_connected() {
    if (g_wifi.isConnected()) return true;

    std::string ssid = g_settings.wifiSsid();
    std::string pass = g_settings.wifiPassword();
    if (ssid.empty()) return false;

    g_wifi.begin();
    if (!g_wifi.connect(ssid.c_str(), pass.c_str())) {
        return false;
    }

    for (int i = 0; i < 100; i++) {
        if (g_wifi.isConnected()) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

void restore_wifi_state(bool wasConnected) {
    if (!wasConnected) {
        g_wifi.disconnect();
    }
}

// ── Word/body helpers ────────────────────────────────────────────────────
int countVisibleChars(const std::string &text) {
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

std::string extractBody(const std::string &content) {
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
