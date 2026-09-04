#include "ui_helpers.h"
#include "font_renderer.h"
#include "markdown_render.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "ime/IME.h"
#include "bt_keyboard.h"
#include <cstdlib>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include "u8g2.h"

extern u8g2_t *g_u8g2;

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
    // 每个 5s 采样点采 8 次取均值,降低单次 ADC 抖动
    float sum = 0.0f;
    int ok = 0;
    for (int i = 0; i < 8; i++) {
        int raw;
        if (adc_oneshot_read(s_adc_handle, ADC_CHANNEL_3, &raw) != ESP_OK) continue;
        int mv;
        if (adc_cali_raw_to_voltage(s_cali_handle, raw, &mv) != ESP_OK) continue;
        sum += mv * 0.001f * 3.0f;
        ok++;
    }
    if (ok == 0) return cached;
    // 跨采样点 EMA 平滑,防止电量显示跳变(在电压域平滑,避免 LUT 折点处产生毛刺)
    static float ema_voltage = 0.0f;
    static bool ema_inited = false;
    float v = sum / ok;
    if (!ema_inited) { ema_voltage = v; ema_inited = true; }
    else ema_voltage = 0.3f * v + 0.7f * ema_voltage;
    v = ema_voltage;
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

// 设备电池文本:"[电池图标] 电量";电量未知时返回空串
// 图标为 PUA 码点,drawText 会渲染成符号
std::string battery_text() {
    int bpct = battery_pct();
    if (bpct < 0) return "";
    char buf[24];
    // U+E001 电池图标(图标后不加空格,紧贴电量)
    snprintf(buf, sizeof(buf), "\xEE\x80\x81%d%%", bpct);
    return buf;
}

// 设备电量 + 有蓝牙键盘时追加 " [蓝牙图标] 键盘电量"
std::string battery_status_text() {
    std::string s = battery_text();
    if (s.empty()) return "";
    if (g_bt.isConnected()) {
        int kb = g_bt.keyboardBatteryPct();
        // U+E002 蓝牙图标,前导空格作为与电池组的间隔
        char buf[24];
        if (kb >= 0)
            snprintf(buf, sizeof(buf), " \xEE\x80\x82%d%%", kb);
        else
            snprintf(buf, sizeof(buf), " \xEE\x80\x82--");
        s += buf;
    }
    return s;
}

// ── 纯图标电量(电平图标,无数字) ──────────────────────────────────────
// U+E018..E022 设备充电电平: 0=不足10%, 1..9=10%~90%, 10=100%
static const char *const s_devBatteryIcons[11] = {
    "\xEE\x80\x98", "\xEE\x80\x99", "\xEE\x80\x9A", "\xEE\x80\x9B",
    "\xEE\x80\x9C", "\xEE\x80\x9D", "\xEE\x80\x9E", "\xEE\x80\x9F",
    "\xEE\x80\xA0", "\xEE\x80\xA1", "\xEE\x80\xA2",
};
// U+E023..E02D 蓝牙键盘电平: 0=不足10%, 1..9=10%~90%, 10=100%
static const char *const s_btBatteryIcons[11] = {
    "\xEE\x80\xA3", "\xEE\x80\xA4", "\xEE\x80\xA5", "\xEE\x80\xA6",
    "\xEE\x80\xA7", "\xEE\x80\xA8", "\xEE\x80\xA9", "\xEE\x80\xAA",
    "\xEE\x80\xAB", "\xEE\x80\xAC", "\xEE\x80\xAD",
};

static const char *batteryLevelIcon(int pct, const char *const icons[11]) {
    if (pct < 0) return nullptr;
    int lvl = pct / 10;
    if (lvl > 10) lvl = 10;
    return icons[lvl];
}

// 设备电池电平图标;电量未知返回空串。目前恒用充电图标(无充电检测,见 memory)。
std::string battery_icon_text() {
    int bpct = battery_pct();
    const char *ic = batteryLevelIcon(bpct, s_devBatteryIcons);
    if (!ic) return "";
    return ic;
}

