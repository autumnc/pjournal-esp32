#include "screen_settings.h"
#include "font_renderer.h"
#include "settings_manager.h"
#include "wifi_manager.h"
#include "flomo_client.h"
#include "ime/IME.h"
#include "pcf85063.h"
#include "quick_edit.h"
#include "typing_click.h"
#include "ui_helpers.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <set>
#include <vector>
#include <esp_timer.h>
#include <esp_sntp.h>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
}

// ── Settings state ────────────────────────────────────────────────────────
struct SettingField { const char *key; const char *label; bool masked; bool action; };
static const SettingField SETTINGS_FIELDS[] = {
    {"_app_mode", "工作模式", false, true},
    {"_home_view", "主页视图", false, true},
    {"_editor_orientation", "文字方向", false, true},
    {"_input_mode", "输入模式", false, true},
    {"_click_chinese", "中文音效触发", false, true},
    {"_click_volume", "打字音效音量", false, true},
    {"_click_timbre", "打字音效音色", false, true},
    {"_file_mgr", "文件管理", false, true},
    {"_dict_mgr", "词库管理", false, true},
    {"file_mgr_token", "文件管理密码", true, false},
    {"_bt_manage", "蓝牙设备管理", false, true},
    {"deepseek_key", "Deepseek Key", false, false},
    {"_polish_prompt", "润色提示词", false, true},
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
    {"recovery_draft", "恢复草稿", false, false},
    {"version_history", "版本历史", false, false},
    {"auto_sleep", "自动休眠", false, false},
    {"sleep_screen", "休眠保留画面", false, false},
    {"md_render", "Markdown渲染", false, false},
    {"first_line_indent", "首行缩进", false, false},
    {"_font_size", "字体大小", false, true},
    {"_sync_time", "网络同步时间", false, true},
};
static const int NUM_SETTINGS = sizeof(SETTINGS_FIELDS) / sizeof(SETTINGS_FIELDS[0]);

// 打字机专用设置行仅在该模式开启时显示
static bool fieldHidden(int idx) {
    const char *k = SETTINGS_FIELDS[idx].key;
    if (strcmp(k, "_click_volume") == 0 || strcmp(k, "_click_timbre") == 0 ||
        strcmp(k, "_click_chinese") == 0)
        return g_settings.inputMode() != "typewriter";
    return false;
}

// 打字机音色 key↔中文名,顺序即设置内循环顺序(与 typing_click.cpp TIMBRES 同步)
struct TimbreOpt { const char *key; const char *label; };
static const TimbreOpt TIMBRE_OPTS[] = {
    {"mechanical", "机械"}, {"soft", "柔和"}, {"electronic", "电子"},
    {"clack", "打字机"}, {"wooden", "木鱼"}, {"crisp", "清脆"}, {"chime", "风铃"},
};
static int timbreIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(TIMBRE_OPTS) / sizeof(TIMBRE_OPTS[0])); i++)
        if (strcmp(k, TIMBRE_OPTS[i].key) == 0) return i;
    return 0;
}
static const char *timbreNext(int idx) {
    int n = (int)(sizeof(TIMBRE_OPTS) / sizeof(TIMBRE_OPTS[0]));
    return TIMBRE_OPTS[(idx + 1) % n].key;
}
// 中文音效触发 key↔中文名(顺序即循环顺序)
static const TimbreOpt CLICK_CHINESE_OPTS[] = {
    {"key", "按键触发"}, {"count", "上屏触发(按字数)"}, {"single", "上屏触发(单声)"},
};
static int clickChineseIndex(const char *k) {
    for (int i = 0; i < (int)(sizeof(CLICK_CHINESE_OPTS) / sizeof(CLICK_CHINESE_OPTS[0])); i++)
        if (strcmp(k, CLICK_CHINESE_OPTS[i].key) == 0) return i;
    return 0;
}
static const char *clickChineseNext(int idx) {
    int n = (int)(sizeof(CLICK_CHINESE_OPTS) / sizeof(CLICK_CHINESE_OPTS[0]));
    return CLICK_CHINESE_OPTS[(idx + 1) % n].key;
}
// UI 序号(跳过隐藏行)→ SETTINGS_FIELDS 真实下标;越界返回最后一个可见行
static int fieldAt(int sel) {
    int lastVisible = -1, vis = 0;
    for (int i = 0; i < NUM_SETTINGS; i++) {
        if (fieldHidden(i)) continue;
        if (vis == sel) return i;
        lastVisible = i;
        vis++;
    }
    return lastVisible >= 0 ? lastVisible : 0;
}
static int fieldVisibleCount() {
    int n = 0;
    for (int i = 0; i < NUM_SETTINGS; i++)
        if (!fieldHidden(i)) n++;
    return n;
}

