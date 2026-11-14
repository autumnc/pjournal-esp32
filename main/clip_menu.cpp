// Win+V 剪贴板历史浮动菜单。
//
// 绘制方式：打开时把当前整帧复制到 PSRAM 作为底板，之后每次重绘都先恢复
// 底板再画菜单，用 ui_send_buffer() 发送（不更新 ui_commit 的快照），这样
// 关闭/被休眠打断后都能干净地回到原界面。
//
// 内存：底板 15 KB 只在首次打开时分配一次，之后复用；历史条目本身由
// clip_history.cpp 管理。分配失败时打开失败，按键按原样交给原界面。
//
// 网络模式（w）：临时连 WiFi 并起 clip_web 的 HTTP 服务，浏览器可查看/编辑
// 条目；Esc 回列表、再 Esc/Win+V 关闭时才停服并断开（仅当是本模式连的 WiFi）。

#include "clip_menu.h"

#include "clip_history.h"
#include "clip_web.h"
#include "clipboard.h"
#include "font_renderer.h"
#include "ime/IME.h"
#include "pjournal_app.h"
#include "screen_editor.h"
#include "screen_flomo.h"
#include "screen_gtd.h"
#include "screen_outline.h"
#include "screen_polish.h"
#include "screen_polish_prompt.h"
#include "screen_settings.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "wifi_manager.h"

#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "u8g2.h"

extern u8g2_t *g_u8g2;

static const char *TAG = "ClipMenu";

static bool s_active = false;
static AppState s_state = APP_MAIN;
static int s_sel = 0;
static int s_scroll = 0;
static uint8_t *s_base = nullptr;   // PSRAM 整帧底板
static size_t s_baseSize = 0;
static int64_t s_lastDeleteUs = 0;
static bool s_webMode = false;
static bool s_wifiOwned = false;    // WiFi 是本模式连的，退出时要断开
static char s_webUrl[64] = "";
static char s_webMsg[64] = "";      // 连接中/失败的提示

static const int kMenuW = 340;
static const int kMenuRows = 6;

static size_t utf8CharLen(const std::string &s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t n = 1;
    if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    if (i + n > s.size()) n = 1;
    return n;
}

// 按像素宽度截断，末尾加省略号。前缀很短（菜单一行 ≈ 320px），
// 逐字符累加即可，不按整条 32 KB 长度做 O(n²) 量宽。
static std::string clampLine(const std::string &s, int maxW) {
    if (g_font.textWidth(s.c_str()) <= maxW) return s;
    const int ew = g_font.textWidth("…");
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t n = utf8CharLen(s, i);
        std::string next = out + s.substr(i, n);
        if (g_font.textWidth(next.c_str()) + ew > maxW) break;
        out.swap(next);
        i += n;
    }
    return out + "…";
}

static std::string entryPreview(int idx, int maxW) {
    const char *data = cliphist_text(idx);
    if (!data) return "（空）";
    const uint32_t len = cliphist_len(idx);

    // 只取第一行开头的一小段，避免每帧把 32 KB 条目整段拷进内部 RAM。
    const char *nl = (const char *)memchr(data, '\n', len);
    size_t firstLen = nl ? (size_t)(nl - data) : len;
    size_t take = 0;
    while (take < firstLen && take < 512) {
        unsigned char c = (unsigned char)data[take];
        size_t step = 1;
        if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (take + step > firstLen) break;
        take += step;
    }

    std::string s(data, take);
    if (nl) s += " …";
    if (s.empty()) s = "（空）";
    return clampLine(s, maxW);
}