// 设备电平图标 + 有蓝牙键盘时追加 " [蓝牙电平图标]"
std::string battery_icon_status_text() {
    std::string s = battery_icon_text();
    if (s.empty()) return "";
    if (g_bt.isConnected()) {
        int kb = g_bt.keyboardBatteryPct();
        const char *ic = (kb >= 0) ? batteryLevelIcon(kb, s_btBatteryIcons) : s_btBatteryIcons[0];
        s += " ";
        s += ic;
    }
    return s;
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
    int cells = byteToCells(line, start);
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

// Leading markdown block marker length in bytes (0 if the line doesn't start
// with one). Keeps the marker ("# ", "> ", "- ", "- [ ] ", "1. ", ...) from
// being split onto its own vrow by the space word-break below. Nested list
// markers include their leading whitespace (mdListMarker.start).
static int mdPrefixLen(const std::string &line) {
    MdListMarker m = mdListMarker(line);
    if (m.ok) return m.start + m.len;
    int len = (int)line.size();
    if (len >= 2 && line[0] == '>' && line[1] == ' ') return 2;
    int h = 0;
    while (h < len && h < 6 && line[h] == '#') h++;
    if (h >= 1 && h < len && line[h] == ' ') return h + 1;
    return 0;
}

// Cells reserved per vrow for the RENDERED block indent so wrapped content
// doesn't overrun the screen. Must match markdown_render.cpp layout: heading at
// cell 2, list/task at marker cells (content at start+cells), quote 4 cells
// (bar at cell 4 + 2px gap). Nested markers reserve leading ws + marker cells.
static int mdIndentCells(const std::string &line) {
    MdListMarker m = mdListMarker(line);
    if (m.ok) return m.start + m.cells;
    int len = (int)line.size();
    if (len >= 2 && line[0] == '>' && line[1] == ' ') return 4;
    return mdPrefixLen(line) > 0 ? 2 : 0;  // heading
}

std::vector<VRow> buildVrows(const std::vector<std::string> &lines,
                             const std::vector<MdLineInfo> *mdInfoIn,
                             const std::set<int> *foldedHeadings) {
    std::vector<VRow> vrows;
    bool firstLineIndent = g_settings.firstLineIndent();
    std::vector<MdLineInfo> localInfo;
    const std::vector<MdLineInfo> *mdInfoPtr = mdInfoIn;
    if (!mdInfoPtr && firstLineIndent) {
        localInfo = mdClassifyLines(lines);
        mdInfoPtr = &localInfo;
    }
    bool folding = foldedHeadings && !foldedHeadings->empty();
    int hideLevel = 0;  // >0 表示当前处于某标题的折叠区内
    for (int li = 0; li < (int)lines.size(); li++) {
        if (folding) {
            int lvl = mdInfoPtr ? (*mdInfoPtr)[li].headingLevel : 0;
            bool inCode = mdInfoPtr ? (*mdInfoPtr)[li].inCodeBlock : false;
            bool isH = lvl > 0 && !inCode;
            if (hideLevel != 0 && !(isH && lvl <= hideLevel)) continue;  // 折叠区内且不是结束边界
            if (isH) hideLevel = foldedHeadings->count(li) ? lvl : 0;
        }
        const auto &line = lines[li];
        int len = (int)line.length();
        if (len == 0) {
            vrows.push_back({li, 0, 0});
            continue;
        }
        int maxc = SCREEN_W / g_font.halfAdvance();
        int indent = mdIndentCells(line);
        int prefixEnd = mdPrefixLen(line);
        int firstIndent = 0;
        if (firstLineIndent && mdInfoPtr) {
            const MdLineInfo &info = (*mdInfoPtr)[li];
            if (info.headingLevel == 0 && !info.list && !info.task &&
                !info.quote && !info.inCodeBlock && !info.hr) {
                firstIndent = 4;  // two Chinese-width characters
            }
        }
        int pos = 0;
        while (pos < len) {
            int cells = 0;
            int end = pos;
            int lastBreak = -1;
            int pe = (pos == 0) ? prefixEnd : 0;  // only the first vrow has the marker
            // cap 用标记的格数而非字节数(pe):CJK 标记(如 `一、`/`1、`)字节数大于格数,
            // 用 pe 会让首 vrow 内容多塞几格、画到屏幕右缘之外。
            int rowIndent = (pos == 0) ? firstIndent : 0;
            int cap = maxc - indent - rowIndent + ((pos == 0) ? byteToCells(line, prefixEnd) : 0);
            if (cap > maxc) cap = maxc;
            if (cap < 1) cap = 1;
            while (end < len) {
                unsigned char c = (unsigned char)line[end];
                int cc = charCellWidth(c);
                if (cells + cc > cap) break;
                cells += cc;
                int clen = utf8CharLen(c);
                if (c == ' ' && end >= pe) {
                    lastBreak = end + 1;
                } else if (c >= 0x80 && end >= pe) {
                    // CJK: 每个汉字都允许折行。否则连续无空格中文被当作一个单词,
                    // 英文+空格+汉字时会固定在空格处折行,英文行尾留大片空白。
                    lastBreak = end + clen;
                }
                end += clen;
            }
            if (end >= len) {
                vrows.push_back({li, pos, len, rowIndent});
                break;
            }
            if (lastBreak > pos) {
                vrows.push_back({li, pos, lastBreak, rowIndent});
                pos = lastBreak;
                while (pos < len && line[pos] == ' ') pos++;
            } else {
                vrows.push_back({li, pos, end, rowIndent});
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
    int totalPages = g_ime.totalPages();
    if (totalPages < 1) totalPages = 1;

    char pageInfo[32];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);

    u8g2_DrawBox(g_u8g2, 0, baseY, SCREEN_W, 67);
    u8g2_SetDrawColor(g_u8g2, 1);

    int cw = g_font.textWidth(code.c_str()) + 8;
    u8g2_DrawBox(g_u8g2, 4, baseY + 4, cw, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText(4, baseY + 4 + g_font.ascent(), code.c_str(), false);
    u8g2_SetDrawColor(g_u8g2, 1);

    int tw = g_font.textWidth(pageInfo);
    int pw = tw + 8;
    int px = SCREEN_W - pw - 4;
    u8g2_DrawBox(g_u8g2, px, baseY + 4, pw, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText(px + 4, baseY + 4 + g_font.ascent(), pageInfo, false);
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
        g_font.drawText(4, baseY + FONT_H + 8 + g_font.ascent(), candLine.c_str(), false);
        u8g2_SetDrawColor(g_u8g2, 0);
    }
}

// ── UI Helpers ────────────────────────────────────────────────────────────
void ui_clear() {
    if (g_u8g2) { u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawBox(g_u8g2, 0, 0, SCREEN_W, SCREEN_H);
        u8g2_SetDrawColor(g_u8g2, 0); }
}

// ── Idle refresh optimization ─────────────────────────────────────────────
// 空闲时屏幕内容逐字节不变(仅键盘输入/日期翻页/电量变化会改变内容),
// 通过快照对比跳过重复的 SPI 刷新。ST7305 是 GRAM 型控制器,图像可长期
// 保持,保留低频 KEEPALIVE_US 兜底以防 RLCD 残影。
static uint8_t *s_frame_snapshot = nullptr;
static int64_t s_last_send_us = 0;
static const int64_t KEEPALIVE_US = 5 * 1000000;  // 5s

void ui_commit() {
    if (!g_u8g2) return;

    uint8_t *buf = u8g2_GetBufferPtr(g_u8g2);
    size_t size = u8g2_GetBufferSize(g_u8g2);

    if (!s_frame_snapshot) {
        s_frame_snapshot = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_frame_snapshot) {
            u8g2_SendBuffer(g_u8g2);  // 无快照时退回无条件发送
            return;
        }
        memcpy(s_frame_snapshot, buf, size);
        u8g2_SendBuffer(g_u8g2);
        s_last_send_us = esp_timer_get_time();
        return;
    }

    int64_t now = esp_timer_get_time();
    if (memcmp(buf, s_frame_snapshot, size) != 0) {
        u8g2_SendBuffer(g_u8g2);
        memcpy(s_frame_snapshot, buf, size);
        s_last_send_us = now;
    } else if (now - s_last_send_us >= KEEPALIVE_US) {
        u8g2_SendBuffer(g_u8g2);
        s_last_send_us = now;
    }
}

// 丢弃快照,使下一次 ui_commit 无条件整屏发送。
// 用于 light sleep 唤醒后面板被复位、需要强制重绘的场景。
void ui_invalidate_snapshot() {
    if (s_frame_snapshot) {
        heap_caps_free(s_frame_snapshot);
        s_frame_snapshot = nullptr;
    }
    s_last_send_us = 0;
}

int ui_text_width(const char *text) { return g_font.textWidth(text); }

void ui_draw_text(int x, int y, const char *text, bool invert, bool bold) {
    if (invert) {
        int w = g_font.textWidth(text);
        int bh = g_font.lineHeight();
        int asc = g_font.ascent();
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
        int asc = g_font.ascent();
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
    // 状态栏始终用22号字体;画完恢复原字号,不影响同帧后续绘制(如编辑器保存确认框)
    int prev_size = g_font.fontSize();
    if (prev_size != STATUS_BAR_FONT_SIZE) g_font.setSize(STATUS_BAR_FONT_SIZE);
    int y = STATUS_BAR_Y;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, y + 1, SCREEN_W, FONT_H + 3);
    u8g2_SetDrawColor(g_u8g2, 0);
    if (left) g_font.drawText(4, y + 1 + g_font.ascent(), left, false);
    if (right) {
        int rw = g_font.textWidth(right);
        g_font.drawText(SCREEN_W - rw - 4, y + 1 + g_font.ascent(), right, false);
    }
    u8g2_SetDrawColor(g_u8g2, 1);
    if (g_font.fontSize() != prev_size) g_font.setSize(prev_size);
}

void ui_show_message_centered(const char *msg) {
    // 先清屏,确保消息框是不透明对话框而不是盖在旧画面上
    ui_clear();
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
