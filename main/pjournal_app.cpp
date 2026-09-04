#include "pjournal_app.h"
#include "clipboard.h"
#include "font_renderer.h"
#include "journal_storage.h"
#include "builtin_prompts.h"
#include "settings_manager.h"
#include "markdown_render.h"
#include "main_menu_icons.h"
#include "screen_editor.h"
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern u8g2_t *g_u8g2;

std::string g_clipboard;  // global clipboard definition
std::string g_flomoPendingText;
AppState g_flomoReturnTo = APP_EDITOR;

extern "C" {
    extern void u8g2_SetDrawColor(void *g_u8g2, int color);
    extern void u8g2_DrawBox(void *g_u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *g_u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *g_u8g2, int x, int y, int w, int h);
    extern void u8g2_SetBitmapMode(void *g_u8g2, uint8_t is_transparent);
    extern void u8g2_DrawBitmap(void *g_u8g2, int x, int y, int cnt, int h, const uint8_t *bitmap);
    extern void u8g2_SendBuffer(void *g_u8g2);
}

// ── Screen state ─────────────────────────────────────────────────────────
static struct {
    int selection = 0; int scroll = 0;
    std::vector<JournalEntry> entries;   // cached listing, avoid full SD rescan each frame
} g_browser;
static struct { std::vector<std::string> lines; int scroll = 0; std::string filename;
    std::string dateStr;
    std::vector<VRow> cachedVrows; bool vrowsDirty = true;
    bool vrowsCachedFirstLineIndent = false;
    std::vector<MdLineInfo> cachedMdInfo; bool mdInfoDirty = true; bool mdCachedOn = false;
} g_viewer;
static struct {
    std::string filename;
    AppState returnTo = APP_BROWSER;
    std::vector<JournalHistoryVersion> versions;
    int selection = 0;
    int scroll = 0;
    bool preview = false;
    bool confirmRestore = false;
    bool confirmDelete = false;
    std::vector<std::string> lines;
    int previewScroll = 0;
    std::vector<VRow> cachedVrows;
    bool vrowsDirty = true;
    bool vrowsCachedFirstLineIndent = false;
    std::vector<MdLineInfo> cachedMdInfo;
    bool mdInfoDirty = true;
    bool mdCachedOn = false;
} g_history;

static const std::vector<VRow>& getViewerVrows() {
    bool firstLineIndent = g_settings.firstLineIndent();
    if (g_viewer.vrowsDirty || g_viewer.vrowsCachedFirstLineIndent != firstLineIndent) {
        g_viewer.vrowsCachedFirstLineIndent = firstLineIndent;
        g_viewer.cachedVrows = buildVrows(g_viewer.lines);
        g_viewer.vrowsDirty = false;
    }
    return g_viewer.cachedVrows;
}

// Markdown classification is only re-run when content changes or the render
// toggle flips, mirroring the editor's md cache.
static const std::vector<MdLineInfo>& getViewerMdInfo(bool mdOn) {
    if (g_viewer.mdInfoDirty || g_viewer.mdCachedOn != mdOn) {
        if (mdOn) g_viewer.cachedMdInfo = mdClassifyLines(g_viewer.lines);
        else g_viewer.cachedMdInfo.assign(g_viewer.lines.size(), MdLineInfo{});
        g_viewer.mdInfoDirty = false;
        g_viewer.mdCachedOn = mdOn;
    }
    return g_viewer.cachedMdInfo;
}

static void refreshBrowserCache() { g_browser.entries = g_journal.listEntries(); }

static const std::vector<VRow>& getHistoryVrows() {
    bool firstLineIndent = g_settings.firstLineIndent();
    if (g_history.vrowsDirty || g_history.vrowsCachedFirstLineIndent != firstLineIndent) {
        g_history.vrowsCachedFirstLineIndent = firstLineIndent;
        g_history.cachedVrows = buildVrows(g_history.lines);
        g_history.vrowsDirty = false;
    }
    return g_history.cachedVrows;
}

