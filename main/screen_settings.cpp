#include "screen_settings.h"
#include "font_renderer.h"
#include "settings_manager.h"
#include "wifi_manager.h"
#include "flomo_client.h"
#include "ime/IME.h"
#include "pcf85063.h"
#include "ui_helpers.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_timer.h>
#include <esp_sntp.h>

extern void *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
}

// ── Settings state ────────────────────────────────────────────────────────
struct SettingField { const char *key; const char *label; bool masked; bool action; };
static const SettingField SETTINGS_FIELDS[] = {
    {"_file_mgr", "文件管理", false, true},
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
    {"auto_save", "自动保存", false, false},
    {"_sync_time", "网络同步时间", false, true},
};
static const int NUM_SETTINGS = sizeof(SETTINGS_FIELDS) / sizeof(SETTINGS_FIELDS[0]);

static struct {
    int selection = 0;
    int scroll = 0;
    bool editing = false;
    std::string editBuffer;
    int editCursor = 0;
    bool imeActive = false;
} g_settingsState;

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

// ── Screen entry points ──────────────────────────────────────────────────
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
        if (g_settingsState.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g_settingsState.editBuffer.insert(g_settingsState.editCursor, imeOut);
                    g_settingsState.editCursor += (int)imeOut.length();
                }
                ui_clear();
                auto &f = SETTINGS_FIELDS[g_settingsState.selection];
                ui_draw_text_centered(28, f.label, false, true);
                u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

                std::string display = g_settingsState.editBuffer;
                if (f.masked) display = std::string(display.length(), '*');
                if (display.empty()) display = " ";
                int textY = FONT_H + 32;
                int cx = g_font.textWidth(display.substr(0, g_settingsState.editCursor).c_str());
                ui_draw_text(4, textY, display.c_str());

                std::string cursorChar = display.substr(g_settingsState.editCursor, 1);
                int cw = cursorChar.empty() || cursorChar == " " ? 8 : g_font.textWidth(cursorChar.c_str());
                u8g2_SetDrawColor(g_u8g2, 0);
                u8g2_DrawBox(g_u8g2, 4 + cx, textY + 4, cw, 3);
                u8g2_SetDrawColor(g_u8g2, 1);

                if (g_ime.composing()) {
                    std::string code = g_ime.displayCode();
                    auto &cands = g_ime.candidates();
                    int pageSize = g_ime.pageSize();
                    int curPage = g_ime.currentPage();
                    int total = g_ime.totalCandidates();
                    int totalPages = (total + pageSize - 1) / pageSize;
                    if (totalPages < 1) totalPages = 1;

                    char pageInfo[32];
                    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
                    int imeY = SCREEN_H - 67;
                    u8g2_DrawBox(g_u8g2, 0, imeY, SCREEN_W, 67);
                    u8g2_SetDrawColor(g_u8g2, 1);

                    int cw = g_font.textWidth(code.c_str()) + 8;
                    u8g2_DrawBox(g_u8g2, 4, imeY + 4, cw, FONT_H);
                    u8g2_SetDrawColor(g_u8g2, 0);
                    g_font.drawText(4, imeY + 4 + FONT_H - 6, code.c_str(), false);
                    u8g2_SetDrawColor(g_u8g2, 1);

                    int tw = g_font.textWidth(pageInfo);
                    int pw = tw + 8;
                    int px = SCREEN_W - pw - 4;
                    u8g2_DrawBox(g_u8g2, px, imeY + 4, pw, FONT_H);
                    u8g2_SetDrawColor(g_u8g2, 0);
                    g_font.drawText(px + 4, imeY + 4 + FONT_H - 6, pageInfo, false);
                    u8g2_SetDrawColor(g_u8g2, 1);

                    u8g2_SetDrawColor(g_u8g2, 0);
                    u8g2_DrawHLine(g_u8g2, 0, imeY + FONT_H + 4, SCREEN_W);
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
            if (g_settingsState.editCursor > 0) {
                int prev = g_settingsState.editCursor - 1;
                while (prev > 0 && ((unsigned char)g_settingsState.editBuffer[prev] & 0xC0) == 0x80)
                    prev--;
                g_settingsState.editBuffer.erase(prev, g_settingsState.editCursor - prev);
                g_settingsState.editCursor = prev;
            }
        } else if (key == KEY_LEFT) {
            if (g_settingsState.editCursor > 0) {
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

        int textY = FONT_H + 32;
        int maxW = SCREEN_W - 8;
        std::vector<std::pair<int,int>> lines;
        int lineStart = 0;
        while (lineStart < (int)display.length()) {
            int lineEnd = lineStart;
            int lastGood = lineStart;
            while (lineEnd <= (int)display.length()) {
                int w = g_font.textWidth(display.substr(lineStart, lineEnd - lineStart).c_str());
                if (w > maxW) {
                    lineEnd = lastGood;
                    break;
                }
                lastGood = lineEnd;
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

        for (int i = 0; i < (int)lines.size(); i++) {
            std::string lineText = display.substr(lines[i].first, lines[i].second - lines[i].first);
            if (lineText.empty() && i == 0) lineText = " ";
            ui_draw_text(4, textY + i * FONT_H, lineText.c_str());
        }

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

        std::string cursorChar = display.substr(g_settingsState.editCursor, 1);
        int cw = cursorChar.empty() || cursorChar == " " ? 8 : g_font.textWidth(cursorChar.c_str());
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, 4 + cursorX, textY + cursorLine * FONT_H + 4, cw, 3);
        u8g2_SetDrawColor(g_u8g2, 1);

        if (g_settingsState.imeActive && g_ime.composing()) {
            std::string code = g_ime.displayCode();
            auto &cands = g_ime.candidates();
            int pageSize = g_ime.pageSize();
            int curPage = g_ime.currentPage();
            int total = g_ime.totalCandidates();
            int totalPages = (total + pageSize - 1) / pageSize;
            if (totalPages < 1) totalPages = 1;

            char pageInfo[32];
            snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
            int imeY = SCREEN_H - 67;
            u8g2_DrawBox(g_u8g2, 0, imeY, SCREEN_W, 67);
            u8g2_SetDrawColor(g_u8g2, 1);

            int cw = g_font.textWidth(code.c_str()) + 8;
            u8g2_DrawBox(g_u8g2, 4, imeY + 4, cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, imeY + 4 + FONT_H - 6, code.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);

            int tw = g_font.textWidth(pageInfo);
            int pw = tw + 8;
            int px = SCREEN_W - pw - 4;
            u8g2_DrawBox(g_u8g2, px, imeY + 4, pw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(px + 4, imeY + 4 + FONT_H - 6, pageInfo, false);
            u8g2_SetDrawColor(g_u8g2, 1);

            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawHLine(g_u8g2, 0, imeY + FONT_H + 4, SCREEN_W);
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
                    vTaskDelay(pdMS_TO_TICKS(500));
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
                        g_rtc.setTime(now);
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
            if (strcmp(f.key, "_file_mgr") == 0) {
                ctx.nextState = APP_FILE_MANAGER;
                return APP_FILE_MANAGER;
            }
        } else {
            g_settingsState.editBuffer = g_settings.getString(f.key);
            g_settingsState.editCursor = (int)g_settingsState.editBuffer.length();
            g_settingsState.editing = true;
            g_settingsState.imeActive = false;
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