static void drawMenu() {
    if (!g_u8g2 || !s_base) return;

    const int prevSize = g_font.fontSize();
    g_font.setSize(22);
    const int asc = g_font.ascent();
    const int rowH = g_font.lineHeight() + 4;
    const int pad = 6;
    const int count = cliphist_count();

    const int boxW = kMenuW;
    const int boxH = pad + rowH + kMenuRows * rowH + rowH + pad;
    const int boxX = (SCREEN_W - boxW) / 2;
    const int boxY = (SCREEN_H - boxH) / 2;
    const int innerX = boxX + pad;
    const int innerW = boxW - 2 * pad;

    // 恢复底板，再在它上面画不透明面板。
    memcpy(u8g2_GetBufferPtr(g_u8g2), s_base, s_baseSize);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);

    char title[64];
    snprintf(title, sizeof(title), "剪贴板历史  %d/%d", count, CLIPHIST_MAX_ENTRIES);
    g_font.drawText(innerX, boxY + pad + asc + 2, title);
    u8g2_DrawHLine(g_u8g2, innerX, boxY + pad + rowH - 2, innerW);

    // 选中项保持在可视区内。
    int maxScroll = count > kMenuRows ? count - kMenuRows : 0;
    if (s_scroll > maxScroll) s_scroll = maxScroll;
    if (s_scroll < 0) s_scroll = 0;
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + kMenuRows) s_scroll = s_sel - kMenuRows + 1;

    const int listY = boxY + pad + rowH;
    if (count == 0) {
        g_font.drawText(innerX + 4, listY + (kMenuRows * rowH) / 2 + asc / 2,
                        "（暂无剪贴板历史）");
    } else {
        for (int i = 0; i < kMenuRows; i++) {
            const int idx = s_scroll + i;
            if (idx >= count) break;
            const int rowTop = listY + i * rowH;
            const bool sel = (idx == s_sel);
            if (sel) {
                u8g2_SetDrawColor(g_u8g2, 0);
                u8g2_DrawBox(g_u8g2, innerX, rowTop, innerW, rowH);
                u8g2_SetDrawColor(g_u8g2, 1);
            }
            std::string line = entryPreview(idx, innerW - 8);
            g_font.drawText(innerX + 4, rowTop + 2 + asc, line.c_str());
            if (sel) u8g2_SetDrawColor(g_u8g2, 0);
        }
    }

    // 提示行多加了“w 网络”，放不下时降一级字号，避免压出面板。
    const char *hint = "↑↓ Enter粘贴 d删除 w网络 Esc关闭";
    if (g_font.textWidth(hint) > innerW) g_font.setSize(18);
    const int hintY = boxY + boxH - pad - g_font.descent() - 2;
    g_font.drawText(boxX + (boxW - g_font.textWidth(hint)) / 2, hintY, hint);

    g_font.setSize(prevSize);
    ui_send_buffer();
}

// ── 网络模式 ─────────────────────────────────────────────────────────────

static void drawWebPanel() {
    if (!g_u8g2 || !s_base) return;

    const int prevSize = g_font.fontSize();
    g_font.setSize(22);
    const int asc = g_font.ascent();
    const int rowH = g_font.lineHeight() + 6;
    const int pad = 8;
    const int boxW = 360;
    const int infoRows = 3;
    const int boxH = pad + rowH + infoRows * rowH + rowH + pad;
    const int boxX = (SCREEN_W - boxW) / 2;
    const int boxY = (SCREEN_H - boxH) / 2;
    const int innerX = boxX + pad;
    const int innerW = boxW - 2 * pad;

    memcpy(u8g2_GetBufferPtr(g_u8g2), s_base, s_baseSize);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);

    g_font.drawText(innerX, boxY + pad + asc + 2, "网络剪贴板");
    u8g2_DrawHLine(g_u8g2, innerX, boxY + pad + rowH - 2, innerW);

    int y = boxY + pad + rowH;
    char line[96];
    std::string ssid = g_settings.wifiSsid();
    snprintf(line, sizeof(line), "WiFi: %s", ssid.empty() ? "（未配置）" : ssid.c_str());
    g_font.drawText(innerX, y + asc, clampLine(line, innerW).c_str());
    y += rowH;

    if (s_webUrl[0]) snprintf(line, sizeof(line), "地址: %s", s_webUrl);
    else            snprintf(line, sizeof(line), "状态: %s",
                             s_webMsg[0] ? s_webMsg : "准备中…");
    g_font.drawText(innerX, y + asc, clampLine(line, innerW).c_str());
    y += rowH;

    snprintf(line, sizeof(line), "历史: %d/%d 条（最新在前）",
             cliphist_count(), CLIPHIST_MAX_ENTRIES);
    g_font.drawText(innerX, y + asc, clampLine(line, innerW).c_str());

    const char *hint = "电脑浏览器打开后可查看/编辑  Esc返回列表";
    if (g_font.textWidth(hint) > innerW) g_font.setSize(18);
    g_font.drawText(boxX + (boxW - g_font.textWidth(hint)) / 2,
                    boxY + boxH - pad - g_font.descent() - 2, hint);

    g_font.setSize(prevSize);
    ui_send_buffer();
}