static const std::vector<MdLineInfo>& getHistoryMdInfo(bool mdOn) {
    if (g_history.mdInfoDirty || g_history.mdCachedOn != mdOn) {
        if (mdOn) g_history.cachedMdInfo = mdClassifyLines(g_history.lines);
        else g_history.cachedMdInfo.assign(g_history.lines.size(), MdLineInfo{});
        g_history.mdInfoDirty = false;
        g_history.mdCachedOn = mdOn;
    }
    return g_history.cachedMdInfo;
}

static void loadHistoryPreviewText(const std::string &content) {
    g_history.lines.clear();
    size_t pos = 0;
    while (pos < content.length()) {
        size_t nl = content.find('\n', pos);
        g_history.lines.push_back((nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (g_history.lines.empty()) g_history.lines.push_back("");
    g_history.previewScroll = 0;
    g_history.vrowsDirty = true;
    g_history.mdInfoDirty = true;
}

static std::string historyTimeLabel(const std::string &fn) {
    if (fn.size() >= 17) return fn.substr(0, 10) + " " + fn.substr(11, 2) + ":" + fn.substr(13, 2);
    return fn;
}

// ── Main Screen ────────────────────────────────────────────────────────
static struct {
    int selection = 0;
} g_mainMenu;

struct MainMenuAction {
    char key;
    const char *name;
    const MainMenuIconGlyph *icon;
};

static const MainMenuAction kMainActions[] = {
    {'p', "提示写作", &MAIN_ICON_PROMPT},
    {'f', "自由写作", &MAIN_ICON_FREE},
    {'v', "查看过往日记", &MAIN_ICON_VIEW},
    {'w', "同步WebDAV", &MAIN_ICON_SYNC},
    {'t', "GTD任务管理", &MAIN_ICON_GTD},
    {'o', "大纲写作", &MAIN_ICON_OUTLINE},
    {'s', "设置", &MAIN_ICON_SETTINGS},
};

static int mainMenuVisibleCount(bool hasEntries) {
    return hasEntries ? 7 : 6;
}

static int mainMenuActionIndex(int visibleIndex, bool hasEntries) {
    if (hasEntries || visibleIndex < 2) return visibleIndex;
    return visibleIndex + 1;
}

static int mainMenuVisibleIndexForKey(char key, bool hasEntries) {
    for (int i = 0; i < mainMenuVisibleCount(hasEntries); i++) {
        int actionIndex = mainMenuActionIndex(i, hasEntries);
        if (kMainActions[actionIndex].key == key) return i;
    }
    return -1;
}

static void mainMenuActivate(const MainMenuAction &action, ScreenContext &ctx) {
    switch (action.key) {
    case 'p':
        ctx.promptMode = true;
        ctx.promptText = BUILTIN_PROMPTS[rand() % BUILTIN_PROMPT_COUNT];
        ctx.prevState = APP_MAIN;
        ctx.nextState = APP_EDITOR;
        break;
    case 'f':
        ctx.promptMode = false;
        ctx.promptText = "";
        ctx.prevState = APP_MAIN;
        ctx.nextState = APP_EDITOR;
        break;
    case 'v':
        ctx.nextState = APP_BROWSER;
        break;
    case 'w':
        ctx.nextState = APP_SYNC_WEBDAV;
        break;
    case 't':
        ctx.nextState = APP_GTD;
        break;
    case 'o':
        ctx.nextState = APP_OUTLINE;
        break;
    case 's':
        ctx.nextState = APP_SETTINGS;
        break;
    default:
        break;
    }
}

static void drawMainMenuIcon(int centerX, int topY, const MainMenuIconGlyph *icon, bool selected) {
    if (!g_u8g2 || !icon) return;
    const int boxSize = 48;
    int boxX = centerX - boxSize / 2;
    int glyphX = centerX - icon->width / 2;
    int glyphY = topY + (boxSize - icon->height) / 2;

    u8g2_SetBitmapMode(g_u8g2, 1);
    if (selected) {
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, boxX, topY, boxSize, boxSize);
        u8g2_SetDrawColor(g_u8g2, 1);
        u8g2_DrawBitmap(g_u8g2, glyphX, glyphY, icon->row_bytes, icon->height, icon->bitmap);
        u8g2_SetDrawColor(g_u8g2, 0);
    } else {
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBitmap(g_u8g2, glyphX, glyphY, icon->row_bytes, icon->height, icon->bitmap);
    }
}

void screen_main_init() {
    static bool seeded = false;
    if (!seeded) {
        srand(esp_random());
        seeded = true;
    }
}

// 月视图主页:固定22pt绘制"当前年月 + 同宽短分割线 + 星期表头 + 当月日历网格",
// 标题顶到屏幕上沿不留白。单元格 = 日期 + 标记(✓已写/·未写/未来无标记),全部
// 白底黑字不反白,今日仅用细边框圈出高亮;不显示连续/总计/今日统计。行距压到
// lh+1、分割线跟随末行,给底部图标+22pt动作标签留空间。绘制完恢复用户字号。
static int drawMonthCalendar(time_t now_t, const struct tm *tmNow) {
    int prevSize = g_font.fontSize();
    if (prevSize != 22) g_font.setSize(22);
    // localtime() 返回进程级静态 tm:下方循环里的 localtime(&d) 会覆盖同一块
    // 内存令 tmNow 失效(isToday 恒真,每格都被当成今日)——入口先拷贝。
    const struct tm base = *tmNow;
    const int todayMday = base.tm_mday;
    const int mrowH = g_font.lineHeight() + 1;  // 22pt → 23px
    int y = g_font.ascent();         // 首行:当前年月,顶到屏幕上沿
    char ym[40];
    snprintf(ym, sizeof(ym), "%d年%d月", base.tm_year + 1900, base.tm_mon + 1);
    ui_draw_text_centered(y, ym);
    // 标题下的短分割线,宽度约等于标题文字;表头与日历随之下移让位
    int ymW = g_font.textWidth(ym);
    int titleDivY = y + g_font.descent() + 3;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, (SCREEN_W - ymW) / 2, titleDivY, ymW);
    y = titleDivY + 5 + g_font.ascent();
    const char *dnames[7] = {"一","二","三","四","五","六","日"};
    const int colWidth = SCREEN_W / 7;
    const int colStartX = (SCREEN_W - colWidth * 7) / 2;
    for (int i = 0; i < 7; i++) {
        int cx = colStartX + i * colWidth + (colWidth - g_font.textWidth(dnames[i])) / 2;
        g_font.drawText(cx, y, dnames[i], false);
    }
    y += mrowH;
    struct tm first = base;
    first.tm_mday = 1; first.tm_hour = 12; first.tm_min = 0; first.tm_sec = 0;
    time_t first_t = mktime(&first);
    struct tm *ftm = localtime(&first_t);
    int lead = (ftm->tm_wday == 0) ? 6 : ftm->tm_wday - 1;  // 周一为首列
    struct tm nxt = first;
    if (++nxt.tm_mon > 11) { nxt.tm_mon = 0; nxt.tm_year++; }
    int dim = (int)((mktime(&nxt) - first_t) / 86400);
    int rows = (lead + dim + 6) / 7;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < 7; c++) {
            int day = r * 7 + c - lead + 1;
            if (day < 1 || day > dim) continue;
            time_t d = first_t + (day - 1) * 86400;
            struct tm *dtm = localtime(&d);
            char ds[16]; strftime(ds, sizeof(ds), "%Y-%m-%d", dtm);
            bool has = g_journal.hasEntry(ds);
            bool isToday = (day == todayMday);
            // d 取当日正午,减 12h 得当日零点,保证今天上午也带 · 标记
            char cell[24];
            if (has) snprintf(cell, sizeof(cell), "%d✓", day);
            else if (d - 43200 <= now_t) snprintf(cell, sizeof(cell), "%d·", day);
            else snprintf(cell, sizeof(cell), "%d", day);
            int cx = colStartX + c * colWidth + (colWidth - g_font.textWidth(cell)) / 2;
            g_font.drawText(cx, y, cell, false);
            if (isToday) {
                // 今日高亮:细边框紧贴当日"日期+标记"字形,不波及相邻行
                u8g2_SetDrawColor(g_u8g2, 0);
                u8g2_DrawFrame(g_u8g2, cx - 3, y - g_font.ascent(),
                               g_font.textWidth(cell) + 6, g_font.ascent() + 2);
            }
        }
        y += mrowH;
    }
    int dividerY = y - mrowH + g_font.descent() + 6;
    if (prevSize != 22) g_font.setSize(prevSize);
    return dividerY;
}

