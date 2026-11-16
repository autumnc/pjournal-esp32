#include "pjournal_app.h"
#include "calendar_panel.h"
#include "clipboard.h"
#include "clip_history.h"
#include "font_renderer.h"
#include "journal_storage.h"
#include "builtin_prompts.h"
#include "settings_manager.h"
#include "markdown_render.h"
#include "main_menu_icons.h"
#include "screen_editor.h"
#include "vertical_layout.h"
#include "ime/IME.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern u8g2_t *g_u8g2;

std::string g_clipboard;  // global clipboard definition
bool g_clipboard_linewise = false;

void clipboardSet(const std::string &text, bool linewise, bool recordHistory) {
    g_clipboard = text;
    g_clipboard_linewise = linewise;
    if (recordHistory) cliphist_record(text, linewise);
}

std::string clipboardFlattenSingleLine(const std::string &text) {
    std::string out = text;
    for (char &c : out) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    return out;
}

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
    std::vector<int> view;               // indices into entries after date filter
    std::string filterDate;              // empty = all dates
    bool calOpen = false;
    CalendarPanelState cal;
    bool searchOpen = false;             // 正在输入搜索词
    std::string searchQuery;             // empty = no text filter
    bool imeActive = false;
    // Enter 触发的全文搜索结果（entries 下标，升序；用条目数/首尾文件名/内容修订号校验是否仍有效）
    std::vector<int> fullTextIdx;
    std::string fullTextQuery;
    std::string fullTextDate;
    size_t fullTextEntryCount = 0;
    std::string fullTextFirstFile;
    std::string fullTextLastFile;
    uint32_t fullTextRevision = 0;
    bool fullTextActive = false;
    // 全文增量扫描：每轮空闲节拍只处理一小批，扫描期间界面和按键仍可用
    bool scanActive = false;
    int scanNext = 0;
    int scanTotal = 0;
    int scanDone = 0;
    std::string scanQuery;
    std::string scanQueryLower;
    std::string scanDate;
    uint32_t scanRevision = 0;
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

static std::string asciiLowerCopy(const std::string &s) {
    std::string out = s;
    for (char &c : out)
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    return out;
}

// ASCII 大小写不敏感的子串匹配；中文按字节精确匹配。
// 直接在原串上逐字节比较，避免为全文搜索复制整篇日记。
static bool browserTextContains(const std::string &hay, const std::string &needleLower) {
    size_t n = needleLower.size();
    if (n == 0) return true;
    if (hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); i++) {
        size_t j = 0;
        while (j < n) {
            char c = hay[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
            if (c != needleLower[j]) break;
            j++;
        }
        if (j == n) return true;
    }
    return false;
}

// 实时过滤只匹配日期/标题/首行预览（不读 SD）
static bool browserEntryMatchesQuery(const JournalEntry &e, const std::string &qLower) {
    return browserTextContains(e.date, qLower) ||
           browserTextContains(e.title, qLower) ||
           browserTextContains(e.preview, qLower);
}

// 搜索词上限：面板只能显示一行，过长查询既没有显示价值，也会把全文匹配拖成
// O(文件长度 × 查询长度)。按字节截断，输入侧直接拒绝超限的提交。
static constexpr size_t kMaxSearchBytes = 128;
// 每个空闲节拍读多少篇：太大则单拍卡顿，太小则长列表扫得太慢。
static constexpr int kFullTextBatch = 16;

static void invalidateFullTextSearch() {
    g_browser.fullTextActive = false;
    g_browser.fullTextIdx.clear();
    g_browser.fullTextQuery.clear();
    g_browser.fullTextDate.clear();
    g_browser.fullTextEntryCount = 0;
    g_browser.fullTextFirstFile.clear();
    g_browser.fullTextLastFile.clear();
    g_browser.fullTextRevision = 0;
}

// 中止增量扫描并丢弃未完成结果（保留已有筛选条件，由调用方决定重绘）。
static void cancelFullTextScan() {
    g_browser.scanActive = false;
    g_browser.scanNext = 0;
    g_browser.scanTotal = 0;
    g_browser.scanDone = 0;
    g_browser.scanQuery.clear();
    g_browser.scanQueryLower.clear();
    g_browser.scanDate.clear();
    g_browser.scanRevision = 0;
    g_browser.fullTextIdx.clear();
}