// 布尔型开关项:显示 开/关,Enter 在 "0"/"1" 间切换
static bool isToggleField(const char *key) {
    return strcmp(key, "auto_save") == 0 || strcmp(key, "auto_sleep") == 0 ||
           strcmp(key, "sleep_screen") == 0 || strcmp(key, "md_render") == 0 ||
           strcmp(key, "first_line_indent") == 0 || strcmp(key, "version_history") == 0 ||
           strcmp(key, "recovery_draft") == 0;
}

static bool toggleValue(const char *key) {
    std::string v = g_settings.getString(key);
    if (strcmp(key, "auto_sleep") == 0) return v != "0";  // 默认开
    if (strcmp(key, "md_render") == 0) return v != "0";   // 默认开
    if (strcmp(key, "recovery_draft") == 0) return v != "0";  // 默认开
    return v == "1";  // auto_save: 默认关
}

enum SettingsScreenMode { SETTINGS_BROWSE, SETTINGS_DICT_CHOOSE, SETTINGS_DICT_LIST, SETTINGS_DICT_ADD };

static struct {
    int selection = 0;
    int scroll = 0;
    bool editing = false;
    std::string editBuffer;
    int editCursor = 0;
    bool imeActive = false;
    SettingsScreenMode mode = SETTINGS_BROWSE;
    IME::UserDictKind dictKind = IME::FIXED_DICT;
    int dictSelection = 0;
    int dictScroll = 0;
    std::set<int> dictSelected;
    std::string dictAddBuffer;
    int dictAddCursor = 0;
    bool dictAddImeActive = false;
    bool dictSearching = false;
    std::string dictSearchBuffer;
    int dictSearchCursor = 0;
    bool dictSearchImeActive = false;
} g_settingsState;

static const char *dictKindLabel(IME::UserDictKind kind) {
    return kind == IME::FIXED_DICT ? "固定词库" : "动态词库";
}

static std::string settingsTrim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static void moveCursorLeft(std::string &s, int &cursor) {
    if (cursor <= 0) return;
    cursor--;
    while (cursor > 0 && ((unsigned char)s[cursor] & 0xC0) == 0x80) cursor--;
}

static void moveCursorRight(std::string &s, int &cursor) {
    if (cursor >= (int)s.length()) return;
    cursor++;
    while (cursor < (int)s.length() && ((unsigned char)s[cursor] & 0xC0) == 0x80) cursor++;
}

static void eraseBeforeCursor(std::string &s, int &cursor) {
    if (cursor <= 0) return;
    int prev = cursor - 1;
    while (prev > 0 && ((unsigned char)s[prev] & 0xC0) == 0x80) prev--;
    s.erase(prev, cursor - prev);
    cursor = prev;
}

static std::vector<int> dictFilteredIndices(const std::vector<IME::UserEntryView> &entries);

static void drawDictChoose() {
    ui_clear();
    ui_draw_text_centered(FONT_H, "词库管理", false, true);
    ui_draw_text(8, FONT_H * 3, "固定词库", g_settingsState.dictSelection == 0);
    ui_draw_text(8, FONT_H * 4, "动态词库", g_settingsState.dictSelection == 1);
    ui_draw_status("Enter进入 Esc返回", "");
    ui_commit();
}