AppState screen_main_handle(int key, ScreenContext &ctx) {
    ui_clear(); int y = FONT_H;
    const int rowH = LINE_SPACING;
    bool hasEntries = g_journal.totalEntries() > 0;
    int actionCount = mainMenuVisibleCount(hasEntries);
    if (g_mainMenu.selection >= actionCount) g_mainMenu.selection = actionCount - 1;
    if (g_mainMenu.selection < 0) g_mainMenu.selection = 0;

    if (key == KEY_LEFT || key == KEY_UP || key == 'h' || key == 'k') {
        g_mainMenu.selection = (g_mainMenu.selection + actionCount - 1) % actionCount;
        key = 0;
    } else if (key == KEY_RIGHT || key == KEY_DOWN || key == 'l' || key == 'j') {
        g_mainMenu.selection = (g_mainMenu.selection + 1) % actionCount;
        key = 0;
    } else {
        char lowerKey = (key >= 'A' && key <= 'Z') ? (char)(key + ('a' - 'A')) : (char)key;
        int idx = mainMenuVisibleIndexForKey(lowerKey, hasEntries);
        if (idx >= 0) g_mainMenu.selection = idx;
    }

    time_t now_t; time(&now_t); struct tm *tm = localtime(&now_t);
    char dateStr[32]; strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", tm);
    bool monthView = g_settings.homeView() == "month";
    int dividerY;
    if (monthView) {
        dividerY = drawMonthCalendar(now_t, tm);
    } else {
        y += rowH;
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
            g_font.drawText(mx, y + rowH, mark, false);
        }
        y += rowH * 2;

        char buf[48]; snprintf(buf, sizeof(buf), "连续:%d天 总计:%d篇", g_journal.getStreak(), g_journal.totalEntries());
        ui_draw_text_centered(y, buf); y += rowH;
        int tc = g_journal.countToday();
        int todayBaseline = y;
        if (tc > 0) { snprintf(buf, sizeof(buf), "✓ 今日已写%d篇", tc); ui_draw_text_centered(y, buf, false, true); }
        else ui_draw_text_centered(y, "今日尚未写日记");
        dividerY = todayBaseline + g_font.descent() + 14;
    }

    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 24, dividerY, SCREEN_W - 48);

    int slotW = SCREEN_W / actionCount;
    // 月视图:底部要容纳 图标+22pt动作标签,图标距分割线更近
    int iconTop = dividerY + (monthView ? 4 : 14);
    for (int i = 0; i < actionCount; i++) {
        const MainMenuAction &action = kMainActions[mainMenuActionIndex(i, hasEntries)];
        int cx = i * slotW + slotW / 2;
        drawMainMenuIcon(cx, iconTop, action.icon, i == g_mainMenu.selection);
    }
    const MainMenuAction &selected = kMainActions[mainMenuActionIndex(g_mainMenu.selection, hasEntries)];
    if (!monthView) {
        y = iconTop + 48 + rowH;
        char selectedText[48];
        snprintf(selectedText, sizeof(selectedText), "[%c] %s", selected.key, selected.name);
        ui_draw_text_centered(y, selectedText, false, true);
        y += rowH;
    } else {
        // 月视图动作标签固定 22pt(与日历一致);6 行日历等放不下时退 16pt
        int statusTop = SCREEN_H - g_font.lineHeight() - 2;  // 用户字号下的状态栏上沿
        int prevSize = g_font.fontSize();
        g_font.setSize(22);
        int labelY = iconTop + 48 + 2 + g_font.ascent();
        if (labelY + g_font.descent() > statusTop - 2) {
            g_font.setSize(16);
            labelY = iconTop + 48 + 2 + g_font.ascent();
            if (labelY + g_font.descent() > statusTop - 2) labelY = -1;
        }
        if (labelY > 0) {
            char selectedText[48];
            snprintf(selectedText, sizeof(selectedText), "[%c] %s", selected.key, selected.name);
            ui_draw_text_centered(labelY, selectedText, false, true);
        }
        g_font.setSize(prevSize);
    }

    std::string batteryGroup = battery_status_text();
    g_font.drawText(4, STATUS_Y + g_font.ascent(), dateStr, false);
    if (!batteryGroup.empty()) {
        int pw = g_font.textWidth(batteryGroup.c_str());
        g_font.drawText(SCREEN_W - pw - 4, STATUS_Y + g_font.ascent(), batteryGroup.c_str(), false);
    }
    ui_commit();

    char lowerKey = (key >= 'A' && key <= 'Z') ? (char)(key + ('a' - 'A')) : (char)key;
    int directIdx = mainMenuVisibleIndexForKey(lowerKey, hasEntries);
    if (directIdx >= 0) {
        mainMenuActivate(kMainActions[mainMenuActionIndex(directIdx, hasEntries)], ctx);
    } else if (key == 0x0A || key == 0x0D) {
        mainMenuActivate(selected, ctx);
    } else if (key=='q'||key=='Q') ctx.nextState=APP_QUIT;
    return ctx.nextState;
}