static void rebuildBrowserView();
static void stepFullTextScan();

// Enter 触发：开始逐篇分块读取正文补搜。扫描不在一次按键里跑完，而是每个
// 空闲节拍处理一小批，期间任何按键都会中止扫描并按正常按键处理，避免长列表
// 把主循环（按键/界面）整段堵死。
static void startFullTextScan() {
    cancelFullTextScan();
    invalidateFullTextSearch();
    if (g_browser.searchQuery.empty()) {
        rebuildBrowserView();
        return;
    }

    g_browser.scanQuery = g_browser.searchQuery;
    g_browser.scanQueryLower = asciiLowerCopy(g_browser.searchQuery);
    g_browser.scanDate = g_browser.filterDate;
    // 记录扫描起点的修订号：扫描期间若有别的任务改写了日记，结果直接作废，
    // 不能让"扫完时再取修订号"把中途改动洗成有效结果。
    g_browser.scanRevision = g_journal.contentRevision();
    for (const auto &e : g_browser.entries)
        if (g_browser.scanDate.empty() || e.date == g_browser.scanDate) g_browser.scanTotal++;
    g_browser.scanNext = 0;
    g_browser.scanDone = 0;
    g_browser.selection = g_browser.scroll = 0;
    g_browser.scanActive = true;
    stepFullTextScan();  // 先跑一批：短列表当场出结果，长列表立刻显示进度
}

static void stepFullTextScan() {
    if (!g_browser.scanActive) return;
    auto &entries = g_browser.entries;

    int processed = 0;
    while (g_browser.scanNext < (int)entries.size() && processed < kFullTextBatch) {
        int i = g_browser.scanNext++;
        const auto &e = entries[i];
        if (!g_browser.scanDate.empty() && e.date != g_browser.scanDate) continue;
        processed++;
        g_browser.scanDone++;

        bool matched = browserEntryMatchesQuery(e, g_browser.scanQueryLower);
        if (!matched) matched = g_journal.searchEntryContent(e.filename, g_browser.scanQueryLower);
        if (matched) g_browser.fullTextIdx.push_back(i);
    }
    if (g_browser.scanNext < (int)entries.size()) return;

    // 扫描完成：固定结果与指纹，条目列表未变时后续直接复用
    g_browser.scanActive = false;
    g_browser.fullTextActive = true;
    g_browser.fullTextQuery = g_browser.scanQuery;
    g_browser.fullTextDate = g_browser.scanDate;
    g_browser.fullTextEntryCount = entries.size();
    g_browser.fullTextRevision = g_browser.scanRevision;
    if (!entries.empty()) {
        g_browser.fullTextFirstFile = entries.front().filename;
        g_browser.fullTextLastFile = entries.back().filename;
    }
    g_browser.scanQuery.clear();
    g_browser.scanQueryLower.clear();
    g_browser.scanDate.clear();
    g_browser.selection = g_browser.scroll = 0;
    rebuildBrowserView();
}

// 按日期筛选与搜索词重建可见列表；不重复扫描 SD。
static void rebuildBrowserView() {
    g_browser.view.clear();
    const auto &entries = g_browser.entries;
    std::string q = asciiLowerCopy(g_browser.searchQuery);
    // 全文结果只在条目列表未变化时沿用，避免外部增删/改写后下标或正文错位
    bool useFullText = g_browser.fullTextActive &&
                       g_browser.fullTextQuery == g_browser.searchQuery &&
                       g_browser.fullTextDate == g_browser.filterDate &&
                       g_browser.fullTextEntryCount == entries.size() &&
                       g_browser.fullTextRevision == g_journal.contentRevision() &&
                       (entries.empty() ||
                        (g_browser.fullTextFirstFile == entries.front().filename &&
                         g_browser.fullTextLastFile == entries.back().filename));
    // 结果已过期就退回首行/标题实时过滤，底栏标签随之从"全文搜索"变回"搜索"；
    // 同时释放可能很大的下标表，避免一次大搜索长期占着 PSRAM。
    if (!useFullText) {
        g_browser.fullTextActive = false;
        std::vector<int>().swap(g_browser.fullTextIdx);  // 连容量一起还掉
    }
    for (int i = 0; i < (int)entries.size(); i++) {
        if (!g_browser.filterDate.empty() && entries[i].date != g_browser.filterDate)
            continue;
        if (useFullText) {
            if (!std::binary_search(g_browser.fullTextIdx.begin(), g_browser.fullTextIdx.end(), i))
                continue;
        } else if (!q.empty() && !browserEntryMatchesQuery(entries[i], q)) {
            continue;
        }
        g_browser.view.push_back(i);
    }
    if (g_browser.selection >= (int)g_browser.view.size())
        g_browser.selection = (int)g_browser.view.size() - 1;
    if (g_browser.selection < 0) g_browser.selection = 0;
    if (g_browser.scroll > g_browser.selection) g_browser.scroll = g_browser.selection;
}