// 停 HTTP 服务，并只在“WiFi 是本模式连的”时断开，避免踢掉用户原有的连接。
static void stopWebResources() {
    if (clip_web_active()) clip_web_stop();
    if (s_wifiOwned) {
        g_wifi.disconnect();
        s_wifiOwned = false;
    }
    s_webMode = false;
    s_webUrl[0] = '\0';
    s_webMsg[0] = '\0';
}

static void enterWebMode() {
    s_webMode = true;
    s_webUrl[0] = '\0';
    snprintf(s_webMsg, sizeof(s_webMsg), "正在连接 WiFi...");
    drawWebPanel();

    if (!g_wifi.isConnected()) {
        std::string ssid = g_settings.wifiSsid();
        if (ssid.empty()) {
            snprintf(s_webMsg, sizeof(s_webMsg), "未配置 WiFi，请先到设置中填写");
            drawWebPanel();
            return;
        }
        std::string pass = g_settings.wifiPassword();
        g_wifi.begin();
        if (!g_wifi.connect(ssid.c_str(), pass.c_str())) {
            g_wifi.disconnect();  // 连不上就把刚开的射频关掉
            snprintf(s_webMsg, sizeof(s_webMsg), "WiFi 连接失败");
            drawWebPanel();
            return;
        }
        s_wifiOwned = true;
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // connect() 已经等到 GOT_IP，正常一次就能取到；这里只是兜底重试。
    std::string ip;
    for (int i = 0; i < 5 && ip.empty(); ++i) {
        ip = g_wifi.getIp();
        if (ip.empty()) vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ip.empty()) {
        if (s_wifiOwned) { g_wifi.disconnect(); s_wifiOwned = false; }
        snprintf(s_webMsg, sizeof(s_webMsg), "获取 IP 失败");
        drawWebPanel();
        return;
    }

    if (!clip_web_start(80)) {
        if (s_wifiOwned) { g_wifi.disconnect(); s_wifiOwned = false; }
        snprintf(s_webMsg, sizeof(s_webMsg), "服务启动失败（端口被占用？）");
        drawWebPanel();
        return;
    }

    snprintf(s_webUrl, sizeof(s_webUrl), "http://%s/", ip.c_str());
    s_webMsg[0] = '\0';
    drawWebPanel();
}

bool clip_menu_active() { return s_active; }

bool clip_menu_open(AppState underlyingState) {
    if (s_active) return true;
    if (!g_u8g2) return false;

    const size_t size = u8g2_GetBufferSize(g_u8g2);
    if (size == 0) return false;
    if (!s_base) {
        s_base = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_base) {
            ESP_LOGW(TAG, "PSRAM frame copy failed, falling back to internal RAM");
            s_base = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
        }
        if (!s_base) {
            ESP_LOGE(TAG, "frame copy alloc failed (%u bytes)", (unsigned)size);
            return false;
        }
        s_baseSize = size;
    } else if (s_baseSize != size) {
        return false;  // 缓冲区尺寸变化不应发生；保守放弃
    }

    memcpy(s_base, u8g2_GetBufferPtr(g_u8g2), size);
    s_state = underlyingState;
    s_sel = 0;
    s_scroll = 0;
    s_lastDeleteUs = 0;
    s_active = true;
    g_ime.cancelComposition();  // 只丢未上屏的拼音组合，保留输入法开关状态
    drawMenu();
    return true;
}