static void drawDictList(bool doCommit = true) {
    auto entries = g_ime.userDictEntries(g_settingsState.dictKind);
    auto filtered = dictFilteredIndices(entries);
    int total = (int)filtered.size();
    if (g_settingsState.dictSelection >= total) g_settingsState.dictSelection = total - 1;
    if (g_settingsState.dictSelection < 0) g_settingsState.dictSelection = 0;

    ui_clear();
    char title[64];
    snprintf(title, sizeof(title), "%s %d/%d", dictKindLabel(g_settingsState.dictKind),
             (int)entries.size(), g_settingsState.dictKind == IME::FIXED_DICT ? 500 : 1000);
    ui_draw_text_centered(FONT_H, title, false, true);

    // 表格底部贴住状态栏分割线:由最后一行单元格底边=STATUS_Y反推表头基线,
    // 空出的顶部余量让表头整体下移,能多放一行就多放一行
    int lastBase = STATUS_Y - FONT_H + g_font.ascent();
    int visible = (lastBase - FONT_H * 2) / FONT_H;
    int startY = lastBase - visible * FONT_H;
    if (visible < 1) visible = 1;
    if (g_settingsState.dictSelection < g_settingsState.dictScroll)
        g_settingsState.dictScroll = g_settingsState.dictSelection;
    if (g_settingsState.dictSelection >= g_settingsState.dictScroll + visible)
        g_settingsState.dictScroll = g_settingsState.dictSelection - visible + 1;

    if (total == 0) {
        ui_draw_text_centered(FONT_H * 4, entries.empty() ? "暂无词条" : "无匹配词条");
    } else {
        u8g2_SetDrawColor(g_u8g2, 0);
        ui_draw_text(8, startY, "  编码", false, true);
        ui_draw_text(120, startY, "候选词", false, true);
        ui_draw_text(320, startY, "频次", false, true);
        for (int i = 0; i < visible && g_settingsState.dictScroll + i < total; i++) {
            int viewIdx = g_settingsState.dictScroll + i;
            int idx = filtered[viewIdx];
            bool sel = viewIdx == g_settingsState.dictSelection;
            bool marked = g_settingsState.dictSelected.count(idx) > 0;
            int rowY = startY + (i + 1) * FONT_H;
            if (rowY >= STATUS_Y) break;
            if (sel) {
                u8g2_SetDrawColor(g_u8g2, 0);
                u8g2_DrawBox(g_u8g2, 0, rowY - g_font.ascent(), SCREEN_W, FONT_H);
                u8g2_SetDrawColor(g_u8g2, 1);
            } else {
                u8g2_SetDrawColor(g_u8g2, 0);
            }
            char mark[2] = { marked ? '*' : ' ', 0 };
            char count[16];
            snprintf(count, sizeof(count), "%d", entries[idx].count);
            g_font.drawText(8, rowY, mark, false);
            g_font.drawText(28, rowY, entries[idx].code.c_str(), false);
            g_font.drawText(120, rowY, entries[idx].word.c_str(), false);
            g_font.drawText(320, rowY, count, false);
            u8g2_SetDrawColor(g_u8g2, 0);
        }
    }
    char left[48];
    snprintf(left, sizeof(left), "a添加 d删 /搜 已选%d", (int)g_settingsState.dictSelected.size());
    std::string right = g_settingsState.dictSearchBuffer.empty() ? "Space多选" : ("/" + g_settingsState.dictSearchBuffer);
    ui_draw_status(left, right.c_str());
    if (doCommit) ui_commit();
}

static void drawDictSearch() {
    drawDictList(false);
    bool composing = g_settingsState.dictSearchImeActive && g_ime.composing();
    // 上移6px:面板底边不压状态栏分割线(面板高 FONT_H*2+8,原底边超出分割线4px)
    int y = composing ? (STATUS_Y - 67 - FONT_H * 2 - 10) : (STATUS_Y - FONT_H * 2 - 10);
    if (y < FONT_H * 2) y = FONT_H * 2;
    // ui_draw_status 结束时 draw color=1(白),此处必须显式设色:白底黑字
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, y, SCREEN_W, FONT_H * 2 + 8);
    u8g2_SetDrawColor(g_u8g2, 0);
    ui_draw_text(4, y + FONT_H, "搜索编码或候选词");
    std::string display = g_settingsState.dictSearchBuffer.empty() ? " " : g_settingsState.dictSearchBuffer;
    ui_draw_text(4, y + FONT_H * 2, display.c_str());
    int cx = g_font.textWidth(display.substr(0, g_settingsState.dictSearchCursor).c_str());
    u8g2_DrawBox(g_u8g2, 4 + cx, y + FONT_H * 2 + 4, 8, 3);
    u8g2_SetDrawColor(g_u8g2, 1);
    if (composing) drawIMEUI(STATUS_Y - 67, true);
    ui_commit();
}