static const JournalEntry *browserSelectedEntry() {
    if (g_browser.view.empty()) return nullptr;
    if (g_browser.selection < 0 || g_browser.selection >= (int)g_browser.view.size()) return nullptr;
    return &g_browser.entries[g_browser.view[g_browser.selection]];
}

// 删除最后一个 UTF-8 码点（避免退格截断多字节汉字）
static void popUtf8Codepoint(std::string &s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    s.erase(i);
}

static void refreshBrowserCache() {
    g_browser.entries = g_journal.listEntries();
    rebuildBrowserView();
}

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

static bool editorVertical() {
    return g_settings.editorOrientation() == "vertical";
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
    {'n', "Flomo笔记", &MAIN_ICON_FLOMO},
    {'s', "设置", &MAIN_ICON_SETTINGS},
    {'?', "使用说明", &MAIN_ICON_HELP},
};

#define MAIN_MENU_ICON_SLOT_W 56

static int mainMenuVisibleCount(bool hasEntries) {
    return hasEntries ? 9 : 8;
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
    case 'n':
        ctx.nextState = APP_FLOMO;
        break;
    case 's':
        ctx.nextState = APP_SETTINGS;
        break;
    case '?':
        ctx.nextState = APP_HELP;
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
// 标题上方留5px空白。单元格 = 日期 + 标记(✓已写/·未写/未来无标记),全部白底
// 黑字不反白,今日仅用细边框圈出高亮;不显示连续/总计/今日统计。行距压到 lh、
// 分割线跟随末行,给底部图标+22pt动作标签留空间。绘制完恢复用户字号。
static int drawMonthCalendar(time_t now_t, const struct tm *tmNow) {
    int prevSize = g_font.fontSize();
    if (prevSize != 22) g_font.setSize(22);
    // localtime() 返回进程级静态 tm:下方循环里的 localtime(&d) 会覆盖同一块
    // 内存令 tmNow 失效(isToday 恒真,每格都被当成今日)——入口先拷贝。
    const struct tm base = *tmNow;
    const int todayMday = base.tm_mday;
    const int mrowH = g_font.lineHeight();  // 22pt → 22px:行距收紧抵消标题下移,保证6行月份仍放得下22pt标签
    int y = g_font.ascent() + 5;     // 首行:当前年月,顶部留5px空白
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
    ctx.nextState = APP_MAIN;
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

    // 图标一行放不下就横向滚动：选中项尽量居中，首尾夹住。方向键/字母直达选中的
    // 那一项都会带动视口，所以只需要按当前 selection 算滚动量，不另存状态。
    const int slotW = MAIN_MENU_ICON_SLOT_W;
    const int totalW = actionCount * slotW;
    const int baseX = totalW < SCREEN_W ? (SCREEN_W - totalW) / 2 : 0;  // 放得下就居中
    int scrollX = g_mainMenu.selection * slotW + slotW / 2 - SCREEN_W / 2;
    if (scrollX > totalW - SCREEN_W) scrollX = totalW - SCREEN_W;
    if (scrollX < 0) scrollX = 0;
    const bool moreLeft = scrollX > 0;
    const bool moreRight = scrollX < totalW - SCREEN_W;

    // 月视图:底部要容纳 图标+22pt动作标签,图标距分割线更近
    int iconTop = dividerY + (monthView ? 4 : 14);
    for (int i = 0; i < actionCount; i++) {
        const MainMenuAction &action = kMainActions[mainMenuActionIndex(i, hasEntries)];
        int cx = baseX + i * slotW + slotW / 2 - scrollX;
        drawMainMenuIcon(cx, iconTop, action.icon, i == g_mainMenu.selection);
    }
    // 两侧的滚动提示
    if (moreLeft) g_font.drawText(2, iconTop + 24 + g_font.ascent() / 2, "<");
    if (moreRight) {
        int w = g_font.textWidth(">");
        g_font.drawText(SCREEN_W - w - 2, iconTop + 24 + g_font.ascent() / 2, ">");
    }
    const MainMenuAction &selected = kMainActions[mainMenuActionIndex(g_mainMenu.selection, hasEntries)];
    if (!monthView) {
        y = iconTop + 48 + rowH;
        char selectedText[48];
        snprintf(selectedText, sizeof(selectedText), "[%c] %s", selected.key, selected.name);
        ui_draw_text_centered(y, selectedText, false, true);
        y += rowH;
    } else {
        // 月视图动作标签固定 22pt(与日历一致);6 行日历等放不下时退 18pt
        int statusTop = SCREEN_H - g_font.lineHeight() - 2;  // 用户字号下的状态栏上沿
        int prevSize = g_font.fontSize();
        g_font.setSize(22);
        int labelY = iconTop + 48 + 2 + g_font.ascent();
        if (labelY + g_font.descent() > statusTop - 2) {
            g_font.setSize(18);
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
    g_browser.calOpen = false;
    g_browser.searchOpen = false;
    g_browser.imeActive = false;
    g_ime.setActive(false);
    cancelFullTextScan();
    const bool hadFullText = g_browser.fullTextActive;
    refreshBrowserCache();
    // 从编辑器保存后回来，或外部改动过日记：rebuild 会把过期结果降级成首行
    // 过滤并清掉 fullTextActive，这里自动重新增量扫描，恢复正文命中。
    if (hadFullText && !g_browser.fullTextActive && !g_browser.searchQuery.empty())
        startFullTextScan();
}

AppState screen_browser_handle(int key, ScreenContext &ctx) {
    auto &entries = g_browser.entries;
    if (entries.empty()) { ctx.nextState = APP_MAIN; return APP_MAIN; }

    // 全文增量扫描进行中：空闲节拍推进一批；任何按键立即中止扫描并继续按
    // 常规按键处理，保证长列表扫描期间界面不失去响应。
    if (g_browser.scanActive) {
        if (key == 0) {
            stepFullTextScan();
        } else {
            const bool esc = (key == 0x1B);
            cancelFullTextScan();
            rebuildBrowserView();
            // Esc 只中止扫描，保留搜索词；再按一次才清除搜索词
            if (esc) key = 0;
        }
    }

    if (g_browser.searchOpen) {
        // 搜索输入：IME 优先消费按键，其余字符逐字追加并实时过滤
        bool consumed = false;
        if (g_browser.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty() &&
                    g_browser.searchQuery.size() + imeOut.size() <= kMaxSearchBytes) {
                    g_browser.searchQuery += imeOut;
                    g_browser.selection = g_browser.scroll = 0;
                    invalidateFullTextSearch();
                    rebuildBrowserView();
                }
                consumed = true;
            }
        }
        if (!consumed) {
            if (key == KEY_IME_TOGGLE) {
                g_browser.imeActive = !g_browser.imeActive;
                g_ime.setActive(g_browser.imeActive);
            } else if (key == KEY_FULLWIDTH_TOGGLE) {
                g_ime.toggleFullwidth();
            } else if (key == KEY_TRAD_TOGGLE) {
                g_ime.toggleTrad();
            } else if (key == KEY_LSHIFT_TAP) {
                g_ime.toggleEnglish();
            } else if (key == 0x1B) {
                g_browser.searchOpen = false;
                g_browser.imeActive = false;
                g_ime.setActive(false);
                g_browser.searchQuery.clear();
                g_browser.selection = g_browser.scroll = 0;
                invalidateFullTextSearch();
                rebuildBrowserView();
            } else if (key == 0x0A || key == 0x0D) {
                g_browser.searchOpen = false;
                g_browser.imeActive = false;
                g_ime.setActive(false);
                startFullTextScan();  // 输入期按首行实时过滤，Enter 再补搜全文
            } else if (key == 0x7F || key == 0x08) {
                popUtf8Codepoint(g_browser.searchQuery);
                g_browser.selection = g_browser.scroll = 0;
                invalidateFullTextSearch();
                rebuildBrowserView();
            } else if (key >= 0x20 && key <= 0x7E) {
                if (g_browser.searchQuery.size() < kMaxSearchBytes) {
                    g_browser.searchQuery += (char)key;
                    g_browser.selection = g_browser.scroll = 0;
                    invalidateFullTextSearch();
                    rebuildBrowserView();
                }
            }
        }
    } else if (g_browser.calOpen) {
        // 浮动日历独占按键：Enter 按日期筛选，退格清除筛选，Esc 关闭面板
        CalendarPanelResult res = calendar_panel_handle_key(key, g_browser.cal);
        if (res == CalendarPanelResult::Accept) {
            g_browser.filterDate = calendar_panel_date_string(g_browser.cal);
            g_browser.selection = g_browser.scroll = 0;
            invalidateFullTextSearch();
            rebuildBrowserView();
            g_browser.calOpen = false;
        } else if (res == CalendarPanelResult::Clear) {
            g_browser.filterDate.clear();
            g_browser.selection = g_browser.scroll = 0;
            invalidateFullTextSearch();
            rebuildBrowserView();
            g_browser.calOpen = false;
        } else if (res == CalendarPanelResult::Cancel) {
            g_browser.calOpen = false;
        }
    } else {
        const int total = (int)g_browser.view.size();

        if (key == 'q' || key == 'Q') { ctx.nextState = APP_MAIN; return APP_MAIN; }
        if (key == 0x1B) {
            // 有搜索/日期筛选时 Esc 逐层清除，全部清空后再按才返回主界面
            if (!g_browser.searchQuery.empty()) {
                g_browser.searchQuery.clear();
                g_browser.selection = g_browser.scroll = 0;
                invalidateFullTextSearch();
                rebuildBrowserView();
            } else if (!g_browser.filterDate.empty()) {
                g_browser.filterDate.clear();
                g_browser.selection = g_browser.scroll = 0;
                invalidateFullTextSearch();
                rebuildBrowserView();
            } else {
                ctx.nextState = APP_MAIN;
                return APP_MAIN;
            }
        }
        if (key == '/') {
            g_browser.searchOpen = true;
            g_browser.imeActive = true;
            g_ime.setActive(true);
        } else if (key == 't' || key == 'T') {
            std::string base = g_browser.filterDate;
            if (base.empty()) {
                const JournalEntry *sel = browserSelectedEntry();
                if (sel) base = sel->date;
            }
            calendar_panel_reset(g_browser.cal, base);
            g_browser.calOpen = true;
        } else {
            if (key == 'j' || key == KEY_DOWN) {
                if (g_browser.selection < total - 1) g_browser.selection++;
            }
            if (key == 'k' || key == KEY_UP) {
                if (g_browser.selection > 0) g_browser.selection--;
            }
            const JournalEntry *sel = browserSelectedEntry();
            if (sel && (key == 0x0A || key == 0x0D)) {
                ctx.selectedEntry = sel->filename;
                ctx.nextState = APP_VIEWER;
                return APP_VIEWER;
            } else if (sel && (key == 'h' || key == 'H')) {
                ctx.selectedEntry = sel->filename;
                ctx.prevState = APP_BROWSER;
                ctx.nextState = APP_HISTORY;
                return APP_HISTORY;
            } else if (sel && (key == 'd' || key == 'D')) {
                const bool wasFullText = g_browser.fullTextActive;
                g_journal.deleteEntry(sel->filename);
                refreshBrowserCache();
                if (g_browser.entries.empty()) { ctx.nextState = APP_MAIN; return APP_MAIN; }
                // 原来在全文结果里删除：重新增量扫描，别退化成只看首行/标题
                if (wasFullText && !g_browser.searchQuery.empty()) startFullTextScan();
            } else if (sel && (key == 'e' || key == 'E')) {
                ctx.prevState = APP_BROWSER;
                std::string content = g_journal.readEntry(sel->filename);
                if (!content.empty()) {
                    ctx.editContent = extractBody(content);
                    ctx.editFilename = sel->filename;
                    ctx.promptMode = false;
                    ctx.promptText = "";
                    ctx.nextState = APP_EDITOR;
                    return APP_EDITOR;
                }
            } else if (sel && key == 0x13) {
                auto content = g_journal.readEntry(sel->filename);
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
        }
    }

    // 增量扫描未完成：显示进度（每个空闲节拍刷新一次），任意键可中止
    if (g_browser.scanActive) {
        char msg[64];
        snprintf(msg, sizeof(msg), "全文搜索 %d/%d  Esc取消",
                 g_browser.scanDone, g_browser.scanTotal);
        ui_show_message_centered(msg);
        return APP_BROWSER;
    }

    const int total = (int)g_browser.view.size();
    ui_clear(); int y = FONT_H;
    ui_draw_text(4, y, "过往日记", false, true);
    // 顶部右侧：日期筛选（粗体）或快捷键提示
    const bool filtered = !g_browser.filterDate.empty();
    const char *hint = filtered ? g_browser.filterDate.c_str() : "t日历 /搜索";
    int hintW = g_font.textWidth(hint);
    if (4 + g_font.textWidth("过往日记") + 8 <= SCREEN_W - hintW - 4)
        ui_draw_text(SCREEN_W - hintW - 4, y, hint, false, filtered);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, y + 7, SCREEN_W);
    y = y + 7 + LINE_SPACING - 4;
    const bool showSearchBar = g_browser.searchOpen || !g_browser.searchQuery.empty();
    int listBottom = showSearchBar ? SCREEN_H - LINE_SPACING - 6 : SCREEN_H;
    int visible = (listBottom - y + LINE_SPACING - 1) / LINE_SPACING;
    if (g_browser.selection < g_browser.scroll) g_browser.scroll = g_browser.selection;
    if (g_browser.selection >= g_browser.scroll + visible)
        g_browser.scroll = g_browser.selection - visible + 1;

    if (total == 0) {
        if (!g_browser.searchQuery.empty()) ui_draw_text_centered(SCREEN_H / 2, "无匹配日记");
        else if (filtered) ui_draw_text_centered(SCREEN_H / 2, "该日期无日记");
        else ui_draw_text_centered(SCREEN_H / 2, "暂无日记");
    } else {
        for (int i = 0; i < visible && (g_browser.scroll + i) < total; i++) {
            auto &e = entries[g_browser.view[g_browser.scroll + i]];
            bool sel = (g_browser.scroll + i == g_browser.selection);
            std::string dateDisplay;
            if (e.filename.length() >= 10) dateDisplay = e.filename.substr(0, 10);
            else dateDisplay = e.date;
            std::string preview = e.preview.empty() ? e.title : e.preview;
            char buf[80];
            snprintf(buf, sizeof(buf), "%s %s", dateDisplay.c_str(), preview.c_str());
            ui_draw_text(8, y + i * LINE_SPACING, buf, sel);
        }
    }

    if (showSearchBar) {
        char fb[128];
        snprintf(fb, sizeof(fb), "%s: %s",
                 g_browser.fullTextActive ? "全文搜索" : "搜索",
                 g_browser.searchQuery.c_str());
        ui_draw_text(4, SCREEN_H - 6, fb, true);
    }

    if (g_browser.calOpen) {
        // 有日记的日期用粗体标出
        calendar_panel_draw(g_browser.cal,
                            [](const std::string &date) { return g_journal.hasEntry(date); });
    }
    if (g_ime.composing()) drawIMEUIFullscreen();
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

    const int headerY = FONT_H;
    const int sepY = headerY + g_font.descent();
    const int contentY = sepY + 26;
    const int contentMaxY = STATUS_Y;

    if (editorVertical()) {
        // 竖排首字墨迹顶边对齐横排首行墨迹顶边(基线-上伸部),顶部不留整行空白
        int vTop = contentY - g_font.ascent();
        VerticalLayoutMetrics vm = verticalMetrics(6, vTop, SCREEN_W - 12, contentMaxY - vTop);
        bool mdOn = g_settings.markdownRender();
        mdSetRenderEnabled(mdOn);
        const auto &mdInfo = getViewerMdInfo(mdOn);
        auto data = buildVerticalData(g_viewer.lines, vm.rows, nullptr, &mdInfo);
        int maxScroll = (int)data.cols.size() - vm.cols;
        if (maxScroll < 0) maxScroll = 0;
        if (key == 'j' || key == KEY_DOWN || key == KEY_LEFT) g_viewer.scroll++;
        if (key == 'k' || key == KEY_UP || key == KEY_RIGHT) {
            if (g_viewer.scroll > 0) g_viewer.scroll--;
        }
        if (key == KEY_PAGE_DOWN) g_viewer.scroll += vm.cols;
        if (key == KEY_PAGE_UP) g_viewer.scroll -= vm.cols;
        if (g_viewer.scroll < 0) g_viewer.scroll = 0;
        if (g_viewer.scroll > maxScroll) g_viewer.scroll = maxScroll;

        ui_clear();
        std::string header = g_viewer.dateStr.empty() ? g_viewer.filename : g_viewer.dateStr;
        ui_draw_text(4, headerY, header.c_str(), true);
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawHLine(g_u8g2, 4, sepY, SCREEN_W - 8);
        VerticalGuideStyle guideStyle = VerticalGuideStyle::Solid;
        std::string guideStyleKey = g_settings.verticalReferenceLineStyle();
        if (guideStyleKey == "dash") guideStyle = VerticalGuideStyle::Dash;
        else if (guideStyleKey == "dot") guideStyle = VerticalGuideStyle::Dot;
        drawVerticalCols(g_viewer.lines, data, g_viewer.scroll, vm,
                         g_settings.verticalReferenceLine(), guideStyle);
        if (g_viewer.scroll > 0 && maxScroll > 0) {
            char pctStr[16];
            snprintf(pctStr, sizeof(pctStr), "%d%%", (g_viewer.scroll * 100) / maxScroll);
            int pctW = g_font.textWidth(pctStr);
            ui_draw_text(SCREEN_W - pctW - 4, headerY, pctStr);
        }
        ui_draw_status("竖排阅读 e编辑 h历史", "");
        ui_commit();
        return APP_VIEWER;
    }

    const auto& vrows = getViewerVrows();
    bool mdOn = g_settings.markdownRender();
    mdSetRenderEnabled(mdOn);
    const auto& mdInfo = getViewerMdInfo(mdOn);
    const int viewH = contentMaxY - contentY;
    if (key == 'j' || key == KEY_DOWN) g_viewer.scroll++;
    if (key == 'k' || key == KEY_UP) { if (g_viewer.scroll > 0) g_viewer.scroll--; }
    int maxScroll = vrows.empty() ? 0 :
        vrowScrollToShow(vrows, (int)vrows.size() - 1, viewH, &mdInfo);
    if (maxScroll < 0) maxScroll = 0;
    if (g_viewer.scroll > maxScroll) g_viewer.scroll = maxScroll;

    ui_clear();

    std::string header = g_viewer.dateStr;
    if (header.empty()) header = g_viewer.filename;
    ui_draw_text(4, headerY, header.c_str(), true);

    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 4, sepY, SCREEN_W - 8);

    int ly = contentY;
    for (int idx = g_viewer.scroll; idx < (int)vrows.size(); idx++) {
        if (idx > g_viewer.scroll) {
            ly += LINE_SPACING + vrowGapBefore(vrows, idx, &mdInfo);
            if (ly + g_font.descent() > contentMaxY) break;
        }
        auto &vr = vrows[idx];
        mdDrawVrow(4, ly, g_viewer.lines[vr.lineIdx], vr.start, vr.end, mdInfo[vr.lineIdx], vr.indentCells);
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
    const int headerY = FONT_H;
    const int sepY = headerY + g_font.descent();
    const int contentY = sepY + 26;
    const int contentMaxY = STATUS_Y;
    if (editorVertical()) {
        // 竖排首字墨迹顶边对齐横排首行墨迹顶边(基线-上伸部),顶部不留整行空白
        int vTop = contentY - g_font.ascent();
        VerticalLayoutMetrics vm = verticalMetrics(6, vTop, SCREEN_W - 12, contentMaxY - vTop);
        bool mdOn = g_settings.markdownRender();
        mdSetRenderEnabled(mdOn);
        const auto &mdInfo = getHistoryMdInfo(mdOn);
        auto data = buildVerticalData(g_history.lines, vm.rows, nullptr, &mdInfo);
        int maxScroll = (int)data.cols.size() - vm.cols;
        if (maxScroll < 0) maxScroll = 0;
        if (g_history.previewScroll > maxScroll) g_history.previewScroll = maxScroll;

        ui_clear();
        std::string header = "历史";
        if (!g_history.versions.empty()) header = historyTimeLabel(g_history.versions[g_history.selection].filename);
        ui_draw_text(4, headerY, header.c_str(), true);
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawHLine(g_u8g2, 4, sepY, SCREEN_W - 8);
        VerticalGuideStyle guideStyle = VerticalGuideStyle::Solid;
        std::string guideStyleKey = g_settings.verticalReferenceLineStyle();
        if (guideStyleKey == "dash") guideStyle = VerticalGuideStyle::Dash;
        else if (guideStyleKey == "dot") guideStyle = VerticalGuideStyle::Dot;
        drawVerticalCols(g_history.lines, data, g_history.previewScroll, vm,
                         g_settings.verticalReferenceLine(), guideStyle);
        ui_draw_status("竖排历史 r恢复 d删除 q返回", "");
        ui_commit();
        return;
    }

    const auto& vrows = getHistoryVrows();
    bool mdOn = g_settings.markdownRender();
    mdSetRenderEnabled(mdOn);
    const auto& mdInfo = getHistoryMdInfo(mdOn);
    const int viewH = contentMaxY - contentY;
    int maxScroll = vrows.empty() ? 0 :
        vrowScrollToShow(vrows, (int)vrows.size() - 1, viewH, &mdInfo);
    if (maxScroll < 0) maxScroll = 0;
    if (g_history.previewScroll > maxScroll) g_history.previewScroll = maxScroll;

    ui_clear();
    std::string header = "历史";
    if (!g_history.versions.empty()) header = historyTimeLabel(g_history.versions[g_history.selection].filename);
    ui_draw_text(4, headerY, header.c_str(), true);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 4, sepY, SCREEN_W - 8);

    int ly = contentY;
    for (int idx = g_history.previewScroll; idx < (int)vrows.size(); idx++) {
        if (idx > g_history.previewScroll) {
            ly += LINE_SPACING + vrowGapBefore(vrows, idx, &mdInfo);
            if (ly + g_font.descent() > contentMaxY) break;
        }
        auto &vr = vrows[idx];
        mdDrawVrow(4, ly, g_history.lines[vr.lineIdx], vr.start, vr.end, mdInfo[vr.lineIdx], vr.indentCells);
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
        int visible = 1;
        int maxScroll = 0;
        if (editorVertical()) {
            // 与 drawHistoryPreview 竖排参数保持一致(墨迹顶边对齐横排)
            int contentY = FONT_H + g_font.descent() + 26;
            int vTop = contentY - g_font.ascent();
            VerticalLayoutMetrics vm = verticalMetrics(6, vTop, SCREEN_W - 12, STATUS_Y - vTop);
            bool mdOn = g_settings.markdownRender();
            mdSetRenderEnabled(mdOn);
            auto data = buildVerticalData(g_history.lines, vm.rows, nullptr,
                                          &getHistoryMdInfo(mdOn));
            visible = vm.cols;
            maxScroll = (int)data.cols.size() - visible;
        } else {
            const auto& vrows = getHistoryVrows();
            bool mdOn = g_settings.markdownRender();
            const auto& mdInfo = getHistoryMdInfo(mdOn);
            int viewH = STATUS_Y - (FONT_H + g_font.descent() + 26);
            visible = vrowFitCount(vrows, g_history.previewScroll, viewH, &mdInfo);
            if (visible < 1) visible = 1;
            maxScroll = vrows.empty() ? 0 :
                vrowScrollToShow(vrows, (int)vrows.size() - 1, viewH, &mdInfo);
        }
        if (maxScroll < 0) maxScroll = 0;
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            g_history.preview = false;
            drawHistoryList();
            return APP_HISTORY;
        }
        if (key == 'j' || key == KEY_DOWN || (editorVertical() && key == KEY_LEFT)) {
            if (g_history.previewScroll < maxScroll) g_history.previewScroll++;
        }
        if (key == 'k' || key == KEY_UP || (editorVertical() && key == KEY_RIGHT)) {
            if (g_history.previewScroll > 0) g_history.previewScroll--;
        }
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