void clip_menu_close(bool restore) {
    if (!s_active) return;
    stopWebResources();
    s_active = false;
    if (restore && s_base && g_u8g2 && s_baseSize == u8g2_GetBufferSize(g_u8g2)) {
        memcpy(u8g2_GetBufferPtr(g_u8g2), s_base, s_baseSize);
        ui_send_buffer();
    }
}

void clip_menu_handle(int key) {
    if (!s_active) return;

    // 网络模式：Esc 停服回列表，Win+V 直接关掉整个菜单；其余键吞掉，
    // 避免和底层界面快捷键混在一起（服务运行期间由浏览器改条目）。
    // 这个分支不读历史表：HTTP 任务正在增删条目，主循环不要并发访问。
    if (s_webMode) {
        if (key == 0x1B) {
            stopWebResources();   // 同步停服，返回后 HTTP 任务已结束
            int count = cliphist_count();  // 浏览器可能删过条目，夹回范围
            if (s_sel >= count) s_sel = count - 1;
            if (s_sel < 0) s_sel = 0;
            drawMenu();
        } else if (key == KEY_WIN_V) {
            clip_menu_close(true);
        }
        return;
    }

    int count = cliphist_count();
    if (s_sel >= count) s_sel = count - 1;
    if (s_sel < 0) s_sel = 0;

    if (key == 'w' || key == 'W') {
        enterWebMode();
        return;
    }
    if (key == KEY_UP || key == 'k') {
        if (s_sel > 0) s_sel--;
        drawMenu();
        return;
    }
    if (key == KEY_DOWN || key == 'j') {
        if (s_sel + 1 < count) s_sel++;
        drawMenu();
        return;
    }
    if (key == KEY_WIN_V || key == 0x1B) {
        clip_menu_close(true);
        return;
    }
    if (key == 'd' || key == 'D') {
        // 300 ms 去抖：按住 d 的自动重复不应把历史清空。
        const int64_t now = esp_timer_get_time();
        if (count > 0 && now - s_lastDeleteUs >= 300000) {
            s_lastDeleteUs = now;
            cliphist_remove(s_sel);
            count = cliphist_count();
            if (s_sel >= count) s_sel = count - 1;
            if (s_sel < 0) s_sel = 0;
            drawMenu();
        }
        return;
    }
    if (key == 0x0A || key == 0x0D) {
        if (count == 0) return;
        std::string text(cliphist_text(s_sel), cliphist_len(s_sel));
        const bool linewise = cliphist_linewise(s_sel);
        // 从历史里选中不算一次新的复制：只更新当前剪贴板，不重排历史。
        clipboardSet(text, linewise, false);
        const AppState st = s_state;
        clip_menu_close(true);
        clip_paste_into_active_input(st, text);
        return;
    }
    // 其余按键吞掉，避免菜单开着时触发底层界面的快捷键。
}

bool clip_paste_into_active_input(AppState state, const std::string &text) {
    switch (state) {
    case APP_EDITOR:        return app_editor_paste_text(text);
    case APP_INSPIRATION:   return screen_inspiration_paste_text(text);
    case APP_FLOMO:         return screen_flomo_paste_text(text);
    case APP_GTD:           return screen_gtd_paste_text(text);
    case APP_OUTLINE:       return screen_outline_paste_text(text);
    case APP_SETTINGS:      return screen_settings_paste_text(text);
    case APP_POLISH:        return screen_polish_paste_text(text);
    case APP_POLISH_PROMPT: return screen_polish_prompt_paste_text(text);
    default:                return false;
    }
}