// ── Browser Screen ─────────────────────────────────────────────────────
void screen_browser_init() {
    g_browser.selection = g_browser.scroll = 0;
    refreshBrowserCache();
}

AppState screen_browser_handle(int key, ScreenContext &ctx) {
    auto &entries = g_browser.entries;
    if (entries.empty()) { ctx.nextState = APP_MAIN; return APP_MAIN; }
    if (g_browser.selection >= (int)entries.size()) g_browser.selection = (int)entries.size() - 1;

    if (key == 'q' || key == 'Q' || key == 0x1B) { ctx.nextState = APP_MAIN; return APP_MAIN; }
    if (key == 'j' || key == KEY_DOWN) { g_browser.selection++; if (g_browser.selection>=(int)entries.size()) g_browser.selection=(int)entries.size()-1; }
    if (key == 'k' || key == KEY_UP) { g_browser.selection--; if (g_browser.selection<0) g_browser.selection=0; }
    if (key == 0x0A || key == 0x0D) { ctx.selectedEntry = entries[g_browser.selection].filename; ctx.nextState = APP_VIEWER; return APP_VIEWER; }
    if (key == 'h' || key == 'H') {
        ctx.selectedEntry = entries[g_browser.selection].filename;
        ctx.prevState = APP_BROWSER;
        ctx.nextState = APP_HISTORY;
        return APP_HISTORY;
    }
    if (key == 'd' || key == 'D') {
        g_journal.deleteEntry(entries[g_browser.selection].filename);
        refreshBrowserCache();
        if (entries.empty()) { ctx.nextState=APP_MAIN; return APP_MAIN; }
        if (g_browser.selection>=(int)entries.size()) g_browser.selection=(int)entries.size()-1;
    }
    if (key == 'e' || key == 'E') {
        ctx.prevState = APP_BROWSER;
        std::string content = g_journal.readEntry(entries[g_browser.selection].filename);
        if (!content.empty()) {
            ctx.editContent = extractBody(content);
            ctx.editFilename = entries[g_browser.selection].filename;
            ctx.promptMode = false;
            ctx.promptText = "";
            ctx.nextState = APP_EDITOR;
            return APP_EDITOR;
        }
    }
    if (key == 0x13) {
        auto content = g_journal.readEntry(entries[g_browser.selection].filename);
        if (!content.empty()) {
            auto body = extractBody(content);
            if (!body.empty()) {
                g_flomoPendingText = body;
                g_flomoReturnTo = APP_BROWSER;
                ctx.nextState = APP_SYNC_SEND_FLOMO;
                return APP_SYNC_SEND_FLOMO;
            }
        }
    }

    ui_clear(); int y = FONT_H;
    ui_draw_text(4, y, "过往日记", false, true);
    u8g2_DrawHLine(g_u8g2, 0, y + 7, SCREEN_W);
    y = y + 7 + LINE_SPACING - 4;
    int visible = (SCREEN_H - y + LINE_SPACING - 1) / LINE_SPACING;
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
        ui_draw_text(8, y + i * LINE_SPACING, buf, sel);
    }
    ui_commit();
    return APP_BROWSER;
}