static void drawDictAdd() {
    ui_clear();
    char title[64];
    snprintf(title, sizeof(title), "添加%s", dictKindLabel(g_settingsState.dictKind));
    ui_draw_text_centered(FONT_H, title, false, true);
    ui_draw_text(4, FONT_H * 3, "格式: code word");
    std::string display = g_settingsState.dictAddBuffer.empty() ? " " : g_settingsState.dictAddBuffer;
    ui_draw_text(4, FONT_H * 4, display.c_str());
    int cx = g_font.textWidth(display.substr(0, g_settingsState.dictAddCursor).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, 4 + cx, FONT_H * 4 + 4, 8, 3);
    u8g2_SetDrawColor(g_u8g2, 1);
    ui_draw_status("Enter确定 Esc取消", "Ctrl+Space中文");
    if (g_settingsState.dictAddImeActive && g_ime.composing())
        drawIMEUI(STATUS_Y - 67, true);
    ui_commit();
}

static bool parseDictAdd(const std::string &line, std::string &code, std::string &word) {
    std::string s = settingsTrim(line);
    size_t sp = s.find(' ');
    if (sp == std::string::npos) return false;
    code = settingsTrim(s.substr(0, sp));
    word = settingsTrim(s.substr(sp + 1));
    return !code.empty() && !word.empty();
}

static std::vector<int> dictFilteredIndices(const std::vector<IME::UserEntryView> &entries) {
    std::vector<int> out;
    std::string q = settingsTrim(g_settingsState.dictSearchBuffer);
    for (int i = 0; i < (int)entries.size(); i++) {
        if (q.empty() || entries[i].code.find(q) != std::string::npos ||
            entries[i].word.find(q) != std::string::npos)
            out.push_back(i);
    }
    return out;
}

static bool connect_wifi_from_settings() {
    std::string ssid = g_settings.wifiSsid();
    if (ssid.empty()) return false;
    std::string pass = g_settings.wifiPassword();
    g_wifi.begin();
    return g_wifi.connect(ssid.c_str(), pass.c_str());
}

// ── Screen entry points ──────────────────────────────────────────────────
void screen_settings_init() {
    g_settingsState.selection = g_settingsState.scroll = 0;
    g_settingsState.editing = false;
    g_settingsState.editBuffer.clear();
    g_settingsState.editCursor = 0;
    g_settingsState.imeActive = false;
    g_settingsState.mode = SETTINGS_BROWSE;
    g_settingsState.dictKind = IME::FIXED_DICT;
    g_settingsState.dictSelection = 0;
    g_settingsState.dictScroll = 0;
    g_settingsState.dictSelected.clear();
    g_settingsState.dictAddBuffer.clear();
    g_settingsState.dictAddCursor = 0;
    g_settingsState.dictAddImeActive = false;
    g_settingsState.dictSearching = false;
    g_settingsState.dictSearchBuffer.clear();
    g_settingsState.dictSearchCursor = 0;
    g_settingsState.dictSearchImeActive = false;
}