// ── Viewer Screen ──────────────────────────────────────────────────────
void screen_viewer_init(const std::string &filename) {
    g_viewer.filename = filename; g_viewer.scroll = 0; g_viewer.lines.clear();
    g_viewer.vrowsDirty = true;
    g_viewer.mdInfoDirty = true;
    if (filename.length() >= 15)
        g_viewer.dateStr = filename.substr(0,10) + " " + filename.substr(11,2) + ":" + filename.substr(13,2);
    else g_viewer.dateStr = filename;
    std::string content = g_journal.readEntry(filename);
    if (content.empty()) return;
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
    if (key == 'e' || key == 'E') {
        ctx.prevState = APP_VIEWER;
        ctx.selectedEntry = g_viewer.filename;
        std::string content = g_journal.readEntry(g_viewer.filename);
        if (!content.empty()) {
            ctx.editContent = extractBody(content);
            ctx.editFilename = g_viewer.filename;
            ctx.promptMode = false;
            ctx.promptText = "";
            ctx.nextState = APP_EDITOR;
            return APP_EDITOR;
        }
    }
    if (key == 'h' || key == 'H') {
        ctx.selectedEntry = g_viewer.filename;
        ctx.prevState = APP_VIEWER;
        ctx.nextState = APP_HISTORY;
        return APP_HISTORY;
    }
    if (key == 'f' || key == 'F') {
        std::string content = g_journal.readEntry(g_viewer.filename);
        if (!content.empty()) {
            auto body = extractBody(content);
            if (!body.empty()) {
                g_flomoPendingText = body;
                g_flomoReturnTo = APP_VIEWER;
                ctx.nextState = APP_SYNC_SEND_FLOMO;
                return APP_SYNC_SEND_FLOMO;
            }
        }
    }

    const auto& vrows = getViewerVrows();

    const int headerY = FONT_H;
    const int sepY = headerY + g_font.descent();
    const int contentY = sepY + 26;
    const int contentMaxY = STATUS_Y;
    int visible = (contentMaxY - contentY + LINE_SPACING - 1) / LINE_SPACING;
    if (visible < 1) visible = 1;
    int maxScroll = (int)vrows.size() - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (g_viewer.scroll > maxScroll) g_viewer.scroll = maxScroll;

    ui_clear();

    std::string header = g_viewer.dateStr;
    if (header.empty()) header = g_viewer.filename;
    ui_draw_text(4, headerY, header.c_str(), true);

    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 4, sepY, SCREEN_W - 8);

    bool mdOn = g_settings.markdownRender();
    mdSetRenderEnabled(mdOn);
    const auto& mdInfo = getViewerMdInfo(mdOn);
    for (int i = 0; i < visible && (g_viewer.scroll + i) < (int)vrows.size(); i++) {
        auto &vr = vrows[g_viewer.scroll + i];
        mdDrawVrow(4, contentY + i * LINE_SPACING, g_viewer.lines[vr.lineIdx], vr.start, vr.end, mdInfo[vr.lineIdx], vr.indentCells);
    }

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

// ── History Screen ─────────────────────────────────────────────────────
void screen_history_init(const std::string &filename, AppState returnTo) {
    g_history.filename = filename;
    g_history.returnTo = returnTo;
    g_history.versions = g_journal.listHistoryVersions(filename);
    g_history.selection = 0;
    g_history.scroll = 0;
    g_history.preview = false;
    g_history.confirmRestore = false;
    g_history.confirmDelete = false;
    g_history.lines.clear();
    g_history.previewScroll = 0;
    g_history.vrowsDirty = true;
    g_history.mdInfoDirty = true;
}

static AppState historyReturn(ScreenContext &ctx) {
    ctx.selectedEntry = g_history.filename;
    ctx.nextState = g_history.returnTo;
    return g_history.returnTo;
}

static void drawHistoryConfirm(const char *title, const char *action) {
    int bw = 300, bh = 120;
    int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2 - 20;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, bx, by, bw);
    u8g2_DrawHLine(g_u8g2, bx, by + bh - 1, bw);
    u8g2_DrawBox(g_u8g2, bx, by, 1, bh);
    u8g2_DrawBox(g_u8g2, bx + bw - 1, by, 1, bh);
    ui_draw_text_centered(by + 28, title);
    ui_draw_text_centered(by + 58, action);
    ui_draw_text_centered(by + 88, "ESC=取消");
    u8g2_SetDrawColor(g_u8g2, 1);
}