AppState screen_settings_handle(int key, ScreenContext &ctx) {
    // ── User dictionary manager ───────────────────────────────────────
    if (g_settingsState.mode == SETTINGS_DICT_CHOOSE) {
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g_settingsState.mode = SETTINGS_BROWSE;
        } else if (key == KEY_UP || key == 'k' || key == KEY_DOWN || key == 'j') {
            g_settingsState.dictSelection = 1 - g_settingsState.dictSelection;
        } else if (key == 0x0A || key == 0x0D) {
            g_settingsState.dictKind = g_settingsState.dictSelection == 0 ? IME::FIXED_DICT : IME::DYNAMIC_DICT;
            g_settingsState.dictSelection = 0;
            g_settingsState.dictScroll = 0;
            g_settingsState.dictSelected.clear();
            g_settingsState.dictSearchBuffer.clear();
            g_settingsState.dictSearchCursor = 0;
            g_settingsState.mode = SETTINGS_DICT_LIST;
        }
        drawDictChoose();
        return APP_SETTINGS;
    }

    if (g_settingsState.mode == SETTINGS_DICT_LIST && g_settingsState.dictSearching) {
        if (g_settingsState.dictSearchImeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g_settingsState.dictSearchBuffer.insert(g_settingsState.dictSearchCursor, imeOut);
                    g_settingsState.dictSearchCursor += (int)imeOut.length();
                }
                drawDictSearch();
                return APP_SETTINGS;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g_settingsState.dictSearchImeActive = !g_settingsState.dictSearchImeActive;
            g_ime.setActive(g_settingsState.dictSearchImeActive);
        } else if (key == 0x1B) {
            g_settingsState.dictSearching = false;
            g_settingsState.dictSearchImeActive = false;
            g_ime.setActive(false);
        } else if (key == 0x0A || key == 0x0D) {
            g_settingsState.dictSearching = false;
            g_settingsState.dictSelection = 0;
            g_settingsState.dictScroll = 0;
            g_settingsState.dictSearchImeActive = false;
            g_ime.setActive(false);
        } else if (key == 0x7F || key == 0x08) {
            eraseBeforeCursor(g_settingsState.dictSearchBuffer, g_settingsState.dictSearchCursor);
            g_settingsState.dictSelection = 0;
            g_settingsState.dictScroll = 0;
        } else if (key == KEY_LEFT) {
            moveCursorLeft(g_settingsState.dictSearchBuffer, g_settingsState.dictSearchCursor);
        } else if (key == KEY_RIGHT) {
            moveCursorRight(g_settingsState.dictSearchBuffer, g_settingsState.dictSearchCursor);
        } else if (key >= 0x20 && key <= 0x7E) {
            g_settingsState.dictSearchBuffer.insert(g_settingsState.dictSearchCursor, 1, (char)key);
            g_settingsState.dictSearchCursor++;
            g_settingsState.dictSelection = 0;
            g_settingsState.dictScroll = 0;
        }
        drawDictSearch();
        return APP_SETTINGS;
    }

    if (g_settingsState.mode == SETTINGS_DICT_LIST) {
        auto entries = g_ime.userDictEntries(g_settingsState.dictKind);
        auto filtered = dictFilteredIndices(entries);
        int total = (int)filtered.size();
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g_settingsState.mode = SETTINGS_DICT_CHOOSE;
            g_settingsState.dictSelected.clear();
            g_settingsState.dictSearchBuffer.clear();
        } else if (key == KEY_UP || key == 'k') {
            if (g_settingsState.dictSelection > 0) g_settingsState.dictSelection--;
        } else if (key == KEY_DOWN || key == 'j') {
            if (g_settingsState.dictSelection < total - 1) g_settingsState.dictSelection++;
        } else if (key == '/') {
            g_settingsState.dictSearching = true;
            g_settingsState.dictSearchCursor = (int)g_settingsState.dictSearchBuffer.length();
            g_settingsState.dictSearchImeActive = false;
        } else if (key == 'a' || key == 'A') {
            g_settingsState.mode = SETTINGS_DICT_ADD;
            g_settingsState.dictAddBuffer.clear();
            g_settingsState.dictAddCursor = 0;
            g_settingsState.dictAddImeActive = false;
            g_ime.setActive(false);
        } else if (key == ' ' && total > 0) {
            int realIdx = filtered[g_settingsState.dictSelection];
            if (g_settingsState.dictSelected.count(realIdx)) g_settingsState.dictSelected.erase(realIdx);
            else g_settingsState.dictSelected.insert(realIdx);
        } else if ((key == 'd' || key == 'D') && total > 0) {
            std::vector<int> indices;
            if (g_settingsState.dictSelected.empty()) {
                indices.push_back(filtered[g_settingsState.dictSelection]);
            } else {
                for (int idx : g_settingsState.dictSelected) indices.push_back(idx);
            }
            g_ime.removeUserDictEntries(g_settingsState.dictKind, indices);
            g_settingsState.dictSelected.clear();
            if (g_settingsState.dictSelection >= total - (int)indices.size())
                g_settingsState.dictSelection = std::max(0, total - (int)indices.size() - 1);
        }
        drawDictList();
        return APP_SETTINGS;
    }

    if (g_settingsState.mode == SETTINGS_DICT_ADD) {
        if (g_settingsState.dictAddImeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g_settingsState.dictAddBuffer.insert(g_settingsState.dictAddCursor, imeOut);
                    g_settingsState.dictAddCursor += (int)imeOut.length();
                }
                drawDictAdd();
                return APP_SETTINGS;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g_settingsState.dictAddImeActive = !g_settingsState.dictAddImeActive;
            g_ime.setActive(g_settingsState.dictAddImeActive);
        } else if (key == 0x1B) {
            g_settingsState.mode = SETTINGS_DICT_LIST;
            g_settingsState.dictAddBuffer.clear();
            g_settingsState.dictAddCursor = 0;
            g_settingsState.dictAddImeActive = false;
            g_ime.setActive(false);
        } else if (key == 0x0A || key == 0x0D) {
            std::string code, word;
            if (parseDictAdd(g_settingsState.dictAddBuffer, code, word))
                g_ime.addUserDictEntry(g_settingsState.dictKind, code, word);
            g_settingsState.mode = SETTINGS_DICT_LIST;
            g_settingsState.dictAddBuffer.clear();
            g_settingsState.dictAddCursor = 0;
            g_settingsState.dictAddImeActive = false;
            g_ime.setActive(false);
        } else if (key == 0x7F || key == 0x08) {
            eraseBeforeCursor(g_settingsState.dictAddBuffer, g_settingsState.dictAddCursor);
        } else if (key == KEY_LEFT) {
            moveCursorLeft(g_settingsState.dictAddBuffer, g_settingsState.dictAddCursor);
        } else if (key == KEY_RIGHT) {
            moveCursorRight(g_settingsState.dictAddBuffer, g_settingsState.dictAddCursor);
        } else if (key >= 0x20 && key <= 0x7E) {
            g_settingsState.dictAddBuffer.insert(g_settingsState.dictAddCursor, 1, (char)key);
            g_settingsState.dictAddCursor++;
        }
        drawDictAdd();
        return APP_SETTINGS;
    }

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
                auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
                ui_draw_text_centered(28, f.label, false, true);
                int sepY = 28 + g_font.descent() + 4;
                u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

                std::string display = g_settingsState.editBuffer;
                if (f.masked) display = std::string(display.length(), '*');
                if (display.empty()) display = " ";
                int textY = sepY + 4 + g_font.ascent();
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
                    int totalPages = g_ime.totalPages();
                    if (totalPages < 1) totalPages = 1;

                    char pageInfo[32];
                    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
                    int imeY = SCREEN_H - 67;
                    u8g2_DrawBox(g_u8g2, 0, imeY, SCREEN_W, 67);
                    u8g2_SetDrawColor(g_u8g2, 1);

                    int cw = g_font.textWidth(code.c_str()) + 8;
                    u8g2_DrawBox(g_u8g2, 4, imeY + 4, cw, FONT_H);
                    u8g2_SetDrawColor(g_u8g2, 0);
                    g_font.drawText(4, imeY + 4 + g_font.ascent(), code.c_str(), false);
                    u8g2_SetDrawColor(g_u8g2, 1);

                    int tw = g_font.textWidth(pageInfo);
                    int pw = tw + 8;
                    int px = SCREEN_W - pw - 4;
                    u8g2_DrawBox(g_u8g2, px, imeY + 4, pw, FONT_H);
                    u8g2_SetDrawColor(g_u8g2, 0);
                    g_font.drawText(px + 4, imeY + 4 + g_font.ascent(), pageInfo, false);
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
                        g_font.drawText(4, imeY + FONT_H + 8 + g_font.ascent(), candLine.c_str(), false);
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
            auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
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
        auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
        ui_draw_text_centered(28, f.label, false, true);
        int sepY = 28 + g_font.descent() + 4;
        u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

        std::string display = g_settingsState.editBuffer;
        if (f.masked) display = std::string(display.length(), '*');

        int textY = sepY + 4 + g_font.ascent();
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
            int totalPages = g_ime.totalPages();
            if (totalPages < 1) totalPages = 1;

            char pageInfo[32];
            snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
            int imeY = SCREEN_H - 67;
            u8g2_DrawBox(g_u8g2, 0, imeY, SCREEN_W, 67);
            u8g2_SetDrawColor(g_u8g2, 1);

            int cw = g_font.textWidth(code.c_str()) + 8;
            u8g2_DrawBox(g_u8g2, 4, imeY + 4, cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, imeY + 4 + g_font.ascent(), code.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);

            int tw = g_font.textWidth(pageInfo);
            int pw = tw + 8;
            int px = SCREEN_W - pw - 4;
            u8g2_DrawBox(g_u8g2, px, imeY + 4, pw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(px + 4, imeY + 4 + g_font.ascent(), pageInfo, false);
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
                g_font.drawText(4, imeY + FONT_H + 8 + g_font.ascent(), candLine.c_str(), false);
                u8g2_SetDrawColor(g_u8g2, 0);
            }
        }

        ui_commit();
        return APP_SETTINGS;
    }

    // ── Browse mode ────────────────────────────────────────────────────
    if (key == 'q' || key == 'Q' || key == 0x1B) {
        ctx.nextState = g_quickEdit ? APP_EDITOR : APP_MAIN;
        return ctx.nextState;
    }
    if (key == 'k' || key == KEY_UP) { if (g_settingsState.selection > 0) g_settingsState.selection--; }
    if (key == 'j' || key == KEY_DOWN) { if (g_settingsState.selection < fieldVisibleCount()-1) g_settingsState.selection++; }
    if (key == 'd' || key == 'D') {
        auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
        if (!f.action) g_settings.erase(SETTINGS_FIELDS[fieldAt(g_settingsState.selection)].key);
    }
    if (key == 0x0A || key == 0x0D) {
        auto &f = SETTINGS_FIELDS[fieldAt(g_settingsState.selection)];
        if (f.action) {
            if (strcmp(f.key, "_app_mode") == 0) {
                std::string next = (g_settings.appMode() == "quick") ? "journal" : "quick";
                g_settings.setString("app_mode", next);
                ctx.statusMessage = "切换模式需重启生效";
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_home_view") == 0) {
                std::string next = (g_settings.homeView() == "month") ? "week" : "month";
                g_settings.setString("home_view", next);
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_editor_orientation") == 0) {
                std::string next = (g_settings.editorOrientation() == "vertical") ? "horizontal" : "vertical";
                g_settings.setString("editor_orientation", next);
                return APP_SETTINGS;
            }
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
            if (strcmp(f.key, "_dict_mgr") == 0) {
                g_ime.ensureUserDictLoaded();
                g_settingsState.mode = SETTINGS_DICT_CHOOSE;
                g_settingsState.dictSelection = 0;
                g_settingsState.dictScroll = 0;
                g_settingsState.dictSelected.clear();
                g_settingsState.dictSearchBuffer.clear();
                drawDictChoose();
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_bt_manage") == 0) {
                ctx.nextState = APP_BT_MANAGE;
                return APP_BT_MANAGE;
            }
            if (strcmp(f.key, "_polish_prompt") == 0) {
                ctx.nextState = APP_POLISH_PROMPT;
                return APP_POLISH_PROMPT;
            }
            if (strcmp(f.key, "_font_size") == 0) {
                int curSize = g_settings.fontSize();
                int newSize = (curSize == 22) ? 18 : 22;
                g_settings.setString("font_size", std::to_string(newSize));
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_input_mode") == 0) {
                std::string next = (g_settings.inputMode() == "typewriter") ? "normal" : "typewriter";
                g_settings.setString("input_mode", next);
                if (next == "normal") typingClickRelease();  // 切回正常立即关喇叭
                if (g_settingsState.selection > fieldVisibleCount() - 1)
                    g_settingsState.selection = fieldVisibleCount() - 1;
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_click_volume") == 0) {
                static const int LV[] = {0, 20, 40, 60, 80, 100};
                const int n = (int)(sizeof(LV) / sizeof(LV[0]));
                int cur = g_settings.typingClickVolume(), idx = 0;
                for (int i = 0; i < n; i++) if (LV[i] == cur) { idx = i; break; }
                g_settings.setString("click_volume", std::to_string(LV[(idx + 1) % n]));
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_click_chinese") == 0) {
                g_settings.setString("click_chinese",
                    clickChineseNext(clickChineseIndex(g_settings.clickChineseMode().c_str())));
                return APP_SETTINGS;
            }
            if (strcmp(f.key, "_click_timbre") == 0) {
                g_settings.setString("click_timbre",
                    timbreNext(timbreIndex(g_settings.typingClickTimbre().c_str())));
                return APP_SETTINGS;
            }
        } else if (isToggleField(f.key)) {
            // 开/关切换:存 "1"(开) 或 "0"(关)
            g_settings.setString(f.key, toggleValue(f.key) ? "0" : "1");
        } else {
            g_settingsState.editBuffer = g_settings.getString(f.key);
            g_settingsState.editCursor = (int)g_settingsState.editBuffer.length();
            g_settingsState.editing = true;
            g_settingsState.imeActive = false;
        }
    }

    ui_clear(); int y = FONT_H;
    ui_draw_text_centered(y, "设置", false, true); y += FONT_H;
    int visible = (SCREEN_H - y + FONT_H - 1) / FONT_H;
    if (g_settingsState.selection < g_settingsState.scroll) g_settingsState.scroll = g_settingsState.selection;
    if (g_settingsState.selection >= g_settingsState.scroll + visible)
        g_settingsState.scroll = g_settingsState.selection - visible + 1;

    int rowCount = fieldVisibleCount();
    for (int i = 0; i < visible && (g_settingsState.scroll + i) < rowCount; i++) {
        bool sel = (g_settingsState.scroll + i == g_settingsState.selection);
        int idx = fieldAt(g_settingsState.scroll + i); auto &f = SETTINGS_FIELDS[idx];
        char buf[80];
        if (f.action) {
            if (strcmp(f.key, "_font_size") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %dpt", f.label, g_settings.fontSize());
            } else if (strcmp(f.key, "_polish_prompt") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.polishPrompt().empty() ? "(未设置)" : "(已设置)");
            } else if (strcmp(f.key, "_app_mode") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.appMode() == "quick" ? "快捷编辑" : "个人日记");
            } else if (strcmp(f.key, "_home_view") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.homeView() == "month" ? "月视图" : "周视图");
            } else if (strcmp(f.key, "_editor_orientation") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.editorOrientation() == "vertical" ? "竖排" : "横排");
            } else if (strcmp(f.key, "_input_mode") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         g_settings.inputMode() == "typewriter" ? "打字机模式" : "正常模式");
            } else if (strcmp(f.key, "_click_chinese") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         CLICK_CHINESE_OPTS[clickChineseIndex(g_settings.clickChineseMode().c_str())].label);
            } else if (strcmp(f.key, "_click_volume") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %d%%", f.label, g_settings.typingClickVolume());
            } else if (strcmp(f.key, "_click_timbre") == 0) {
                snprintf(buf, sizeof(buf), "▶ %s: %s", f.label,
                         TIMBRE_OPTS[timbreIndex(g_settings.typingClickTimbre().c_str())].label);
            } else {
                snprintf(buf, sizeof(buf), "▶ %s", f.label);
            }
        } else if (isToggleField(f.key)) {
            snprintf(buf, sizeof(buf), "%s:%s", f.label, toggleValue(f.key) ? "开" : "关");
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