static void drawHistoryList() {
    ui_clear();
    const int rowH = LINE_SPACING;
    ui_draw_text(4, FONT_H, "历史版本", false, true);
    std::string count = std::to_string((int)g_history.versions.size());
    ui_draw_text(SCREEN_W - 4 - g_font.textWidth(count.c_str()), FONT_H, count.c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 4, FONT_H + g_font.descent(), SCREEN_W - 8);

    int y = FONT_H + rowH;
    int visible = (STATUS_Y - y + rowH - 1) / rowH;
    if (visible < 1) visible = 1;
    int total = (int)g_history.versions.size();
    if (g_history.selection >= total) g_history.selection = total - 1;
    if (g_history.selection < 0) g_history.selection = 0;
    if (g_history.selection < g_history.scroll) g_history.scroll = g_history.selection;
    if (g_history.selection >= g_history.scroll + visible)
        g_history.scroll = g_history.selection - visible + 1;

    if (total == 0) {
        ui_draw_text_centered(SCREEN_H / 2, "暂无历史版本");
    } else {
        for (int i = 0; i < visible && (g_history.scroll + i) < total; i++) {
            int idx = g_history.scroll + i;
            const auto &v = g_history.versions[idx];
            char buf[80];
            snprintf(buf, sizeof(buf), "%s %uB", historyTimeLabel(v.filename).c_str(), (unsigned)v.size);
            ui_draw_text(8, y + i * rowH, buf, idx == g_history.selection);
        }
    }

    ui_draw_status("Enter查看 r恢复 d删除 q返回", "");
    ui_commit();
}

static void drawHistoryPreview() {
    const auto& vrows = getHistoryVrows();
    const int headerY = FONT_H;
    const int sepY = headerY + g_font.descent();
    const int contentY = sepY + 26;
    const int contentMaxY = STATUS_Y;
    int visible = (contentMaxY - contentY + LINE_SPACING - 1) / LINE_SPACING;
    if (visible < 1) visible = 1;
    int maxScroll = (int)vrows.size() - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (g_history.previewScroll > maxScroll) g_history.previewScroll = maxScroll;

    ui_clear();
    std::string header = "历史";
    if (!g_history.versions.empty()) header = historyTimeLabel(g_history.versions[g_history.selection].filename);
    ui_draw_text(4, headerY, header.c_str(), true);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 4, sepY, SCREEN_W - 8);

    bool mdOn = g_settings.markdownRender();
    mdSetRenderEnabled(mdOn);
    const auto& mdInfo = getHistoryMdInfo(mdOn);
    for (int i = 0; i < visible && (g_history.previewScroll + i) < (int)vrows.size(); i++) {
        auto &vr = vrows[g_history.previewScroll + i];
        mdDrawVrow(4, contentY + i * LINE_SPACING, g_history.lines[vr.lineIdx], vr.start, vr.end, mdInfo[vr.lineIdx], vr.indentCells);
    }
    ui_draw_status("r恢复 d删除 q返回", "");
    ui_commit();
}

AppState screen_history_handle(int key, ScreenContext &ctx) {
    if (g_history.confirmRestore) {
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            if (!g_history.versions.empty()) {
                ui_clear(); ui_show_message_centered("正在恢复..."); ui_commit();
                std::string hist = g_history.versions[g_history.selection].filename;
                bool ok = g_journal.restoreHistoryVersion(g_history.filename, hist);
                if (ok) {
                    std::string content = g_journal.readEntry(g_history.filename);
                    if (g_history.returnTo == APP_EDITOR) {
                        ctx.editFilename = g_history.filename;
                        ctx.editContent = extractBody(content);
                        ctx.promptMode = false;
                        ctx.promptText = "";
                        app_editor_request_reinit();
                    }
                    ctx.statusMessage = "已恢复历史版本";
                    g_history.confirmRestore = false;
                    return historyReturn(ctx);
                }
                ctx.statusMessage = "恢复失败";
            }
            g_history.confirmRestore = false;
            return APP_HISTORY;
        }
        if (key == 0x1B || key == 'q' || key == 'Q' || key == 'n' || key == 'N') {
            g_history.confirmRestore = false;
            if (g_history.preview) drawHistoryPreview(); else drawHistoryList();
            return APP_HISTORY;
        }
        if (g_history.preview) drawHistoryPreview(); else drawHistoryList();
        drawHistoryConfirm("恢复此历史版本？", "Enter=恢复");
        ui_commit();
        return APP_HISTORY;
    }

    if (g_history.confirmDelete) {
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            if (!g_history.versions.empty()) {
                std::string hist = g_history.versions[g_history.selection].filename;
                bool ok = g_journal.deleteHistoryVersion(g_history.filename, hist);
                g_history.versions = g_journal.listHistoryVersions(g_history.filename);
                if (g_history.selection >= (int)g_history.versions.size())
                    g_history.selection = (int)g_history.versions.size() - 1;
                if (g_history.selection < 0) g_history.selection = 0;
                g_history.preview = false;
                ctx.statusMessage = ok ? "已删除历史版本" : "删除失败";
            }
            g_history.confirmDelete = false;
            drawHistoryList();
            return APP_HISTORY;
        }
        if (key == 0x1B || key == 'q' || key == 'Q' || key == 'n' || key == 'N') {
            g_history.confirmDelete = false;
            if (g_history.preview) drawHistoryPreview(); else drawHistoryList();
            return APP_HISTORY;
        }
        if (g_history.preview) drawHistoryPreview(); else drawHistoryList();
        drawHistoryConfirm("删除此历史版本？", "Enter=删除");
        ui_commit();
        return APP_HISTORY;
    }

    if (g_history.preview) {
        const auto& vrows = getHistoryVrows();
        int visible = (STATUS_Y - (FONT_H + g_font.descent() + 26) + LINE_SPACING - 1) / LINE_SPACING;
        if (visible < 1) visible = 1;
        int maxScroll = (int)vrows.size() - visible;
        if (maxScroll < 0) maxScroll = 0;
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            g_history.preview = false;
            drawHistoryList();
            return APP_HISTORY;
        }
        if (key == 'j' || key == KEY_DOWN) { if (g_history.previewScroll < maxScroll) g_history.previewScroll++; }
        if (key == 'k' || key == KEY_UP) { if (g_history.previewScroll > 0) g_history.previewScroll--; }
        if (key == KEY_PAGE_DOWN) { g_history.previewScroll += visible; if (g_history.previewScroll > maxScroll) g_history.previewScroll = maxScroll; }
        if (key == KEY_PAGE_UP) { g_history.previewScroll -= visible; if (g_history.previewScroll < 0) g_history.previewScroll = 0; }
        if (key == 'r' || key == 'R') g_history.confirmRestore = true;
        if (key == 'd' || key == 'D') g_history.confirmDelete = true;
        drawHistoryPreview();
        if (g_history.confirmRestore) drawHistoryConfirm("恢复此历史版本？", "Enter=恢复");
        if (g_history.confirmDelete) drawHistoryConfirm("删除此历史版本？", "Enter=删除");
        ui_commit();
        return APP_HISTORY;
    }

    int total = (int)g_history.versions.size();
    if (key == 'q' || key == 'Q' || key == 0x1B) return historyReturn(ctx);
    if (key == 'j' || key == KEY_DOWN) { if (g_history.selection < total - 1) g_history.selection++; }
    if (key == 'k' || key == KEY_UP) { if (g_history.selection > 0) g_history.selection--; }
    if ((key == 0x0A || key == 0x0D) && total > 0) {
        ui_clear(); ui_show_message_centered("正在读取..."); ui_commit();
        std::string content = g_journal.readHistoryVersion(g_history.filename, g_history.versions[g_history.selection].filename);
        loadHistoryPreviewText(content);
        g_history.preview = true;
        drawHistoryPreview();
        return APP_HISTORY;
    }
    if ((key == 'r' || key == 'R') && total > 0) g_history.confirmRestore = true;
    if ((key == 'd' || key == 'D') && total > 0) g_history.confirmDelete = true;
    drawHistoryList();
    if (g_history.confirmRestore) drawHistoryConfirm("恢复此历史版本？", "Enter=恢复");
    if (g_history.confirmDelete) drawHistoryConfirm("删除此历史版本？", "Enter=删除");
    ui_commit();
    return APP_HISTORY;
}
