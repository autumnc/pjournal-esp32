#include "screen_editor.h"
#include "io_probe.h"
#include "screen_polish.h"
#include "font_renderer.h"
#include "journal_storage.h"
#include "deepseek_client.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "quick_edit.h"
#include "file_edit.h"
#include "typing_click.h"
#include "ui_helpers.h"
#include "markdown_render.h"
#include "vertical_layout.h"
#include "ime/IME.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>
#include <algorithm>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *u8g2, int x, int y, int w, int h);
}

#include "clipboard.h"
#include <esp_log.h>

#define EDITOR_MAX_CELLS (SCREEN_W / g_font.halfAdvance())

// ── 上屏(commit)耗时探针 ─────────────────────────────────────────────────
//
// 和 IME 内部那套共用 PJOURNAL_IME_PERF_LOG 开关: 一次开机就能同时看到
// "匹配"和"上屏"两段。上屏这一段此前完全没有埋点, 所以"长词组卡顿"到底卡在
// IME 处理、正文插入、文档上下文重建还是重绘, 只能靠这个分开量。
//
// 阈值以下不打印: 一次上屏要打 2 行, 全部打印会把串口刷屏、反而看不出慢的那次。
#if PJOURNAL_IME_PERF_LOG
static const char *const EDITOR_PERF_TAG = "EditorPerf";
static const int64_t EDITOR_PERF_SLOW_US = 12000;
#define EDITOR_PERF_NOW() esp_timer_get_time()
// drawEditor 内部的分段: 布局(全篇重排)与绘制分开量。
static int64_t s_perfDrawStart = 0;
static int64_t s_perfLayoutEnd = 0;
#else
#define EDITOR_PERF_NOW() 0
#endif

// ── Editor state ─────────────────────────────────────────────────────────

struct EditorSnapshot {
    std::string text;
    int cx = 0;
    int cy = 0;
    int scroll = 0;
};

enum class UndoGroup {
    None,
    Typing,
    Delete,
    Structural,
};

enum class FileMgrMode {
    Browse,
    NewName,
    RenameName,
    ConfirmDelete,
};

// ── vi 模式状态 ──────────────────────────────────────────────────────────
// 固定大小结构, 不随文档增长; 命名寄存器按需分配。
enum class ViMode { Insert, Normal, Visual, VisualLine };
enum class ViOperator { None, Delete, Change, Yank, IndentRight, IndentLeft, ToggleCase };

struct ViRegister {
    std::string text;
    bool linewise = false;
};

struct ViState {
    ViMode mode = ViMode::Insert;
    ViOperator op = ViOperator::None;
    int count = 0, opCount = 0;
    bool wantRegister = false;
    char reg = 0;              // 0=unnamed, a-z, A-Z=append
    bool gPending = false;     // 按下 g 后
    char findCmd = 0, findChar = 0, lastFindCmd = 0, lastFindChar = 0;
    bool replacePending = false;
    bool insertImePref = true;
    bool cmdlineActive = false;
    bool cmdlineIme = false;   // 仅 '/' '?' 命令栏允许打开输入法
    char cmdlineType = ':';
    std::string cmdline;
    int cmdlineCur = 0;
    bool lastSearchForward = true;
    std::string lastSearch;
    ViRegister regs[26];
};

struct EditorState {
    std::vector<std::string> lines;
    int cx = 0, cy = 0;
    int scroll = 0;
    int targetCx = -1;
    std::string promptText;
    std::string titleOverride;
    bool promptMode = false;
    bool imeActive = false;
    bool confirmSave = false;
    bool vrowsDirty = true;
    std::vector<VRow> cachedVrows;
    bool cachedFirstLineIndent = false;
    int cachedGutterCells = -1;
    int cachedWordCount = 0;
    bool wordCountDirty = true;
    bool mdInfoDirty = true;
    std::vector<MdLineInfo> cachedMdInfo;
    bool cachedMdOn = false;
    // 折叠的标题行号(Ctrl+T 切换)。视图态:不随文本持久化,按行号平移维护。
    std::set<int> foldedHeadings;
    std::vector<EditorSnapshot> undoStack;
    std::vector<EditorSnapshot> redoStack;
    size_t undoBytes = 0;
    size_t redoBytes = 0;
    UndoGroup lastUndoGroup = UndoGroup::None;
    int64_t lastUndoTime = 0;
    int64_t autoSaveTime = 0;
    bool modifiedSinceSave = false;
    ViState vi;
    std::string savedFilename;
    bool recoveryPrompt = false;
    std::string recoveryContent;
    std::string recoveryMeta;
    uint32_t lastRecoveryHash = 0;
    bool promptGenerating = false;

    // Selection
    bool hasSelection = false;
    int selAnchorCy = 0, selAnchorCx = 0;
    // Whether the editor content is currently on screen. Idle ticks skip the
    // full redraw once it is; reset when another screen paints over it.
    bool drawnOnce = false;

    // 查找/替换对话框 (Ctrl+/ 打开)
    struct {
        bool active = false;
        std::string term;        // 查找词(UTF-8,可含换行)
        std::string rep;         // 替换文本
        int termCur = 0, repCur = 0;  // 各字段光标(字节偏移)
        bool focusRep = false;   // false=查找字段, true=替换字段
        bool imeActive = false;  // 对话框内输入法开关
        int cur = -1;            // 当前匹配索引(matches 为空时 -1)
        std::vector<std::pair<int,int>> matches;  // 匹配区间[docStart,docEnd)
    } search;

    // 快捷键帮助对话框 (Ctrl+?)
    bool helpActive = false;
    int helpScroll = 0;

    // 文件编辑模式内的本地文件管理器(Ctrl+E)
    struct {
        bool active = false;
        std::string dir = "/sdcard";
        std::vector<FileEditEntry> entries;
        int selection = 0;
        int scroll = 0;
        FileMgrMode mode = FileMgrMode::Browse;
        std::string nameBuffer;
        int nameCursor = 0;
        std::string targetPath;
    } fileMgr;
};

static EditorState g_editor;
static EditorState s_stashedEditor;
static bool s_hasStashedEditor = false;
// 语音听写背景模式:只画正文,不画状态栏/候选条/行号。
static bool s_skipStatusBarAndIme = false;
// 全局剪贴板内容是否为整行(vi linewise), 与 g_clipboard 配套, 跨编辑器重开保留。
static bool s_clipboardLinewise = false;

static volatile bool s_promptTaskDone = false;
static DeepseekResult s_promptTaskResult = {false, ""};
static std::string s_promptTaskContext;
static SemaphoreHandle_t s_promptResultMutex = nullptr;

static void ensurePromptResultMutex() {
    if (!s_promptResultMutex) s_promptResultMutex = xSemaphoreCreateMutex();
}

static void lockPromptResult() {
    ensurePromptResultMutex();
    if (s_promptResultMutex) xSemaphoreTake(s_promptResultMutex, portMAX_DELAY);
}

static void unlockPromptResult() {
    if (s_promptResultMutex) xSemaphoreGive(s_promptResultMutex);
}

// Mark every line-derived cache (vrows, word count, markdown info) stale.
static void markDirty() {
    g_editor.vrowsDirty = true;
    g_editor.wordCountDirty = true;
    g_editor.mdInfoDirty = true;
}

// 打字机模式:光标钉在可视区中间(按键音效与输入模式无关,由设置单独控制)
static bool editorTypewriter() { return g_settings.inputMode() == "typewriter"; }
static bool editorVertical() { return g_settings.editorOrientation() == "vertical"; }

// ── 行号槽 ───────────────────────────────────────────────────────────────
// 仅横排编辑区显示;竖排从右往左分列,没有合适的左侧槽位,语音背景也不显示。
static bool editorLineNumbersOn() {
    return g_settings.showLineNumbers() && !editorVertical() && !s_skipStatusBarAndIme;
}
static int editorGutterDigits() {
    int n = (int)g_editor.lines.size();
    int d = 1;
    while (n >= 10) { n /= 10; ++d; }
    return d;
}
static int editorGutterCells() {
    return editorLineNumbersOn() ? editorGutterDigits() + 1 : 0;  // +1 格与正文留白
}
static int editorTextX() {
    return 4 + editorGutterCells() * g_font.halfAdvance();
}
static int editorTextCells() {
    int n = EDITOR_MAX_CELLS - editorGutterCells();
    return n < 1 ? 1 : n;
}
static bool inFileEditSession();
static void drawEditor();
static int utf8Next(const std::string &s, int pos);
static int utf8Prev(const std::string &s, int pos);
static void insertMultilineAt(int cy, int cx, const std::string &text,
                              int &outCy, int &outCx);
static void insertLinesAt(int at, const std::vector<std::string> &lines,
                          int &outCy, int &outCx);

static const char *editorStatusTitle() {
    if (!g_editor.titleOverride.empty()) return g_editor.titleOverride.c_str();
    if (inFileEditSession()) return "文件编辑";
    return g_editor.promptMode ? "提示写作" : "自由写作";
}

// 行数变化时平移折叠标题的行号,保持折叠集与缓冲区对齐。
static void foldLinesInserted(int at, int count) {
    if (g_editor.foldedHeadings.empty()) return;
    std::set<int> shifted;
    for (int e : g_editor.foldedHeadings)
        shifted.insert(e >= at ? e + count : e);
    g_editor.foldedHeadings.swap(shifted);
}

static void foldLinesErased(int at, int count) {
    if (g_editor.foldedHeadings.empty()) return;
    std::set<int> shifted;
    for (int e : g_editor.foldedHeadings) {
        if (e >= at && e < at + count) continue;   // 被删除的折叠标题
        shifted.insert(e >= at + count ? e - count : e);
    }
    g_editor.foldedHeadings.swap(shifted);
}

static uint32_t fnv1a(const std::string &s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h ? h : 1;
}

static std::string metaValue(const std::string &meta, const char *key) {
    std::string prefix = std::string(key) + "=";
    size_t pos = meta.find(prefix);
    if (pos == std::string::npos) return "";
    pos += prefix.size();
    size_t end = meta.find('\n', pos);
    return meta.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// ── Selection helpers ────────────────────────────────────────────────────
// Selection is defined by anchor (selAnchorCy, selAnchorCx) and cursor (cy, cx).
// The "start" is the earlier position, "end" is the later one.

struct TextPos { int cy, cx; };

static bool posLess(const TextPos &a, const TextPos &b) {
    if (a.cy != b.cy) return a.cy < b.cy;
    return a.cx < b.cx;
}

static void getSelRange(TextPos &start, TextPos &end) {
    if (!g_editor.hasSelection) {
        start = {g_editor.cy, g_editor.cx};
        end = start;
        return;
    }
    TextPos anchor = {g_editor.selAnchorCy, g_editor.selAnchorCx};
    TextPos cursor = {g_editor.cy, g_editor.cx};
    if (posLess(anchor, cursor)) { start = anchor; end = cursor; }
    else { start = cursor; end = anchor; }
}

static std::string getSelectedText() {
    TextPos start, end;
    getSelRange(start, end);
    if (start.cy == end.cy && start.cx == end.cx) return "";
    std::string result;
    if (start.cy == end.cy) {
        result = g_editor.lines[start.cy].substr(start.cx, end.cx - start.cx);
    } else {
        result = g_editor.lines[start.cy].substr(start.cx) + "\n";
        for (int i = start.cy + 1; i < end.cy; i++)
            result += g_editor.lines[i] + "\n";
        result += g_editor.lines[end.cy].substr(0, end.cx);
    }
    return result;
}

static void deleteSelection() {
    TextPos start, end;
    getSelRange(start, end);
    if (start.cy == end.cy && start.cx == end.cx) return;
    // Keep text before start and after end, join on same line
    g_editor.lines[start.cy] = g_editor.lines[start.cy].substr(0, start.cx)
        + g_editor.lines[end.cy].substr(end.cx);
    // Remove lines between start and end
    if (end.cy > start.cy) {
        g_editor.lines.erase(g_editor.lines.begin() + start.cy + 1,
                             g_editor.lines.begin() + end.cy + 1);
        foldLinesErased(start.cy + 1, end.cy - start.cy);
    }
    g_editor.cy = start.cy;
    g_editor.cx = start.cx;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

static void clearSelection() {
    g_editor.hasSelection = false;
}

static void extendSelection() {
    if (!g_editor.hasSelection) {
        g_editor.selAnchorCy = g_editor.cy;
        g_editor.selAnchorCx = g_editor.cx;
        g_editor.hasSelection = true;
    }
}

// Move cursor vertically by `step` visual rows (negative = up, positive = down),
// preserving the target visual column. Returns true if the cursor moved.
static bool moveCursorVertical(int step, const std::vector<VRow> &vrows) {
    int curVR = -1;
    for (int vi = 0; vi < (int)vrows.size(); vi++) {
        if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
            curVR = vi; break;
        }
    }
    if (curVR < 0) return false;
    int targetVR = curVR + step;
    if (targetVR < 0) targetVR = 0;
    if (targetVR > (int)vrows.size() - 1) targetVR = (int)vrows.size() - 1;
    if (targetVR == curVR) return false;
    auto &dst = vrows[targetVR];
    if (g_editor.targetCx < 0)
        g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
    int visualCol = g_editor.targetCx % editorTextCells();
    g_editor.cy = dst.lineIdx;
    int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], dst.start);
    int targetCells = vrowStartCells + visualCol;
    g_editor.cx = cellsToByte(g_editor.lines[g_editor.cy], dst.start, dst.end, targetCells);
    return true;
}

// 一屏可显示的行数(减去状态栏并留一行上下文), 作为 PageUp/PageDown 的翻页步长。
static int editorPageRows() {
    int rows = (STATUS_Y - FONT_H + LINE_SPACING - 1) / LINE_SPACING - 1;
    if (rows < 1) rows = 1;
    return rows;
}

static const std::vector<MdLineInfo>& getMdInfo(bool mdOn);

static const std::vector<VRow>& getVrows() {
    bool firstLineIndent = g_settings.firstLineIndent();
    int gutter = editorGutterCells();
    if (g_editor.vrowsDirty || g_editor.cachedFirstLineIndent != firstLineIndent ||
        g_editor.cachedGutterCells != gutter) {
        g_editor.cachedFirstLineIndent = firstLineIndent;
        g_editor.cachedGutterCells = gutter;
        // 传缓存避免重复 classify;md 渲染关闭时缓存全零,须传 nullptr 让
        // buildVrows 自行 classify(首行缩进仍需区分标题/列表)。
        bool mdOn = g_settings.markdownRender();
        const auto &mi = getMdInfo(mdOn);
        g_editor.cachedVrows = buildVrows(g_editor.lines, mdOn ? &mi : nullptr,
                                          &g_editor.foldedHeadings, gutter);
        g_editor.vrowsDirty = false;
    }
    return g_editor.cachedVrows;
}

static const std::vector<MdLineInfo>& getMdInfo(bool mdOn) {
    // Recompute when lines changed OR when the markdown toggle changed.
    if (g_editor.mdInfoDirty || g_editor.cachedMdOn != mdOn) {
        g_editor.cachedMdOn = mdOn;
        if (mdOn) {
            g_editor.cachedMdInfo = mdClassifyLines(g_editor.lines);
        } else {
            g_editor.cachedMdInfo.assign(g_editor.lines.size(), MdLineInfo{});
        }
        g_editor.mdInfoDirty = false;
    }
    return g_editor.cachedMdInfo;
}

static int getWordCount() {
    if (g_editor.wordCountDirty) {
        std::string fullText;
        for (auto &l : g_editor.lines) {
            if (!fullText.empty()) fullText += '\n';
            fullText += l;
        }
        g_editor.cachedWordCount = countVisibleChars(fullText);
        g_editor.wordCountDirty = false;
    }
    return g_editor.cachedWordCount;
}

// ── Quick edit (快捷编辑) helpers ─────────────────────────────────────────
static std::string currentEditorText() {
    std::string text;
    for (auto &l : g_editor.lines) { text += l; text += '\n'; }
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

static void loadLinesIntoEditor(const std::string &content) {
    g_editor.lines.clear();
    g_editor.foldedHeadings.clear();  // 折叠是视图态,整篇重载(undo/加载)后不保留
    if (content.empty()) {
        g_editor.lines.push_back("");
        g_editor.cx = g_editor.cy = 0;
        return;
    }
    size_t pos = 0;
    while (pos < content.length()) {
        size_t nl = content.find('\n', pos);
        g_editor.lines.push_back((nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    while (g_editor.lines.size() > 1 && g_editor.lines.back().empty())
        g_editor.lines.pop_back();
    g_editor.cx = (int)g_editor.lines.back().length();
    g_editor.cy = (int)g_editor.lines.size() - 1;
}

static EditorSnapshot makeSnapshot() {
    EditorSnapshot s;
    s.text = currentEditorText();
    s.cx = g_editor.cx;
    s.cy = g_editor.cy;
    s.scroll = g_editor.scroll;
    return s;
}

static void restoreSnapshot(const EditorSnapshot &s) {
    loadLinesIntoEditor(s.text);
    g_editor.cy = s.cy;
    if (g_editor.cy < 0) g_editor.cy = 0;
    if (g_editor.cy >= (int)g_editor.lines.size()) g_editor.cy = (int)g_editor.lines.size() - 1;
    g_editor.cx = s.cx;
    if (g_editor.cx < 0) g_editor.cx = 0;
    if (g_editor.cx > (int)g_editor.lines[g_editor.cy].length())
        g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
    g_editor.scroll = s.scroll;
    g_editor.targetCx = -1;
    g_editor.hasSelection = false;
    markDirty();
}

static void trimUndoStack(std::vector<EditorSnapshot> &stack, size_t &bytes) {
    const size_t maxBytes = 192 * 1024;
    const size_t maxItems = 32;
    while (stack.size() > maxItems || bytes > maxBytes) {
        if (stack.empty()) break;
        size_t sz = stack.front().text.size();
        bytes = (bytes >= sz) ? (bytes - sz) : 0;
        stack.erase(stack.begin());
    }
}

static void clearRedoHistory() {
    g_editor.redoStack.clear();
    g_editor.redoBytes = 0;
}

static void recordUndoSnapshot(UndoGroup group = UndoGroup::Structural) {
    int64_t now = esp_timer_get_time();
    bool mergeable = group == UndoGroup::Typing || group == UndoGroup::Delete;
    if (mergeable && g_editor.lastUndoGroup == group &&
        now - g_editor.lastUndoTime < 1000000) {
        g_editor.lastUndoTime = now;
        clearRedoHistory();
        return;
    }
    EditorSnapshot s = makeSnapshot();
    if (!g_editor.undoStack.empty() && g_editor.undoStack.back().text == s.text) return;
    if (s.text.size() > 96 * 1024) {
        g_editor.undoStack.clear();
        g_editor.undoBytes = 0;
        clearRedoHistory();
        g_editor.lastUndoGroup = UndoGroup::None;
        return;
    }
    size_t sz = s.text.size();
    g_editor.undoStack.push_back(std::move(s));
    g_editor.undoBytes += sz;
    trimUndoStack(g_editor.undoStack, g_editor.undoBytes);
    clearRedoHistory();
    g_editor.lastUndoGroup = group;
    g_editor.lastUndoTime = now;
}

static bool undoEditor() {
    if (g_editor.undoStack.empty()) return false;
    EditorSnapshot cur = makeSnapshot();
    size_t curSize = cur.text.size();
    g_editor.redoStack.push_back(std::move(cur));
    g_editor.redoBytes += curSize;
    trimUndoStack(g_editor.redoStack, g_editor.redoBytes);
    EditorSnapshot s = std::move(g_editor.undoStack.back());
    size_t sz = s.text.size();
    g_editor.undoBytes = (g_editor.undoBytes >= sz) ? (g_editor.undoBytes - sz) : 0;
    g_editor.undoStack.pop_back();
    restoreSnapshot(s);
    g_editor.lastUndoGroup = UndoGroup::None;
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
    return true;
}

static bool redoEditor() {
    if (g_editor.redoStack.empty()) return false;
    EditorSnapshot cur = makeSnapshot();
    size_t curSize = cur.text.size();
    g_editor.undoStack.push_back(std::move(cur));
    g_editor.undoBytes += curSize;
    trimUndoStack(g_editor.undoStack, g_editor.undoBytes);
    EditorSnapshot s = std::move(g_editor.redoStack.back());
    size_t sz = s.text.size();
    g_editor.redoBytes = (g_editor.redoBytes >= sz) ? (g_editor.redoBytes - sz) : 0;
    g_editor.redoStack.pop_back();
    restoreSnapshot(s);
    g_editor.lastUndoGroup = UndoGroup::None;
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
    return true;
}

static void clearUndoHistory() {
    g_editor.undoStack.clear();
    g_editor.redoStack.clear();
    g_editor.undoBytes = 0;
    g_editor.redoBytes = 0;
    g_editor.lastUndoGroup = UndoGroup::None;
    g_editor.lastUndoTime = 0;
}

static void loadQuickEditFile() {
    loadLinesIntoEditor(quickEditLoad(quickEditIndex()));
    g_editor.scroll = 0;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.modifiedSinceSave = false;
    g_editor.autoSaveTime = 0;
    clearUndoHistory();
}

// 是否为快捷编辑主会话(直接编辑 /sdcard/{n}.txt)。g_quickEdit 模式下若正
// 在编辑灵感/日记内容(savedFilename 非空),则不属于快捷文件会话。
static bool inQuickFileSession() {
    return g_quickEdit && g_editor.savedFilename.empty();
}

static bool inFileEditSession() {
    return g_fileEdit && g_editor.savedFilename.empty();
}

static void fileMgrRefresh() {
    auto &fm = g_editor.fileMgr;
    fm.entries = fileEditListDir(fm.dir);
    if (fm.selection >= (int)fm.entries.size()) fm.selection = (int)fm.entries.size() - 1;
    if (fm.selection < 0) fm.selection = 0;
    if (fm.scroll > fm.selection) fm.scroll = fm.selection;
}

static void fileMgrOpen() {
    auto &fm = g_editor.fileMgr;
    fm.active = true;
    fm.dir = fileEditCurrentDir();
    fm.mode = FileMgrMode::Browse;
    fm.nameBuffer.clear();
    fm.nameCursor = 0;
    fm.targetPath.clear();
    fileMgrRefresh();
    std::string cur = fileEditCurrentPath();
    for (int i = 0; i < (int)fm.entries.size(); i++) {
        if (fm.entries[i].path == cur) {
            fm.selection = i;
            break;
        }
    }
}

static void fileMgrClose() {
    g_editor.fileMgr.active = false;
    g_editor.fileMgr.mode = FileMgrMode::Browse;
    g_editor.drawnOnce = false;
}

static bool saveFileEditBeforeSwitch() {
    if (!inFileEditSession()) return true;
    if (fileEditSave(currentEditorText())) {
        g_editor.modifiedSinceSave = false;
        return true;
    }
    return false;
}

static void loadFileEditCurrent() {
    loadLinesIntoEditor(fileEditLoad());
    g_editor.scroll = 0;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.modifiedSinceSave = false;
    g_editor.autoSaveTime = 0;
    clearUndoHistory();
}

static std::string displayPathShort(const std::string &path) {
    if (path.find("/sdcard/") == 0) return path.substr(8);
    if (path == "/sdcard") return "/";
    return path;
}

static void drawFileManagerPanel() {
    auto &fm = g_editor.fileMgr;
    g_editor.drawnOnce = true;
    ui_clear();
    int y = FONT_H;
    ui_draw_text(4, y, "文件", false, true);
    std::string dirLabel = displayPathShort(fm.dir);
    int dirW = g_font.textWidth(dirLabel.c_str());
    if (dirW > SCREEN_W - 80) dirLabel = "..." + dirLabel.substr(dirLabel.size() > 18 ? dirLabel.size() - 18 : 0);
    ui_draw_text(SCREEN_W - 4 - g_font.textWidth(dirLabel.c_str()), y, dirLabel.c_str());
    y += 4;
    u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
    y += LINE_SPACING;

    if (fm.mode == FileMgrMode::NewName || fm.mode == FileMgrMode::RenameName) {
        ui_draw_text(4, y, fm.mode == FileMgrMode::NewName ? "新建文件名:" : "改名为:");
        y += LINE_SPACING;
        std::string label = fm.nameBuffer.empty() ? " " : fm.nameBuffer;
        ui_draw_text(4, y, label.c_str(), false, true);
        int cx = 4 + g_font.textWidth(fm.nameBuffer.substr(0, fm.nameCursor).c_str());
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, cx, y + 4, 8, 3);
        ui_draw_status("Enter确认 Esc取消", imeStatusLabel(g_editor.imeActive).c_str());
        if (g_editor.imeActive && g_ime.composing()) drawIMEUIFullscreen();
        ui_commit();
        return;
    }

    if (fm.mode == FileMgrMode::ConfirmDelete) {
        std::string name = fileEditBaseName(fm.targetPath);
        ui_draw_text_centered(105, "确认删除?");
        ui_draw_text_centered(135, name.c_str());
        ui_draw_status("Enter删除 Esc取消", "");
        ui_commit();
        return;
    }

    int visible = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    if (visible < 1) visible = 1;
    if (fm.selection < fm.scroll) fm.scroll = fm.selection;
    if (fm.selection >= fm.scroll + visible) fm.scroll = fm.selection - visible + 1;
    if (fm.entries.empty()) {
        ui_draw_text_centered(135, "空目录");
    } else {
        for (int i = 0; i < visible && fm.scroll + i < (int)fm.entries.size(); i++) {
            int idx = fm.scroll + i;
            const auto &e = fm.entries[idx];
            std::string line = e.isDir ? ("[" + e.name + "]") : e.name;
            bool sel = idx == fm.selection;
            ui_draw_text(4, y + i * LINE_SPACING, line.c_str(), sel, false);
        }
    }
    ui_draw_status("Ctrl+E关 a新 r改 d删 Enter开", "");
    ui_commit();
}

static bool fileMgrNameTextSafe(const std::string &text) {
    return text.find('/') == std::string::npos && text.find('\\') == std::string::npos;
}

static void fileMgrInsertNameText(const std::string &text) {
    if (text.empty() || !fileMgrNameTextSafe(text)) return;
    auto &fm = g_editor.fileMgr;
    fm.nameBuffer.insert(fm.nameCursor, text);
    fm.nameCursor += (int)text.length();
}

static void fileMgrHandleNameRawKey(int key) {
    auto &fm = g_editor.fileMgr;
    if (key >= 32 && key < 127 && key != '/' && key != '\\') {
        fm.nameBuffer.insert(fm.nameCursor, 1, (char)key);
        fm.nameCursor++;
    } else if (key == '\b') {
        if (fm.nameCursor > 0) {
            int prev = utf8Prev(fm.nameBuffer, fm.nameCursor);
            fm.nameBuffer.erase(prev, fm.nameCursor - prev);
            fm.nameCursor = prev;
        }
    } else if (key == KEY_LEFT) {
        if (fm.nameCursor > 0) fm.nameCursor = utf8Prev(fm.nameBuffer, fm.nameCursor);
    } else if (key == KEY_RIGHT) {
        if (fm.nameCursor < (int)fm.nameBuffer.size())
            fm.nameCursor = utf8Next(fm.nameBuffer, fm.nameCursor);
    }
}

static AppState screen_editor_file_manager_handle(int key, ScreenContext &ctx) {
    auto &fm = g_editor.fileMgr;
    if (key == 0x05) {
        fileMgrClose();
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }

    if (fm.mode == FileMgrMode::NewName || fm.mode == FileMgrMode::RenameName) {
        if (g_editor.imeActive && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                fileMgrInsertNameText(imeOut);
                drawFileManagerPanel();
                return APP_EDITOR;
            }
        }
        if (key == 0x1B) {
            fm.mode = FileMgrMode::Browse;
        } else if (key == 0x0A || key == 0x0D) {
            std::string outPath;
            bool ok = false;
            if (fm.mode == FileMgrMode::NewName) {
                ok = fileEditCreateFile(fm.dir, fm.nameBuffer, outPath);
            } else {
                ok = fileEditRenamePath(fm.targetPath, fm.nameBuffer, outPath);
            }
            if (ok) {
                fm.mode = FileMgrMode::Browse;
                fileMgrRefresh();
                for (int i = 0; i < (int)fm.entries.size(); i++) {
                    if (fm.entries[i].path == outPath) { fm.selection = i; break; }
                }
            } else {
                ctx.statusMessage = "操作失败";
                ctx.statusDuration = 30;
            }
        } else {
            fileMgrHandleNameRawKey(key);
        }
        drawFileManagerPanel();
        return APP_EDITOR;
    }

    if (fm.mode == FileMgrMode::ConfirmDelete) {
        if (key == 0x1B || key == 'n' || key == 'N') {
            fm.mode = FileMgrMode::Browse;
        } else if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            bool deletingCurrent = (fm.targetPath == fileEditCurrentPath());
            if (fileEditDeletePath(fm.targetPath)) {
                if (deletingCurrent) {
                    std::string name = fileEditUniqueName(fm.dir, "untitled.txt");
                    std::string newPath;
                    if (fileEditCreateFile(fm.dir, name, newPath)) {
                        fileEditSetCurrentPath(newPath);
                        loadFileEditCurrent();
                    }
                }
                ctx.statusMessage = "已删除";
            } else {
                ctx.statusMessage = "删除失败";
            }
            ctx.statusDuration = 30;
            fm.mode = FileMgrMode::Browse;
            fileMgrRefresh();
        }
        drawFileManagerPanel();
        return APP_EDITOR;
    }

    if (key == 0x1B) {
        fileMgrClose();
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == KEY_UP || key == 'k') {
        if (fm.selection > 0) fm.selection--;
    } else if (key == KEY_DOWN || key == 'j') {
        if (fm.selection + 1 < (int)fm.entries.size()) fm.selection++;
    } else if (key == 'a' || key == 'A') {
        fm.mode = FileMgrMode::NewName;
        fm.nameBuffer = fileEditUniqueName(fm.dir, "untitled.txt");
        fm.nameCursor = (int)fm.nameBuffer.size();
    } else if ((key == 'r' || key == 'R') && !fm.entries.empty() && fm.entries[fm.selection].name != "..") {
        fm.mode = FileMgrMode::RenameName;
        fm.targetPath = fm.entries[fm.selection].path;
        fm.nameBuffer = fm.entries[fm.selection].name;
        fm.nameCursor = (int)fm.nameBuffer.size();
    } else if ((key == 'd' || key == 'D') && !fm.entries.empty() && fm.entries[fm.selection].name != "..") {
        fm.mode = FileMgrMode::ConfirmDelete;
        fm.targetPath = fm.entries[fm.selection].path;
    } else if ((key == 0x0A || key == 0x0D) && !fm.entries.empty()) {
        const auto &e = fm.entries[fm.selection];
        if (e.isDir) {
            fm.dir = e.path;
            fm.selection = fm.scroll = 0;
            fileMgrRefresh();
        } else if (saveFileEditBeforeSwitch()) {
            fileEditSetCurrentPath(e.path);
            loadFileEditCurrent();
            fileMgrClose();
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        } else {
            ctx.statusMessage = "保存失败";
            ctx.statusDuration = 30;
        }
    }
    drawFileManagerPanel();
    return APP_EDITOR;
}

static bool saveRecoveryDraftIfChanged() {
    std::string text = currentEditorText();
    std::string meta;
    meta += std::string("mode=") + (inQuickFileSession() ? "quick" : (inFileEditSession() ? "file" : "journal")) + "\n";
    meta += "filename=" + g_editor.savedFilename + "\n";
    if (inFileEditSession()) meta += "file_path=" + fileEditCurrentPath() + "\n";
    meta += "quick_index=" + std::to_string(quickEditIndex()) + "\n";
    time_t now;
    time(&now);
    meta += "timestamp=" + std::to_string((long long)now) + "\n";
    uint32_t hash = fnv1a(text + "\n" + metaValue(meta, "mode") + "\n" +
                          metaValue(meta, "filename") + "\n" +
                          metaValue(meta, "quick_index"));
    if (hash == g_editor.lastRecoveryHash) return true;
    if (!g_journal.saveRecoveryDraft(text, meta)) return false;
    g_editor.lastRecoveryHash = hash;
    return true;
}

static void quickEditSwitchTo(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;
    if (idx == quickEditIndex()) return;
    if (quickEditSave(quickEditIndex(), currentEditorText())) g_journal.clearRecoveryDraft();
    quickEditSetIndex(idx);
    loadQuickEditFile();
}

// ── 查找/替换 (Ctrl+/) ────────────────────────────────────────────────────
// 匹配区间用"文档字节偏移"表示:文档 = currentEditorText()(行间以 '\n' 连接,
// 无结尾换行)。docOffsetToPos/posToDocOffset 与行坐标互转。

static const char *ELLIPSIS = "\xe2\x80\xa6";  // "…" U+2026 (3 bytes)

// pos 之后第一个 UTF-8 字符边界(跳过后续字节);越界返回串尾。
static int utf8Next(const std::string &s, int pos) {
    if (pos < 0 || pos >= (int)s.length()) return (int)s.length();
    const char *p = s.c_str() + pos;
    FontRenderer::utf8Decode(p);
    return (int)(p - s.c_str());
}
// pos 之前一个 UTF-8 字符边界。
static int utf8Prev(const std::string &s, int pos) {
    if (pos <= 0) return 0;
    int prev = pos - 1;
    while (prev > 0 && ((unsigned char)s[prev] & 0xC0) == 0x80) prev--;
    return prev;
}

// 字符串中的 UTF-8 字符(码点)数。
static int utf8Count(const std::string &s) {
    int n = 0;
    for (int i = 0; i < (int)s.length(); i = utf8Next(s, i)) n++;
    return n;
}

// 喂给输入法的"文档上下文":光标之前的正文尾部约 200 字。从末行往前拼、拼够就停,
// 免得为一个尾窗口把整篇正文复制一遍。只取光标之前的内容,后面还没写的不算上下文。
static std::string editorImeContextText() {
    const int kTargetChars = 200;
    const size_t kMaxBytes = 640;  // 200 个 CJK 字的上限,超出按字节裁并回退到字边界
    std::string tail;
    int chars = 0;
    for (int y = g_editor.cy; y >= 0; y--) {
        std::string line = (y == g_editor.cy)
            ? g_editor.lines[y].substr(0, g_editor.cx)
            : g_editor.lines[y];
        tail.insert(0, line);
        chars += utf8Count(line) + 1;  // +1 记行间换行
        if (y > 0) tail.insert(0, "\n");
        if (chars >= kTargetChars) break;
    }
    while (!tail.empty() && tail.front() == '\n') tail.erase(0, 1);
    if (tail.size() > kMaxBytes) {
        size_t start = tail.size() - kMaxBytes;
        while (start < tail.size() && ((unsigned char)tail[start] & 0xC0) == 0x80) start++;
        tail.erase(0, start);
    }
    return tail;
}

static void moveCursorVerticalInline(int step) {
    if (step < 0) {
        if (g_editor.cx > 0) {
            g_editor.cx = utf8Prev(g_editor.lines[g_editor.cy], g_editor.cx);
        } else if (g_editor.cy > 0) {
            g_editor.cy--;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
        }
    } else if (step > 0) {
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            g_editor.cx = utf8Next(g_editor.lines[g_editor.cy], g_editor.cx);
        } else if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            g_editor.cy++;
            g_editor.cx = 0;
        }
    }
    g_editor.targetCx = -1;
}

static void moveCursorVerticalColumn(int dir, const VerticalData &data) {
    int curCol = verticalFindCol(data, g_editor.lines, g_editor.cy, g_editor.cx);
    if (curCol < 0) return;
    int row = verticalCellRow(data.cells[g_editor.cy], g_editor.cx) - data.cols[curCol].start;
    int dstCol = curCol + dir;
    if (dstCol < 0) dstCol = 0;
    if (dstCol >= (int)data.cols.size()) dstCol = (int)data.cols.size() - 1;
    const auto &dst = data.cols[dstCol];
    g_editor.cy = dst.lineIdx;
    g_editor.cx = verticalRowToByte(data.cells[dst.lineIdx], dst.start, dst.end, row);
    g_editor.targetCx = -1;
}

// 竖排选区高亮:对 [hStart, hEnd) 范围内的字符格做 XOR 反白
// (与横排选区同机制,draw color 2),格 = 整个字身方(线高×线高)
static void drawVerticalHighlight(const VerticalData &data,
                                  int scrollCol, const VerticalLayoutMetrics &vm,
                                  TextPos hStart, TextPos hEnd) {
    int cell = g_font.lineHeight();
    for (int ci = 0; ci < vm.cols; ci++) {
        int colIdx = scrollCol + ci;
        if (colIdx < 0 || colIdx >= (int)data.cols.size()) continue;
        const auto &col = data.cols[colIdx];
        if (col.lineIdx < hStart.cy || col.lineIdx > hEnd.cy) continue;
        const auto &cells = data.cells[col.lineIdx];
        int x = vm.x + vm.w - vm.colAdvance - ci * vm.colAdvance;
        for (int i = col.start; i < col.end; i++) {
            int b = cells[i].start;
            bool geStart = col.lineIdx > hStart.cy || (col.lineIdx == hStart.cy && b >= hStart.cx);
            bool ltEnd = col.lineIdx < hEnd.cy || (col.lineIdx == hEnd.cy && b < hEnd.cx);
            if (geStart && ltEnd) {
                int row = i - col.start;
                u8g2_SetDrawColor(g_u8g2, 2);
                u8g2_DrawBox(g_u8g2, x, vm.y + row * vm.rowAdvance, cell, cell);
                u8g2_SetDrawColor(g_u8g2, 1);
            }
        }
    }
}

static void docOffsetToPos(int off, int &cy, int &cx) {
    int remain = off;
    for (int i = 0; i < (int)g_editor.lines.size(); i++) {
        int len = (int)g_editor.lines[i].length();
        if (remain <= len) { cy = i; cx = remain; return; }
        remain -= len + 1;
    }
    cy = (int)g_editor.lines.size() - 1;
    cx = (int)g_editor.lines.back().length();
}

static int posToDocOffset(int cy, int cx) {
    int off = 0;
    for (int i = 0; i < cy; i++) off += (int)g_editor.lines[i].length() + 1;
    off += cx;
    return off;
}

static void searchComputeMatches() {
    auto &sh = g_editor.search;
    sh.matches.clear();
    sh.cur = -1;
    if (sh.term.empty()) return;
    const std::string doc = currentEditorText();
    const std::string &t = sh.term;
    size_t pos = 0;
    while (pos < doc.length()) {
        size_t f = doc.find(t, pos);
        if (f == std::string::npos) break;
        sh.matches.push_back({(int)f, (int)(f + t.length())});
        pos = f + t.length();
    }
    if (!sh.matches.empty()) sh.cur = 0;
}

// 把编辑器光标定位到第 idx 个匹配(并滚动跟随),不清除编辑内容。
static void searchGotoMatch(int idx) {
    auto &sh = g_editor.search;
    if (idx < 0 || idx >= (int)sh.matches.size()) return;
    sh.cur = idx;
    int cy, cx;
    docOffsetToPos(sh.matches[idx].first, cy, cx);
    g_editor.cy = cy;
    g_editor.cx = cx;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    markDirty();
}

// 匹配已由 searchComputeMatches 计算好:定位到从 startOffset 起(含)的第一个
// 匹配,没有则回到第一个(环绕)。
static void searchRefindFrom(int startOffset) {
    auto &sh = g_editor.search;
    int n = (int)sh.matches.size();
    if (n == 0) { sh.cur = -1; return; }
    for (int i = 0; i < n; i++) {
        if (sh.matches[i].first >= startOffset) { searchGotoMatch(i); return; }
    }
    searchGotoMatch(0);
}

// 关键词变化后重算匹配并定位。
static void searchAfterTermChange() {
    searchComputeMatches();
    if (g_editor.search.matches.empty()) return;
    searchRefindFrom(posToDocOffset(g_editor.cy, g_editor.cx));
}

static void searchNextMatch() {
    auto &sh = g_editor.search;
    if (sh.matches.empty()) return;
    searchGotoMatch((sh.cur + 1) % (int)sh.matches.size());
}

static void searchPrevMatch() {
    auto &sh = g_editor.search;
    if (sh.matches.empty()) return;
    int n = (int)sh.matches.size();
    searchGotoMatch((sh.cur - 1 + n) % n);
}

// 用 replacement 替换文档 [start,end) 字节区间,光标移到替换文本之后。
static void applyDocReplace(int start, int end, const std::string &repl) {
    recordUndoSnapshot(UndoGroup::Structural);
    std::string doc = currentEditorText();
    std::string newDoc = doc.substr(0, start) + repl + doc.substr(end);
    loadLinesIntoEditor(newDoc);
    int off = start + (int)repl.length();
    int cy, cx;
    docOffsetToPos(off, cy, cx);
    g_editor.cy = cy; g_editor.cx = cx;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

static void searchReplaceCurrent() {
    auto &sh = g_editor.search;
    if (sh.matches.empty() || sh.cur < 0) return;
    auto &m = sh.matches[sh.cur];
    int start = m.first, end = m.second;
    applyDocReplace(start, end, sh.rep);
    searchAfterTermChange();  // 文档已变,重新计算匹配并定位
}

// 全部替换,返回替换次数。替换后重新定位匹配。
static int searchReplaceAll() {
    auto &sh = g_editor.search;
    if (sh.term.empty()) return 0;
    std::string doc = currentEditorText();
    const std::string &t = sh.term;
    const std::string &r = sh.rep;
    std::string out;
    size_t pos = 0;
    int count = 0;
    while (pos < doc.length()) {
        size_t f = doc.find(t, pos);
        if (f == std::string::npos) break;
        out += doc.substr(pos, f - pos);
        out += r;
        pos = f + t.length();
        count++;
    }
    out += doc.substr(pos);
    if (count == 0) {
        searchRefindFrom(posToDocOffset(g_editor.cy, g_editor.cx));
        return 0;
    }
    recordUndoSnapshot(UndoGroup::Structural);
    loadLinesIntoEditor(out);
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
    searchAfterTermChange();  // 文档已变,重新计算匹配并定位
    return count;
}

// ── 面板绘制 ──────────────────────────────────────────────────────────────

// 第 idx 条匹配所在行的文本及其高亮字节区间(跨行匹配只高亮到行尾)。
static std::string searchMatchContext(int idx, int &hlStart, int &hlEnd) {
    auto &sh = g_editor.search;
    auto &m = sh.matches[idx];
    int cyS, cxS; docOffsetToPos(m.first, cyS, cxS);
    int cyE, cxE; docOffsetToPos(m.second, cyE, cxE);
    std::string line = g_editor.lines[cyS];
    hlStart = cxS;
    hlEnd = (cyE == cyS) ? cxE : (int)line.length();
    return line;
}

// 截取包含 [hlStart,hlEnd) 且不超 maxW 的窗口;必要时前置/追加 "…"。
static std::string searchContextWindow(const std::string &line, int hlStart, int hlEnd,
                                       int maxW, int &outStart, int &outEnd) {
    int len = (int)line.length();
    if (g_font.textWidth(line.c_str()) <= maxW) {
        outStart = hlStart; outEnd = hlEnd;
        return line;
    }
    int ew = g_font.textWidth(ELLIPSIS);
    int budget = maxW - 2 * ew;
    if (budget < ew) budget = ew;
    int ws = hlStart, we = hlEnd;
    while (we < len) {
        int next = utf8Next(line, we);
        if (g_font.textWidth(line.substr(ws, next - ws).c_str()) > budget) break;
        we = next;
    }
    while (ws > 0) {
        int prev = utf8Prev(line, ws);
        if (g_font.textWidth(line.substr(prev, we - prev).c_str()) > budget) break;
        ws = prev;
    }
    std::string mid = line.substr(ws, we - ws);
    bool pre = ws > 0, post = we < len;
    std::string disp;
    int preLen = 0;
    if (pre) { disp += ELLIPSIS; preLen = 3; }
    disp += mid;
    if (post) disp += ELLIPSIS;
    outStart = preLen + (hlStart - ws);
    outEnd = preLen + (hlEnd - ws);
    return disp;
}

// 输入框文本窗口:保证光标可见,超宽时在光标两侧截断。
static void searchFieldView(const std::string &s, int cur, int maxW,
                            std::string &disp, int &dispCur) {
    if (g_font.textWidth(s.c_str()) <= maxW) { disp = s; dispCur = cur; return; }
    int len = (int)s.length();
    int ws = cur, we = cur;
    while (we < len) {
        int next = utf8Next(s, we);
        if (g_font.textWidth(s.substr(ws, next - ws).c_str()) > maxW) break;
        we = next;
    }
    while (ws > 0) {
        int prev = utf8Prev(s, ws);
        if (g_font.textWidth(s.substr(prev, we - prev).c_str()) > maxW) break;
        ws = prev;
    }
    disp = s.substr(ws, we - ws);
    dispCur = cur - ws;
}

// 绘制第 idx 条匹配:整行墨色文字,命中词 XOR 反显;当前匹配整行反显。
static void drawSearchMatchLine(int idx, int y, bool isCurrent) {
    int hlS = 0, hlE = 0;
    std::string line = searchMatchContext(idx, hlS, hlE);
    int outS = 0, outE = 0;
    std::string disp = searchContextWindow(line, hlS, hlE, SCREEN_W - 8, outS, outE);
    g_font.drawText(4, y, disp.c_str());
    if (isCurrent) {
        u8g2_SetDrawColor(g_u8g2, 2);  // XOR: 整行反显标记当前匹配
        u8g2_DrawBox(g_u8g2, 0, y - g_font.ascent(), SCREEN_W, FONT_H);
    } else {
        int midX = 4 + g_font.textWidth(disp.substr(0, outS).c_str());
        int midW = g_font.textWidth(disp.substr(outS, outE - outS).c_str());
        u8g2_SetDrawColor(g_u8g2, 2);  // XOR: 反显命中词
        u8g2_DrawBox(g_u8g2, midX, y - g_font.ascent(), midW, FONT_H);
    }
    u8g2_SetDrawColor(g_u8g2, 0);  // 恢复墨色
}

static void drawSearchPanel() {
    g_editor.drawnOnce = true;
    ui_clear();
    auto &sh = g_editor.search;
    const int rowH = LINE_SPACING;

    // 标题行 + 匹配信息
    ui_draw_text(4, FONT_H, "查找/替换", false, true);
    std::string info;
    if (sh.term.empty()) info = "输入关键词";
    else if (sh.matches.empty()) info = "未找到";
    else info = std::to_string(sh.cur + 1) + "/" + std::to_string((int)sh.matches.size());
    ui_draw_text(SCREEN_W - 4 - g_font.textWidth(info.c_str()), FONT_H, info.c_str());

    // 查找字段
    {
        int y = FONT_H + rowH;
        ui_draw_text(4, y, "查找:");
        int tx = 4 + g_font.textWidth("查找:");
        std::string field = sh.term;
        for (auto &c : field) if (c == '\n') c = ' ';  // '\n' 同为1字节,光标偏移不变
        std::string disp; int dispCur;
        searchFieldView(field, sh.termCur, SCREEN_W - 8 - (tx - 4), disp, dispCur);
        g_font.drawText(tx, y, disp.c_str());
        if (!sh.focusRep) {
            int cx = tx + g_font.textWidth(disp.substr(0, dispCur).c_str());
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, cx, y + 4, 8, 3);
            u8g2_SetDrawColor(g_u8g2, 0);
        }
    }
    // 替换字段
    {
        int y = FONT_H + 2 * rowH;
        ui_draw_text(4, y, "替换:");
        int tx = 4 + g_font.textWidth("替换:");
        std::string disp; int dispCur;
        searchFieldView(sh.rep, sh.repCur, SCREEN_W - 8 - (tx - 4), disp, dispCur);
        g_font.drawText(tx, y, disp.c_str());
        if (sh.focusRep) {
            int cx = tx + g_font.textWidth(disp.substr(0, dispCur).c_str());
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, cx, y + 4, 8, 3);
            u8g2_SetDrawColor(g_u8g2, 0);
        }
    }
    // 分隔线:区分输入区与匹配区
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 3 * rowH - 4, SCREEN_W);

    // 匹配区:一行一条匹配,显示上下文并反显命中词;当前匹配整行反显
    {
        int y = FONT_H + 4 * rowH;
        int total = (int)sh.matches.size();
        if (sh.term.empty()) {
            g_font.drawText(4, y, "输入关键词");
        } else if (total == 0) {
            g_font.drawText(4, y, "未找到匹配");
        } else {
            const int show = 4;
            int winStart;
            if (total <= show) {
                winStart = 0;
            } else {
                winStart = sh.cur - (show - 1);
                if (winStart < 0) winStart = 0;
                if (winStart + show > total) winStart = total - show;
            }
            for (int li = 0; li < show; li++) {
                int idx = winStart + li;
                if (idx >= total) break;
                drawSearchMatchLine(idx, y + li * rowH, idx == sh.cur);
            }
        }
    }

    // 状态栏
    std::string imeLabel = imeStatusLabel(sh.imeActive);
    std::string right = imeLabel;
    std::string bt = battery_icon_status_text();
    if (!bt.empty()) right += " " + bt;
    ui_draw_status(sh.focusRep ? "替换字段" : "查找字段", right.c_str());

    if (sh.imeActive && g_ime.composing()) drawIMEUIFullscreen();
    ui_commit();
}

// ── 开/关与按键处理 ───────────────────────────────────────────────────────

static void searchOpen() {
    auto &sh = g_editor.search;
    if (g_editor.hasSelection) {
        sh.term = getSelectedText();  // 选中文本作为关键词(可含换行)
        TextPos start, end;
        getSelRange(start, end);
        g_editor.cy = start.cy;  // 光标回到选区起点,首个匹配即选中文本
        g_editor.cx = start.cx;
        clearSelection();
        g_editor.targetCx = -1;
    }
    sh.termCur = (int)sh.term.length();
    sh.focusRep = false;
    sh.repCur = (int)sh.rep.length();
    sh.imeActive = false;
    g_ime.setActive(false);  // 取消编辑器输入法组合,对话框默认英文
    sh.active = true;
    searchAfterTermChange();
}

static void searchClose() {
    auto &sh = g_editor.search;
    sh.active = false;
    sh.imeActive = false;
    sh.matches.clear();
    sh.cur = -1;
    g_ime.setActive(g_editor.imeActive);  // 恢复编辑器输入法状态
    if (g_editor.imeActive) g_ime.setDocumentContext(editorImeContextText());
}

static void searchInsertFocused(const std::string &ins) {
    auto &sh = g_editor.search;
    if (sh.focusRep) { sh.rep.insert(sh.repCur, ins); sh.repCur += (int)ins.length(); }
    else { sh.term.insert(sh.termCur, ins); sh.termCur += (int)ins.length(); }
}

static void searchBackspaceFocused() {
    auto &sh = g_editor.search;
    if (sh.focusRep) {
        if (sh.repCur > 0) { int prev = utf8Prev(sh.rep, sh.repCur); sh.rep.erase(prev, sh.repCur - prev); sh.repCur = prev; }
    } else {
        if (sh.termCur > 0) { int prev = utf8Prev(sh.term, sh.termCur); sh.term.erase(prev, sh.termCur - prev); sh.termCur = prev; }
    }
}

static void searchMoveFocusedLeft() {
    auto &sh = g_editor.search;
    if (sh.focusRep) { if (sh.repCur > 0) sh.repCur = utf8Prev(sh.rep, sh.repCur); }
    else { if (sh.termCur > 0) sh.termCur = utf8Prev(sh.term, sh.termCur); }
}

static void searchMoveFocusedRight() {
    auto &sh = g_editor.search;
    if (sh.focusRep) { if (sh.repCur < (int)sh.rep.length()) sh.repCur = utf8Next(sh.rep, sh.repCur); }
    else { if (sh.termCur < (int)sh.term.length()) sh.termCur = utf8Next(sh.term, sh.termCur); }
}

static void drawEditor();

static AppState screen_editor_search_handle(int key, ScreenContext &ctx) {
    auto &sh = g_editor.search;

    // 对话框内输入法(与编辑器共用 g_ime)
    if (sh.imeActive && key != 0) {
        std::string imeOut;
        if (g_ime.handleKey(key, imeOut)) {
            if (!imeOut.empty()) {
                searchInsertFocused(imeOut);
                if (!sh.focusRep) searchAfterTermChange();
            }
            drawSearchPanel();
            return APP_EDITOR;
        }
    }

    if (key == KEY_SEARCH || key == 0x1B) {
        // 关闭时把当前匹配设为选区:正文立即反显匹配位置(横竖排共用选区高亮),
        // 不然对话框全程盖住正文,关掉后匹配处只有光标,难以定位。
        auto &sh2 = g_editor.search;
        if (sh2.cur >= 0 && sh2.cur < (int)sh2.matches.size()) {
            int cy, cx;
            docOffsetToPos(sh2.matches[sh2.cur].first, cy, cx);
            g_editor.selAnchorCy = cy;
            g_editor.selAnchorCx = cx;
            docOffsetToPos(sh2.matches[sh2.cur].second, cy, cx);
            g_editor.cy = cy;
            g_editor.cx = cx;
            g_editor.hasSelection = true;
            g_editor.targetCx = -1;
            markDirty();
        }
        searchClose();
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == KEY_IME_TOGGLE) {
        sh.imeActive = !sh.imeActive;
        g_ime.setActive(sh.imeActive);
    } else if (key == KEY_FULLWIDTH_TOGGLE) {
        g_ime.toggleFullwidth();
    } else if (key == KEY_TRAD_TOGGLE) {
        g_ime.toggleTrad();
    } else if (key == KEY_LSHIFT_TAP) {
        g_ime.toggleEnglish();
    } else if (key == 0x09) {  // Tab: 切换字段
        sh.focusRep = !sh.focusRep;
    } else if (key == 0x0A || key == 0x0D || key == KEY_DOWN) {  // 下一处
        searchNextMatch();
    } else if (key == KEY_CTRL_ENTER || key == KEY_UP) {  // 上一处
        searchPrevMatch();
    } else if (key == 0x12) {  // Ctrl+R: 替换当前
        searchReplaceCurrent();
    } else if (key == 0x01) {  // Ctrl+A: 全部替换
        int n = searchReplaceAll();
        ctx.statusMessage = (n > 0) ? ("已替换 " + std::to_string(n) + " 处") : "未找到匹配";
        ctx.statusDuration = 30;
    } else if (key == 0x7F || key == 0x08) {  // Backspace
        searchBackspaceFocused();
        if (!sh.focusRep) searchAfterTermChange();
    } else if (key == KEY_LEFT) {
        searchMoveFocusedLeft();
    } else if (key == KEY_RIGHT) {
        searchMoveFocusedRight();
    } else if (key == KEY_HOME) {
        if (sh.focusRep) sh.repCur = 0; else sh.termCur = 0;
    } else if (key == KEY_END) {
        if (sh.focusRep) sh.repCur = (int)sh.rep.length(); else sh.termCur = (int)sh.term.length();
    } else if (key >= 0x20 && key <= 0x7E) {
        searchInsertFocused(std::string(1, (char)key));
        if (!sh.focusRep) searchAfterTermChange();
    }

    drawSearchPanel();
    return APP_EDITOR;
}

// ── 快捷键帮助对话框 (Ctrl+?) ─────────────────────────────────────────────
// 每行一条快捷键。含查找/替换对话框内的快捷键(见前4行)。
static const char *HELP_LINES[] = {
    "Ctrl+? 开关本帮助  Esc关闭",
    "Ctrl+/ 查找/替换",
    "  Enter下一处 Ctrl+Enter上一处",
    "  Tab切字段 Ctrl+R替换当前",
    "  Ctrl+A全部替换",
    "Ctrl+A全选  Ctrl+C复制",
    "Ctrl+X剪切  Ctrl+V粘贴",
    "Ctrl+Z撤销  Ctrl+R重做",
    "Ctrl+S保存  Ctrl+Q退出",
    "Ctrl+O润色选区",
    "Ctrl+T折叠/展开标题",
    "Ctrl+I灵感面板",
    "Ctrl+F发送Flomo",
    "Ctrl+Y历史版本",
    "Ctrl+N/P快捷编辑文件",
    "Ctrl+Space输入法开关",
    "Ctrl+D删除用户词",
    "Shift+Space全半角切换",
    "Ctrl+Shift+F简繁",
    "左Shift临时英文",
    "Home/End 行首/行尾",
    "PgUp/PgDn 翻页",
    "双击BOOT全文润色",
    "双击USER语音听写",
    "Ctrl+0-9快捷编辑文件切换",
    "-- Vi 模式 --",
    "i a I A o O 进入插入  Esc回普通",
    "hjkl w b e 0 ^ $ gg G 移动",
    "d c y > < 操作符  dd yy cc 整行",
    "x X s S D C Y r J ~ 改字",
    "v V 可视  y d c > < ~ p 选区操作",
    "p P 粘贴  u Ctrl+R 撤销重做",
    "/ ? n N 搜索  :w :q :wq :x 命令",
    "命令栏 Ctrl+Space 临时开输入法",
};
static const int HELP_COUNT = (int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0]));

// 帮助可见行数(标题下到状态栏之间)。
static int helpMaxVis() {
    return (STATUS_Y - FONT_H - LINE_SPACING + LINE_SPACING - 1) / LINE_SPACING;
}

static void drawHelpPanel() {
    g_editor.drawnOnce = true;
    ui_clear();
    const int rowH = LINE_SPACING;
    int maxVis = helpMaxVis();
    int maxScroll = HELP_COUNT - maxVis;
    if (maxScroll < 0) maxScroll = 0;
    if (g_editor.helpScroll > maxScroll) g_editor.helpScroll = maxScroll;

    ui_draw_text(4, FONT_H, "快捷键帮助", false, true);
    std::string info = std::to_string(g_editor.helpScroll + 1) + "/" + std::to_string(HELP_COUNT);
    ui_draw_text(SCREEN_W - 4 - g_font.textWidth(info.c_str()), FONT_H, info.c_str());

    for (int i = 0; i < maxVis && (g_editor.helpScroll + i) < HELP_COUNT; i++) {
        ui_draw_text(4, FONT_H + rowH + i * rowH, HELP_LINES[g_editor.helpScroll + i]);
    }

    ui_draw_status("Up/Down滚动 PgUp/PgDn翻页 Esc关闭", "");
    ui_commit();
}

static AppState screen_editor_help_handle(int key, ScreenContext &ctx) {
    (void)ctx;
    auto &g = g_editor;
    int maxVis = helpMaxVis();
    int maxScroll = HELP_COUNT - maxVis;
    if (maxScroll < 0) maxScroll = 0;
    if (key == KEY_HELP || key == 0x1B) {  // Ctrl+? 或 Esc 关闭
        g.helpActive = false;
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == KEY_DOWN || key == 0x0A || key == 0x0D) {
        if (g.helpScroll < maxScroll) g.helpScroll++;
    } else if (key == KEY_UP) {
        if (g.helpScroll > 0) g.helpScroll--;
    } else if (key == KEY_PAGE_DOWN) {
        g.helpScroll += maxVis;
        if (g.helpScroll > maxScroll) g.helpScroll = maxScroll;
    } else if (key == KEY_PAGE_UP) {
        g.helpScroll -= maxVis;
        if (g.helpScroll < 0) g.helpScroll = 0;
    }
    drawHelpPanel();
    return APP_EDITOR;
}

// ── vi 绘制辅助 ──────────────────────────────────────────────────────────
static char s_viMsg[160] = {0};
static int64_t s_viMsgUntil = 0;

static const char *viModeName() {
    switch (g_editor.vi.mode) {
        case ViMode::Normal: return "NORMAL";
        case ViMode::Visual: return "VISUAL";
        case ViMode::VisualLine: return "VISUAL LINE";
        case ViMode::Insert: return "INSERT";
    }
    return "NORMAL";
}

// 待决状态(计数/操作符/寄存器/g/f)拼成状态栏后缀, 便于确认键序。
static std::string viPendingLabel() {
    const ViState &v = g_editor.vi;
    std::string s;
    if (v.count > 0) s += std::to_string(v.count);
    switch (v.op) {
        case ViOperator::Delete: s += 'd'; break;
        case ViOperator::Change: s += 'c'; break;
        case ViOperator::Yank: s += 'y'; break;
        case ViOperator::IndentRight: s += '>'; break;
        case ViOperator::IndentLeft: s += '<'; break;
        case ViOperator::ToggleCase: s += '~'; break;
        default: break;
    }
    if (v.wantRegister) { s += '"'; if (v.reg) s += v.reg; }
    if (v.gPending) s += 'g';
    if (v.findCmd) s += v.findCmd;
    if (v.replacePending) s += 'r';
    return s;
}

static std::string viStatusLeft() {
    if (s_viMsg[0] && esp_timer_get_time() < s_viMsgUntil) return s_viMsg;
    std::string s = std::string("-- ") + viModeName() + " --";
    std::string pend = viPendingLabel();
    if (!pend.empty()) s += " " + pend;
    return s;
}

// 字符 visual 的选区包含光标所在字符, 行 visual 选中整行; 供绘制与运算共用。
static void viVisualRange(TextPos &start, TextPos &end) {
    TextPos a = {g_editor.selAnchorCy, g_editor.selAnchorCx};
    TextPos c = {g_editor.cy, g_editor.cx};
    if (g_editor.vi.mode == ViMode::VisualLine) {
        int ay = std::min(a.cy, c.cy), by = std::max(a.cy, c.cy);
        if (ay < 0) ay = 0;
        if (by >= (int)g_editor.lines.size()) by = (int)g_editor.lines.size() - 1;
        start = {ay, 0};
        end = {by, by >= 0 ? (int)g_editor.lines[by].length() : 0};
        return;
    }
    if (posLess(c, a)) { start = c; end = a; }
    else { start = a; end = c; }
    if (end.cy >= 0 && end.cy < (int)g_editor.lines.size() &&
        end.cx < (int)g_editor.lines[end.cy].length())
        end.cx = utf8Next(g_editor.lines[end.cy], end.cx);
}

static void viDrawCommandBar() {
    int y = STATUS_BAR_Y;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, y + 1, SCREEN_W, FONT_H + 3);
    u8g2_SetDrawColor(g_u8g2, 0);
    int textY = y + 1 + (STATUS_BAR_H - g_font.lineHeight()) / 2 + g_font.ascent();

    std::string full = std::string(1, g_editor.vi.cmdlineType) + g_editor.vi.cmdline;
    int curAbs = 1 + g_editor.vi.cmdlineCur;
    if (curAbs < 0) curAbs = 0;
    if (curAbs > (int)full.length()) curAbs = (int)full.length();
    // 超宽时丢前导字符, 保证看到尾部与光标
    int maxW = SCREEN_W - 8 - g_font.halfAdvance();
    size_t drop = 0;
    while (drop < full.length() &&
           g_font.textWidth(full.substr(drop).c_str()) > maxW) {
        drop = utf8Next(full, (int)drop);
    }
    std::string vis = full.substr(drop);
    int curVis = (int)(curAbs - (int)drop);
    if (curVis < 0) curVis = 0;
    if (curVis > (int)vis.length()) curVis = (int)vis.length();
    g_font.drawText(4, textY, vis.c_str());
    int curX = 4 + g_font.textWidth(vis.substr(0, curVis).c_str());
    int cw = g_font.halfAdvance();
    if (curVis < (int)vis.length()) {
        const char *cp = vis.c_str() + curVis;
        std::string one;
        unsigned char b = (unsigned char)*cp;
        if (b < 0x80) one = vis.substr(curVis, 1);
        else if ((b & 0xE0) == 0xC0) one = vis.substr(curVis, 2);
        else if ((b & 0xF0) == 0xE0) one = vis.substr(curVis, 3);
        else if ((b & 0xF8) == 0xF0) one = vis.substr(curVis, 4);
        if (!one.empty()) cw = g_font.textWidth(one.c_str());
    }
    u8g2_DrawFrame(g_u8g2, curX, y + 2, cw, FONT_H);
    u8g2_SetDrawColor(g_u8g2, 1);
}

// ── Editor drawing ────────────────────────────────────────────────────────
// s_skipStatusBarAndIme 定义在文件顶部,供行号槽与 drawEditor 共用。

// 光标落入折叠区时自动展开最近的折叠标题。放 drawEditor 入口覆盖所有重绘路径
// (主循环尾部、搜索跳转、Ctrl+X/V 早退、idle)。
static void reconcileFoldsForCursor() {
    if (g_editor.foldedHeadings.empty()) return;
    int cy = g_editor.cy;
    const auto &mi = getMdInfo(g_settings.markdownRender());
    if (cy < 0 || cy >= (int)mi.size()) return;
    int hideLevel = 0, foldAt = -1;
    for (int li = 0; li <= cy; li++) {
        int lvl = mi[li].headingLevel;
        bool isH = lvl > 0 && !mi[li].inCodeBlock;
        if (hideLevel != 0 && !(isH && lvl <= hideLevel)) continue;
        if (isH) {
            if (g_editor.foldedHeadings.count(li)) { hideLevel = lvl; foldAt = li; }
            else { hideLevel = 0; foldAt = -1; }
        }
    }
    // foldAt == cy 时光标在折叠标题行本身(可见边界),不算落入折叠区
    if (hideLevel != 0 && foldAt >= 0 && foldAt != cy) {
        g_editor.foldedHeadings.erase(foldAt);
        g_editor.vrowsDirty = true;
    }
}

// 提示词表头折行范围。drawEditor 与 editorVerticalVm 共用,保证表头占用的
// 行数在绘制与竖排布局计算之间一致。
static std::vector<std::pair<const char *, const char *>> promptWrappedRowRanges() {
    std::vector<std::pair<const char *, const char *>> rows;
    if (!g_editor.promptMode || g_editor.promptText.empty()) return rows;
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
        if (p > rowStart) rows.push_back({rowStart, p});
    }
    return rows;
}

// 竖排布局度量。draw 与左右/PageUp/PageDown 导航必须用同一套参数,否则
// 导航按 STATUS_Y 全高切列、绘制按候选条保留高度切列,列边界错位导致光标跳行。
static VerticalLayoutMetrics editorVerticalVm() {
    int y = FONT_H;
    int pRows = (int)promptWrappedRowRanges().size();
    if (pRows > 0) y += (pRows + 1) * LINE_SPACING;
    bool reserveIME = !s_skipStatusBarAndIme &&
        (g_editor.imeActive || (g_editor.vi.cmdlineActive && g_editor.vi.cmdlineIme));
    int contentEndY;
    if (reserveIME) {
        // 候选条底边锚定分割线(276)后,编码行白框上沿 = 276-2*FONT_H-11
        // (18pt:229 / 22pt:221)。竖排正文下探到编码行上沿附近,
        // 行数 18/22pt = 11/9(锚定 STATUS_Y 时为 10/8)。
        contentEndY = g_font.fontSize() == 18 ? 232 : 221;
    } else {
        contentEndY = STATUS_Y;
    }
    // 竖排首字墨迹顶边与横排首行对齐(横排首行基线 y,顶边 y-ascent);
    // 竖排基线 = vm.y + ascent,故 vm.y 取 y-ascent,顶部不留整行空白
    int vTop = y - g_font.ascent();
    return verticalMetrics(6, vTop, SCREEN_W - 12, contentEndY - vTop);
}

static void drawEditor() {
    g_editor.drawnOnce = true;
#if PJOURNAL_IME_PERF_LOG
    s_perfDrawStart = EDITOR_PERF_NOW();
    s_perfLayoutEnd = s_perfDrawStart;
#endif
    reconcileFoldsForCursor();
    int y = FONT_H;

    if (g_editor.promptMode && !g_editor.promptText.empty()) {
        for (auto &r : promptWrappedRowRanges()) {
            ui_draw_text(4, y, std::string(r.first, r.second - r.first).c_str(), false, true);
            y += LINE_SPACING;
        }
        u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
        y += LINE_SPACING;
    }

    if (editorVertical()) {
        bool composing = !s_skipStatusBarAndIme &&
        (g_editor.imeActive || (g_editor.vi.cmdlineActive && g_editor.vi.cmdlineIme)) &&
        g_ime.composing();
        VerticalLayoutMetrics vm = editorVerticalVm();
        bool mdOn = g_settings.markdownRender();
        mdSetRenderEnabled(mdOn);
        const std::vector<MdLineInfo> &mdInfo = getMdInfo(mdOn);
        auto hidden = mdFoldHiddenLines(g_editor.lines, mdOn ? &mdInfo : nullptr,
                                        &g_editor.foldedHeadings);
        auto data = buildVerticalData(g_editor.lines, vm.rows, &hidden,
                                      mdOn ? &mdInfo : nullptr, &g_editor.foldedHeadings,
                                      g_editor.cy, g_editor.cx);
        int cursorCol = verticalFindCol(data, g_editor.lines, g_editor.cy, g_editor.cx);
        if (editorTypewriter() && cursorCol >= 0) {
            g_editor.scroll = cursorCol - vm.cols / 2;
        } else {
            if (cursorCol < g_editor.scroll) g_editor.scroll = cursorCol;
            if (cursorCol >= g_editor.scroll + vm.cols) g_editor.scroll = cursorCol - vm.cols + 1;
            if (g_editor.scroll < 0) g_editor.scroll = 0;
        }
        int maxScroll = (int)data.cols.size() - vm.cols;
        if (maxScroll < 0) maxScroll = 0;
        if (!editorTypewriter()) {
            if (g_editor.scroll > maxScroll) g_editor.scroll = maxScroll;
        }

        if (composing) drawIMEUIWithStatusBar();
        VerticalGuideStyle guideStyle = VerticalGuideStyle::Solid;
        std::string guideStyleKey = g_settings.verticalReferenceLineStyle();
        if (guideStyleKey == "dash") guideStyle = VerticalGuideStyle::Dash;
        else if (guideStyleKey == "dot") guideStyle = VerticalGuideStyle::Dot;
        drawVerticalCols(g_editor.lines, data, g_editor.scroll, vm,
                         g_settings.verticalReferenceLine(), guideStyle);
        // Markdown 关闭时折叠标题行末补折叠标志;开启时 mdVerticalCells 已含折叠标志格
        if (!mdOn) {
            for (int li : g_editor.foldedHeadings) {
                if (li < 0 || li >= (int)g_editor.lines.size()) continue;
                int colIdx = -1;
                for (int i = 0; i < (int)data.cols.size(); i++)
                    if (data.cols[i].lineIdx == li) colIdx = i;
                if (colIdx < 0 || colIdx < g_editor.scroll || colIdx >= g_editor.scroll + vm.cols)
                    continue;
                const auto &col = data.cols[colIdx];
                if (col.end != (int)data.cells[li].size()) continue;
                int row = col.end - col.start;
                if (row >= vm.rows) continue;
                int ci = colIdx - g_editor.scroll;
                int x = vm.x + vm.w - vm.colAdvance - ci * vm.colAdvance;
                int wpx = g_font.textWidth(kFoldMarker);
                g_font.drawText(x + (g_font.lineHeight() - wpx) / 2,
                                vm.y + row * vm.rowAdvance + g_font.ascent(),
                                kFoldMarker, false);
            }
        }
        if (g_settings.viMode() &&
            (g_editor.vi.mode == ViMode::Visual || g_editor.vi.mode == ViMode::VisualLine)) {
            TextPos selStart, selEnd;
            viVisualRange(selStart, selEnd);
            drawVerticalHighlight(data, g_editor.scroll, vm, selStart, selEnd);
        } else if (g_editor.hasSelection) {
            TextPos selStart, selEnd;
            getSelRange(selStart, selEnd);
            drawVerticalHighlight(data, g_editor.scroll, vm, selStart, selEnd);
        }
        bool viBlockCursor = g_settings.viMode() && g_editor.vi.mode != ViMode::Insert;
        drawVerticalCursor(g_editor.lines, data, g_editor.scroll, vm, g_editor.cy, g_editor.cx,
                           viBlockCursor);

        if (!s_skipStatusBarAndIme) {
            if (g_settings.viMode() && g_editor.vi.cmdlineActive) {
                viDrawCommandBar();
            } else {
                int wc = getWordCount();
                std::string left, right;
                if (g_settings.viMode()) {
                    left = viStatusLeft();
                    const std::string &curLine = g_editor.lines[g_editor.cy];
                    int col = utf8Count(curLine.substr(0, std::min(g_editor.cx, (int)curLine.length()))) + 1;
                    right = std::to_string(g_editor.cy + 1) + ":" + std::to_string(col) +
                            " " + std::to_string(wc) + "字";
                } else {
                    char lbuf[48];
                    if (inQuickFileSession()) snprintf(lbuf, sizeof(lbuf), "[%d] 竖排", quickEditIndex());
                    else if (inFileEditSession()) snprintf(lbuf, sizeof(lbuf), "%s 竖排", fileEditBaseName(fileEditCurrentPath()).c_str());
                    else snprintf(lbuf, sizeof(lbuf), "%s 竖排", editorStatusTitle());
                    left = lbuf;
                    right = std::to_string(wc) + "字 " + imeStatusLabel(g_editor.imeActive);
                }
                std::string bt = battery_icon_status_text();
                if (!bt.empty()) right += " " + bt;
                ui_draw_status(left.c_str(), right.c_str());
            }
        }
        return;
    }

    const auto& vrows = getVrows();
#if PJOURNAL_IME_PERF_LOG
    // getVrows() 内部就是 buildVrows(全篇重排) + getMdInfo(全篇分类),
    // 是 drawEditor 里唯一 O(正文长度) 的部分, 单独标出来。
    s_perfLayoutEnd = EDITOR_PERF_NOW();
#endif
    bool composing = !s_skipStatusBarAndIme &&
        (g_editor.imeActive || (g_editor.vi.cmdlineActive && g_editor.vi.cmdlineIme)) &&
        g_ime.composing();
    // IME 开启期间恒定保留候选条区域,选字后候选条隐藏不再引起正文重排跳动
    bool reserveIME = !s_skipStatusBarAndIme &&
        (g_editor.imeActive || (g_editor.vi.cmdlineActive && g_editor.vi.cmdlineIme));
    int contentEndY = reserveIME ? imeStatusPanelTopY() : STATUS_Y;
    bool mdOn = g_settings.markdownRender();
    mdSetRenderEnabled(mdOn);
    const std::vector<MdLineInfo> &mdInfo = getMdInfo(mdOn);
    const int normalHeight = STATUS_Y - y;       // 无候选条时的可视高度
    const int visibleHeight = contentEndY - y;   // 当前可视高度

    int cursorVR = -1;
    for (int vi = 0; vi < (int)vrows.size(); vi++) {
        if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
            cursorVR = vi;
            break;
        }
    }

    if (editorTypewriter() && cursorVR >= 0) {
        // 打字机模式:光标钉在可视区中间。居中按「未弹出候选区」的高度计算,
        // 段落间距计入,滚动与 IME 候选面板的弹出/消失无关;scroll 允许为负,
        // 文首/短文档由负 slot 补足顶部留白(绘制循环跳过越界下标)。
        g_editor.scroll = vrowScrollCenter(vrows, cursorVR, normalHeight, &mdInfo);
        // 极端小屏兜底:候选区几乎占满内容区时,保证光标仍落在候选条上方的可视行,
        // 不因居中而藏到候选面板之下。
        if (composing && vrowOffsetFrom(vrows, g_editor.scroll, cursorVR, &mdInfo) >=
                             visibleHeight)
            g_editor.scroll = vrowScrollToShow(vrows, cursorVR, visibleHeight, &mdInfo);
    } else {
        if (cursorVR < g_editor.scroll) g_editor.scroll = cursorVR;
        else if (cursorVR >= 0 &&
                 vrowOffsetFrom(vrows, g_editor.scroll, cursorVR, &mdInfo) >= visibleHeight)
            g_editor.scroll = vrowScrollToShow(vrows, cursorVR, visibleHeight, &mdInfo);
        if (g_editor.scroll < 0) g_editor.scroll = 0;
    }

    int visibleVrows = vrowFitCount(vrows, g_editor.scroll, visibleHeight, &mdInfo);
    if (visibleVrows < 1) visibleVrows = 1;
    // 负 scroll 时首行不落在 y,而是下方 lead*LINE_SPACING 处
    const int drawBase = g_editor.scroll < 0 ? 0 : g_editor.scroll;
    int textX = editorTextX();
    bool gutter = editorLineNumbersOn();
    int gutterDigits = editorGutterDigits();
    int ly = y + (drawBase - g_editor.scroll) * LINE_SPACING;
    int prevDrawn = -1;
    for (int i = 0; i < visibleVrows; i++) {
        int idx = g_editor.scroll + i;
        if (idx < 0 || idx >= (int)vrows.size()) continue;  // 打字机留白行
        if (prevDrawn >= 0) ly += LINE_SPACING + vrowGapBefore(vrows, idx, &mdInfo);
        prevDrawn = idx;
        auto &vr = vrows[idx];
        // 行号只标在硬行首行的视觉行, 续行留空; 右对齐在槽内
        if (gutter && vr.start == 0) {
            char num[16];
            snprintf(num, sizeof(num), "%d", vr.lineIdx + 1);
            int nd = (int)strlen(num);
            int nx = 4 + (gutterDigits - nd) * g_font.halfAdvance();
            g_font.drawText(nx, ly, num);
        }
        int mdCursor = (vr.lineIdx == g_editor.cy) ? g_editor.cx : -1;
        bool folded = !g_editor.foldedHeadings.empty() &&
                      g_editor.foldedHeadings.count(vr.lineIdx);
        mdDrawVrow(textX, ly, g_editor.lines[vr.lineIdx], vr.start, vr.end,
                   mdInfo[vr.lineIdx], vr.indentCells, mdCursor, folded);
    }

    // Selection highlight
    bool viVisual = g_settings.viMode() &&
        (g_editor.vi.mode == ViMode::Visual || g_editor.vi.mode == ViMode::VisualLine);
    if (g_editor.hasSelection || viVisual) {
        TextPos selStart, selEnd;
        if (viVisual) viVisualRange(selStart, selEnd);
        else getSelRange(selStart, selEnd);
        int selLy = y + (drawBase - g_editor.scroll) * LINE_SPACING;
        int prevSel = -1;
        for (int i = 0; i < visibleVrows; i++) {
            int vrIdx = g_editor.scroll + i;
            if (vrIdx < 0 || vrIdx >= (int)vrows.size()) continue;  // 打字机留白行
            if (prevSel >= 0) selLy += LINE_SPACING + vrowGapBefore(vrows, vrIdx, &mdInfo);
            prevSel = vrIdx;
            auto &vr = vrows[vrIdx];
            int lineIdx = vr.lineIdx;
            int rowStart = vr.start, rowEnd = vr.end;
            if (lineIdx < selStart.cy || lineIdx > selEnd.cy) continue;
            // Calculate overlap of [rowStart, rowEnd) with selection on this line
            int hlStart = rowStart, hlEnd = rowEnd;
            if (lineIdx == selStart.cy) hlStart = std::max(hlStart, selStart.cx);
            if (lineIdx == selEnd.cy) hlEnd = std::min(hlEnd, selEnd.cx);
            if (hlStart >= hlEnd) continue;
            // Highlight range [hlStart, hlEnd) on this vrow
            const MdLineInfo &mdi = mdInfo[lineIdx];
            bool folded = !g_editor.foldedHeadings.empty() &&
                          g_editor.foldedHeadings.count(lineIdx);
            int mdCursor = (lineIdx == g_editor.cy) ? g_editor.cx : -1;
            int xOff = textX + mdVrowX(g_editor.lines[lineIdx], mdi, hlStart, rowStart,
                                       vr.indentCells, mdCursor, folded);
            int selEndX = textX + mdVrowX(g_editor.lines[lineIdx], mdi, hlEnd, rowStart,
                                          vr.indentCells, mdCursor, folded);
            int selW = selEndX - xOff;
            int ly = selLy;
            u8g2_SetDrawColor(g_u8g2, 2);  // XOR mode
            u8g2_DrawBox(g_u8g2, xOff, ly - g_font.ascent(), selW, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 1);  // restore
        }
    }

    if (cursorVR >= 0 && cursorVR >= g_editor.scroll && cursorVR < g_editor.scroll + visibleVrows) {
        auto &vr = vrows[cursorVR];
        const std::string &line = g_editor.lines[vr.lineIdx];
        const MdLineInfo &mdi = mdInfo[vr.lineIdx];
        bool folded = !g_editor.foldedHeadings.empty() &&
                      g_editor.foldedHeadings.count(vr.lineIdx);
        int cx = textX + mdVrowX(line, mdi, g_editor.cx, vr.start, vr.indentCells,
                                 g_editor.cx, folded);
        int cy_draw = y + vrowOffsetFrom(vrows, g_editor.scroll, cursorVR, &mdInfo);
        int cw = g_font.halfAdvance();
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
        bool viBlock = g_settings.viMode() && g_editor.vi.mode != ViMode::Insert;
        u8g2_SetDrawColor(g_u8g2, 0);
        if (viBlock) {
            // normal/visual: 覆盖字符单元的框形光标
            u8g2_DrawFrame(g_u8g2, cx, cy_draw - g_font.ascent(), cw, FONT_H);
            if (cw > 2) u8g2_DrawFrame(g_u8g2, cx + 1, cy_draw - g_font.ascent() + 1,
                                       cw - 2, FONT_H - 2);
        } else {
            u8g2_DrawBox(g_u8g2, cx, cy_draw + 4, cw, 3);
        }
        u8g2_SetDrawColor(g_u8g2, 1);
    }

    if (composing) drawIMEUIWithStatusBar();

    if (!s_skipStatusBarAndIme) {
        if (g_settings.viMode() && g_editor.vi.cmdlineActive) {
            viDrawCommandBar();
            return;
        }
        int wc = getWordCount();
        std::string left, right;
        if (g_settings.viMode()) {
            left = viStatusLeft();
            const std::string &curLine = g_editor.lines[g_editor.cy];
            int col = utf8Count(curLine.substr(0, std::min(g_editor.cx, (int)curLine.length()))) + 1;
            right = std::to_string(g_editor.cy + 1) + ":" + std::to_string(col) +
                    " " + std::to_string(wc) + "字";
        } else {
            char lbuf[48];
            if (inQuickFileSession()) {
                snprintf(lbuf, sizeof(lbuf), "[%d]", quickEditIndex());
            } else if (inFileEditSession()) {
                snprintf(lbuf, sizeof(lbuf), "%s", fileEditBaseName(fileEditCurrentPath()).c_str());
            } else {
                snprintf(lbuf, sizeof(lbuf), "%s", editorStatusTitle());
            }
            left = lbuf;
            right = std::to_string(wc) + "字 " + imeStatusLabel(g_editor.imeActive);
        }
        std::string bt = battery_icon_status_text();
        if (!bt.empty()) right += " " + bt;

        ui_draw_status(left.c_str(), right.c_str());
    }
}

// Draw only the editor content (no IME strip, no status bar) so the voice
// dictation screen can show the editor as a live-transparent background.
void screen_editor_draw_voice_bg() {
    g_ime.cancelComposition();  // cancel any in-progress composition
    s_skipStatusBarAndIme = true;
    drawEditor();
    s_skipStatusBarAndIme = false;
}

static void drawConfirmDialog() {
    int bw = 280, bh = 120;
    int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2 - 20;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);
    ui_draw_text_centered(by + 28, "是否保存当前内容？");
    ui_draw_text_centered(by + 58, "Enter=保存");
    ui_draw_text_centered(by + 88, "ESC=放弃");
    u8g2_SetDrawColor(g_u8g2, 1);
}

static void drawRecoveryDialog() {
    int bw = 310, bh = 120;
    int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2 - 20;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);
    ui_draw_text_centered(by + 28, "发现未保存草稿");
    ui_draw_text_centered(by + 58, "Enter=恢复");
    ui_draw_text_centered(by + 88, "ESC=忽略");
    u8g2_SetDrawColor(g_u8g2, 1);
}

static void drawPromptGenerating() {
    ui_clear();
    ui_show_message_centered("AI生成提示中...");
    ui_draw_status("请稍候", "");
    ui_commit();
}

static void runPromptTask(void *arg) {
    (void)arg;
    bool wifiWasConnected = g_wifi.isConnected();
    DeepseekResult result = {false, ""};
    if (ensure_wifi_connected()) {
        result = g_deepseek.generatePrompt(s_promptTaskContext);
    } else {
        result = {false, "WiFi连接失败"};
    }
    restore_wifi_state(wifiWasConnected);
    lockPromptResult();
    s_promptTaskResult = result;
    unlockPromptResult();
    s_promptTaskDone = true;
    vTaskDelete(nullptr);
}

// ── Editor save helper ────────────────────────────────────────────────────
static bool saveCurrentContent(bool createHistory = true) {
    std::string text = currentEditorText();
    if (inQuickFileSession()) {
        // 快捷编辑: 直接写回 /sdcard/{n}.txt, 允许保存空文件
        bool ok = quickEditSave(quickEditIndex(), text, createHistory);
        if (ok) g_journal.clearRecoveryDraft();
        return ok;
    }
    if (inFileEditSession()) {
        // 文件编辑: 直接写回当前文件,允许保存空文件;默认自动保存。
        bool ok = fileEditSave(text);
        if (ok) g_journal.clearRecoveryDraft();
        return ok;
    }
    if (text.empty()) return false;

    time_t now; time(&now); struct tm *tm = localtime(&now);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    int wc = getWordCount();
    std::string headerStr; headerStr.resize(128);
    int hlen = snprintf(&headerStr[0], 128, "日期: %s\n字数: %d\n\n", ts, wc);
    headerStr.resize(hlen);
    std::string fullText;
    if (g_editor.promptMode)
        fullText = headerStr + "提示词: " + g_editor.promptText + "\n\n" + text;
    else
        fullText = headerStr + "自由写作\n\n" + text;

    if (g_editor.savedFilename.empty()) {
        char fname[32];
        strftime(fname, sizeof(fname), "%Y-%m-%d_%H%M%S", tm);
        g_editor.savedFilename = std::string(fname) + ".txt";
    }
    bool ok = g_journal.saveEntryRaw(g_editor.savedFilename, fullText, createHistory);
    if (ok) g_journal.clearRecoveryDraft();
    return ok;
}

static AppState finishEditor(ScreenContext &ctx) {
    g_editor.modifiedSinceSave = false;
    if (saveCurrentContent()) {
        ctx.nextState = ctx.prevState;
        return ctx.prevState;
    }
    ctx.statusMessage = "保存失败，请检查SD卡";
    ctx.nextState = ctx.prevState;
    return ctx.prevState;
}

// ── vi 模式实现 ──────────────────────────────────────────────────────────
enum class ViCharClass { Space, Word, Punct, Han };

struct ViMotion {
    bool ok = false;
    bool linewise = false;
    bool inclusive = false;
    bool keepCol = false;
    int cy = 0, cx = 0;
};

static void viSetMsg(const std::string &m) {
    size_t n = m.size();
    if (n > sizeof(s_viMsg) - 1) {
        n = sizeof(s_viMsg) - 1;
        // 不要从 UTF-8 多字节字符中间截断, 否则绘制端会读到半个码点。
        while (n > 0 && ((unsigned char)m[n] & 0xC0) == 0x80) n--;
    }
    memcpy(s_viMsg, m.data(), n);
    s_viMsg[n] = 0;
    s_viMsgUntil = esp_timer_get_time() + 2500000;
}

static bool viIsHan(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF);
}
static bool viIsSpaceCp(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x3000 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029;
}
static bool viIsCjkPunct(uint32_t cp) {
    return (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF20) ||
           (cp >= 0xFF3B && cp <= 0xFF65) || (cp >= 0x2018 && cp <= 0x201F) ||
           cp == 0x2026 || cp == 0x2014 || cp == 0x2013;
}
static ViCharClass viClassifyCp(uint32_t cp) {
    if (viIsSpaceCp(cp)) return ViCharClass::Space;
    if (cp >= 0x80 && viIsCjkPunct(cp)) return ViCharClass::Punct;
    if (viIsHan(cp)) return ViCharClass::Han;
    if (cp < 0x80) {
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
            (cp >= '0' && cp <= '9') || cp == '_')
            return ViCharClass::Word;
        return ViCharClass::Punct;
    }
    return ViCharClass::Word;
}
static ViCharClass viClassAt(const std::string &s, int pos) {
    if (pos < 0 || pos >= (int)s.length()) return ViCharClass::Space;
    const char *p = s.c_str() + pos;
    return viClassifyCp(FontRenderer::utf8Decode(p));
}
static bool viSpaceAt(const std::string &s, int pos) {
    return viClassAt(s, pos) == ViCharClass::Space;
}

static int viFirstNonBlank(const std::string &s) {
    int i = 0;
    while (i < (int)s.length() && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}
static int viLastCharPos(const std::string &s) {
    return s.empty() ? 0 : utf8Prev(s, (int)s.length());
}

static void viClampNormal() {
    if (g_editor.lines.empty()) g_editor.lines.push_back("");
    if (g_editor.cy < 0) g_editor.cy = 0;
    if (g_editor.cy >= (int)g_editor.lines.size())
        g_editor.cy = (int)g_editor.lines.size() - 1;
    const std::string &s = g_editor.lines[g_editor.cy];
    if (g_editor.cx < 0) g_editor.cx = 0;
    if (g_editor.cx > (int)s.length()) g_editor.cx = (int)s.length();
    // normal/visual 光标停在字符上, 不越过行尾
    if (g_editor.cx == (int)s.length() && !s.empty())
        g_editor.cx = utf8Prev(s, g_editor.cx);
}

static void viMarkChanged() {
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

// vi 的 count 没有上限时, 长按数字会在 int 上溢出(UB), 后续循环也失去意义。
static const int kViMaxCount = 999999;
static void viCountDigit(int &count, int digit) {
    if (count >= kViMaxCount) { count = kViMaxCount; return; }
    count = count * 10 + digit;
    if (count > kViMaxCount) count = kViMaxCount;
}
// 操作符 count 与 motion count 相乘, 同样夹在 int 安全范围内。
static int viCountMul(int a, int b) {
    long long v = (long long)a * (long long)b;
    if (v > kViMaxCount) v = kViMaxCount;
    if (v < 1) v = 1;
    return (int)v;
}

// ── 寄存器 ───────────────────────────────────────────────────────────────
static ViRegister &viRegSlot(char reg) {
    int i = (reg >= 'A' && reg <= 'Z') ? (reg - 'A') : (reg - 'a');
    if (i < 0) i = 0;
    if (i > 25) i = 25;
    return g_editor.vi.regs[i];
}
static bool viRegNamed(char reg) {
    return (reg >= 'a' && reg <= 'z') || (reg >= 'A' && reg <= 'Z');
}
static bool viRegAppend(char reg) { return reg >= 'A' && reg <= 'Z'; }

static bool viReadRegister(char reg, std::string &text, bool &linewise) {
    if (reg == 0 || reg == '+' || reg == '*' || (reg >= '0' && reg <= '9')) {
        text = g_clipboard;
        linewise = s_clipboardLinewise;
        return !text.empty();
    }
    if (viRegNamed(reg)) {
        ViRegister &r = viRegSlot(reg);
        text = r.text;
        linewise = r.linewise;
        return !text.empty();
    }
    text.clear();
    linewise = false;
    return false;
}
// 命名寄存器上限 32KB: 26 个寄存器各存一份大选区会成倍占用内部 RAM。
static const size_t kViRegMaxBytes = 32768;
static void viCapRegister(ViRegister &r) {
    if (r.text.size() <= kViRegMaxBytes) return;
    size_t n = kViRegMaxBytes;
    while (n > 0 && ((unsigned char)r.text[n] & 0xC0) == 0x80) n--;
    r.text.resize(n);
}

static void viWriteRegister(char reg, const std::string &text, bool linewise) {
    if (viRegNamed(reg)) {
        ViRegister &r = viRegSlot(reg);
        if (viRegAppend(reg) && !r.text.empty()) {
            r.text += text;
            r.linewise = (linewise && r.linewise) || (r.text.find('\n') == std::string::npos ? linewise : false);
        } else {
            r.text = text;
            r.linewise = linewise;
        }
        viCapRegister(r);
        // unnamed 寄存器仍保留完整文本, 命名寄存器超限只影响 "a 这类显式引用。
        g_clipboard = text;
        s_clipboardLinewise = linewise;
    } else {
        g_clipboard = text;
        s_clipboardLinewise = linewise;
    }
}
static std::string viLineRangeText(int a, int b) {
    std::string out;
    if (a < 0) a = 0;
    if (b >= (int)g_editor.lines.size()) b = (int)g_editor.lines.size() - 1;
    for (int i = a; i <= b; i++) { out += g_editor.lines[i]; out += '\n'; }
    return out;
}
static void viClampPos(TextPos &p) {
    if (g_editor.lines.empty()) g_editor.lines.push_back("");
    if (p.cy < 0) p.cy = 0;
    if (p.cy >= (int)g_editor.lines.size()) p.cy = (int)g_editor.lines.size() - 1;
    const int len = (int)g_editor.lines[p.cy].length();
    if (p.cx < 0) p.cx = 0;
    if (p.cx > len) p.cx = len;
}

static std::string viCharRangeText(TextPos s, TextPos e) {
    viClampPos(s);
    viClampPos(e);
    if (posLess(e, s)) std::swap(s, e);
    if (s.cy == e.cy) {
        if (e.cx <= s.cx) return "";
        return g_editor.lines[s.cy].substr(s.cx, e.cx - s.cx);
    }
    std::string out = g_editor.lines[s.cy].substr(s.cx) + "\n";
    for (int i = s.cy + 1; i < e.cy; i++) out += g_editor.lines[i] + "\n";
    out += g_editor.lines[e.cy].substr(0, e.cx);
    return out;
}

// ── 删除 / 缩进 / 大小写 ─────────────────────────────────────────────────
static void viDeleteCharwise(TextPos s, TextPos e) {
    viClampPos(s);
    viClampPos(e);
    if (posLess(e, s)) std::swap(s, e);
    if (s.cy == e.cy && s.cx == e.cx) return;
    g_editor.selAnchorCy = s.cy;
    g_editor.selAnchorCx = s.cx;
    g_editor.cy = e.cy;
    g_editor.cx = e.cx;
    g_editor.hasSelection = true;
    deleteSelection();
}

static void viDeleteLines(int a, int b) {
    if (a < 0) a = 0;
    if (b >= (int)g_editor.lines.size()) b = (int)g_editor.lines.size() - 1;
    if (a > b) return;
    int count = b - a + 1;
    g_editor.lines.erase(g_editor.lines.begin() + a, g_editor.lines.begin() + b + 1);
    foldLinesErased(a, count);
    if (g_editor.lines.empty()) g_editor.lines.push_back("");
    if (g_editor.cy >= (int)g_editor.lines.size())
        g_editor.cy = (int)g_editor.lines.size() - 1;
    if (g_editor.cy < a) g_editor.cy = a;
    if (g_editor.cy >= (int)g_editor.lines.size())
        g_editor.cy = (int)g_editor.lines.size() - 1;
    g_editor.cx = viFirstNonBlank(g_editor.lines[g_editor.cy]);
    viMarkChanged();
}

static void viChangeLines(int a, int b) {
    if (a < 0) a = 0;
    if (b >= (int)g_editor.lines.size()) b = (int)g_editor.lines.size() - 1;
    if (a > b) return;
    if (b > a) {
        g_editor.lines.erase(g_editor.lines.begin() + a + 1, g_editor.lines.begin() + b + 1);
        foldLinesErased(a + 1, b - a);
    }
    g_editor.lines[a].clear();
    g_editor.cy = a;
    g_editor.cx = 0;
    viMarkChanged();
}

static void viIndentLines(int a, int b, bool right) {
    if (a < 0) a = 0;
    if (b >= (int)g_editor.lines.size()) b = (int)g_editor.lines.size() - 1;
    for (int i = a; i <= b && i < (int)g_editor.lines.size(); i++) {
        std::string &s = g_editor.lines[i];
        if (right) {
            s.insert(0, "    ");
        } else if (!s.empty()) {
            if (s[0] == '\t') s.erase(0, 1);
            else {
                int n = 0;
                while (n < 4 && n < (int)s.length() && s[n] == ' ') n++;
                s.erase(0, n);
            }
        }
    }
    g_editor.cy = a;
    if (g_editor.cy >= (int)g_editor.lines.size())
        g_editor.cy = (int)g_editor.lines.size() - 1;
    g_editor.cx = viFirstNonBlank(g_editor.lines[g_editor.cy]);
    viMarkChanged();
}

static void viToggleCaseRange(TextPos s, TextPos e) {
    if (s.cy == e.cy && s.cx == e.cx) return;
    for (int y = s.cy; y <= e.cy && y < (int)g_editor.lines.size(); y++) {
        std::string &line = g_editor.lines[y];
        int from = (y == s.cy) ? s.cx : 0;
        int to = (y == e.cy) ? e.cx : (int)line.length();
        if (to > (int)line.length()) to = (int)line.length();
        for (int i = from; i < to; i++) {
            unsigned char c = (unsigned char)line[i];
            if (c >= 'a' && c <= 'z') line[i] = (char)(c - 'a' + 'A');
            else if (c >= 'A' && c <= 'Z') line[i] = (char)(c - 'A' + 'a');
        }
    }
    g_editor.cy = e.cy;
    g_editor.cx = e.cx;
    g_editor.targetCx = -1;
    viClampNormal();
    markDirty();
}

// ── 词移动 ───────────────────────────────────────────────────────────────
static void viStepNext(int &y, int &x) {
    if (y < 0 || y >= (int)g_editor.lines.size()) return;
    const std::string &s = g_editor.lines[y];
    if (x < (int)s.length()) x = utf8Next(s, x);
    else if (y < (int)g_editor.lines.size() - 1) { y++; x = 0; }
}
static void viStepPrev(int &y, int &x) {
    if (y < 0 || y >= (int)g_editor.lines.size()) return;
    if (x > 0) x = utf8Prev(g_editor.lines[y], x);
    else if (y > 0) { y--; x = (int)g_editor.lines[y].length(); }
}

static void viWordForward(int &y, int &x, int times, bool big) {
    for (int t = 0; t < times; t++) {
        if (y >= (int)g_editor.lines.size()) return;
        // 已在文末再往前: 提前退出, 避免超大 count 空转。
        if (y == (int)g_editor.lines.size() - 1 &&
            x >= (int)g_editor.lines[y].length()) return;
        const std::string &s = g_editor.lines[y];
        ViCharClass cls = (x >= (int)s.length()) ? ViCharClass::Space : viClassAt(s, x);
        if (cls == ViCharClass::Space) {
            // 跳过空白(可跨行)
            while (true) {
                const std::string &cur = g_editor.lines[y];
                while (x < (int)cur.length() && viSpaceAt(cur, x)) x = utf8Next(cur, x);
                if (x < (int)cur.length()) break;
                if (y + 1 < (int)g_editor.lines.size()) { y++; x = 0; }
                else break;
            }
        } else {
            if (big) {
                while (x < (int)s.length() && !viSpaceAt(s, x)) x = utf8Next(s, x);
            } else if (cls == ViCharClass::Han) {
                x = utf8Next(s, x);
            } else {
                while (x < (int)s.length() && viClassAt(s, x) == cls) x = utf8Next(s, x);
            }
            // 跳过后续空白(可跨行)
            while (true) {
                if (y >= (int)g_editor.lines.size()) break;
                const std::string &cur = g_editor.lines[y];
                while (x < (int)cur.length() && viSpaceAt(cur, x)) x = utf8Next(cur, x);
                if (x < (int)cur.length()) break;
                if (y + 1 < (int)g_editor.lines.size()) { y++; x = 0; }
                else break;
            }
        }
    }
}

static void viWordEnd(int &y, int &x, int times, bool big) {
    for (int t = 0; t < times; t++) {
        if (y >= (int)g_editor.lines.size()) return;
        if (y == (int)g_editor.lines.size() - 1 &&
            x >= (int)g_editor.lines[y].length()) return;
        viStepNext(y, x);
        // 跳过空白
        while (true) {
            if (y >= (int)g_editor.lines.size()) return;
            const std::string &s = g_editor.lines[y];
            while (x < (int)s.length() && viSpaceAt(s, x)) x = utf8Next(s, x);
            if (x < (int)s.length()) break;
            if (y + 1 < (int)g_editor.lines.size()) { y++; x = 0; }
            else return;
        }
        const std::string &s = g_editor.lines[y];
        ViCharClass cls = viClassAt(s, x);
        if (big) {
            while (true) {
                int n = utf8Next(s, x);
                if (n >= (int)s.length() || viSpaceAt(s, n)) break;
                x = n;
            }
        } else if (cls != ViCharClass::Han) {
            while (true) {
                int n = utf8Next(s, x);
                if (n >= (int)s.length() || viClassAt(s, n) != cls) break;
                x = n;
            }
        }
    }
}

static void viWordBackward(int &y, int &x, int times, bool big) {
    for (int t = 0; t < times; t++) {
        if (y <= 0 && x <= 0) return;  // 已在文首, 超大 count 不必空转
        viStepPrev(y, x);
        // 跳过空白(向前)
        while (true) {
            if (y < 0) return;
            const std::string &s = g_editor.lines[y];
            while (x > 0 && viSpaceAt(s, x)) x = utf8Prev(s, x);
            if (x > 0) break;
            if (viSpaceAt(s, 0) && y > 0) { y--; x = (int)g_editor.lines[y].length(); continue; }
            break;
        }
        const std::string &s = g_editor.lines[y];
        if (x >= (int)s.length()) continue;
        ViCharClass cls = viClassAt(s, x);
        if (big) {
            while (x > 0) {
                int p = utf8Prev(s, x);
                if (viSpaceAt(s, p)) break;
                x = p;
            }
        } else if (cls != ViCharClass::Han) {
            while (x > 0) {
                int p = utf8Prev(s, x);
                if (p < 0 || viClassAt(s, p) != cls) break;
                x = p;
            }
        }
    }
}

// ── 动作计算 ─────────────────────────────────────────────────────────────
static bool viLineRelMotion(int dir, int count, ViMotion &m) {
    if (dir == 0 || count <= 0) return false;
    int cy = g_editor.cy;
    int col = g_editor.targetCx;
    if (col < 0) col = byteToCells(g_editor.lines[cy], g_editor.cx);
    int ny = cy + dir * count;
    if (ny < 0) ny = 0;
    if (ny >= (int)g_editor.lines.size()) ny = (int)g_editor.lines.size() - 1;
    m.ok = true;
    m.linewise = true;
    m.keepCol = true;
    m.cy = ny;
    const std::string &s = g_editor.lines[ny];
    m.cx = cellsToByte(s, 0, (int)s.length(), col);
    return true;
}

static bool viFindMotion(int cmd, char target, int count, ViMotion &m) {
    if (target == 0 || count <= 0) return false;
    const std::string &s = g_editor.lines[g_editor.cy];
    int pos = g_editor.cx;
    int found = -1;
    if (cmd == 'f' || cmd == 't') {
        int from = pos;
        for (int i = 0; i < count; i++) {
            if (from >= (int)s.length()) return false;
            size_t p = s.find(target, from + 1);
            if (p == std::string::npos) return false;
            found = (int)p;
            from = found;
        }
        if (cmd == 't') found = utf8Prev(s, found);
    } else {
        int from = pos;
        for (int i = 0; i < count; i++) {
            if (from <= 0) return false;
            size_t p = s.rfind(target, from - 1);
            if (p == std::string::npos) return false;
            found = (int)p;
            from = found;
        }
        if (cmd == 'T') found = utf8Next(s, found);
    }
    m.ok = true;
    m.cy = g_editor.cy;
    m.cx = found;
    m.inclusive = (cmd == 'f' || cmd == 'F');
    return true;
}

// 括号表用码点, 扫描时直接解码比较; 旧实现为全文每个字符建 std::string,
// 长文档按 % 会分配数万个短串, 既慢又吃堆。
static const uint32_t kBrOpenCp[]  = {'(', '[', '{', 0xFF08, 0x3010, 0x300C, 0x300E, 0x300A, 0x3008, 0x3014};
static const uint32_t kBrCloseCp[] = {')', ']', '}', 0xFF09, 0x3011, 0x300D, 0x300F, 0x300B, 0x3009, 0x3015};
static const int kBrPairs = 10;

static int viBracketPairOf(uint32_t cp, bool &open) {
    for (int k = 0; k < kBrPairs; k++) {
        if (cp == kBrOpenCp[k]) { open = true; return k; }
        if (cp == kBrCloseCp[k]) { open = false; return k; }
    }
    return -1;
}

// 解码 s[pos] 并给出下一个码点边界; 非法/截断序列退化为前进一个字节。
static uint32_t viDecodeAt(const std::string &s, int pos, int &next) {
    const char *p = s.c_str() + pos;
    uint32_t cp = FontRenderer::utf8Decode(p);
    int nx = (int)(p - s.c_str());
    if (nx <= pos) nx = pos + 1;
    if (nx > (int)s.length()) nx = (int)s.length();
    next = nx;
    return cp;
}

static bool viMatchBracket(ViMotion &m) {
    const std::string &line = g_editor.lines[g_editor.cy];
    int atPos = -1, pair = -1;
    bool open = false;
    for (int p = g_editor.cx; p < (int)line.length();) {
        int nx;
        uint32_t cp = viDecodeAt(line, p, nx);
        bool o;
        int k = viBracketPairOf(cp, o);
        if (k >= 0) { atPos = p; pair = k; open = o; break; }
        p = nx;
    }
    if (atPos < 0) return false;

    int depth = 0;
    if (open) {
        for (int y = g_editor.cy; y < (int)g_editor.lines.size(); y++) {
            const std::string &s = g_editor.lines[y];
            for (int p = (y == g_editor.cy ? atPos : 0); p < (int)s.length();) {
                int nx;
                uint32_t cp = viDecodeAt(s, p, nx);
                if (cp == kBrOpenCp[pair]) depth++;
                else if (cp == kBrCloseCp[pair]) {
                    depth--;
                    if (depth == 0) {
                        m.ok = true; m.inclusive = true;
                        m.cy = y; m.cx = p;
                        return true;
                    }
                }
                p = nx;
            }
        }
    } else {
        for (int y = g_editor.cy; y >= 0; y--) {
            const std::string &s = g_editor.lines[y];
            for (int p = (y == g_editor.cy ? atPos : (int)s.length()); p > 0;) {
                p = utf8Prev(s, p);
                int nx;
                uint32_t cp = viDecodeAt(s, p, nx);
                if (cp == kBrCloseCp[pair]) depth++;
                else if (cp == kBrOpenCp[pair]) {
                    depth--;
                    if (depth == 0) {
                        m.ok = true; m.inclusive = true;
                        m.cy = y; m.cx = p;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// G 的 count 是绝对行号(1 起); lineNo<=0 表示末行。
static void viGotoLineMotion(int lineNo, ViMotion &m) {
    int ny = lineNo > 0 ? lineNo - 1 : (int)g_editor.lines.size() - 1;
    if (ny < 0) ny = 0;
    if (ny >= (int)g_editor.lines.size()) ny = (int)g_editor.lines.size() - 1;
    m.ok = true;
    m.linewise = true;
    m.cy = ny;
    m.cx = viFirstNonBlank(g_editor.lines[ny]);
}

static bool viMotionKey(int key, int count, ViMotion &m) {
    if (count <= 0) count = 1;
    bool vertical = editorVertical();
    int cy = g_editor.cy, cx = g_editor.cx;
    const std::string &line = g_editor.lines[cy];

    switch (key) {
        case 'h': case KEY_LEFT:
            if (vertical) {
                int ny = cy - count; if (ny < 0) ny = 0;
                m.ok = true; m.cy = ny; m.cx = viLastCharPos(g_editor.lines[ny]);
                return true;
            }
            for (int i = 0; i < count; i++) {
                if (cx <= 0) break;
                cx = utf8Prev(line, cx);
            }
            m.ok = true; m.cy = cy; m.cx = cx; return true;
        case 'l': case KEY_RIGHT:
            if (vertical) {
                int ny = cy + count; if (ny >= (int)g_editor.lines.size()) ny = (int)g_editor.lines.size() - 1;
                m.ok = true; m.cy = ny; m.cx = viFirstNonBlank(g_editor.lines[ny]);
                return true;
            }
            for (int i = 0; i < count; i++) {
                if (cx >= (int)line.length()) break;
                cx = utf8Next(line, cx);
            }
            m.ok = true; m.cy = cy; m.cx = cx; return true;
        case 'j': case KEY_DOWN:
            if (vertical) {
                for (int i = 0; i < count; i++) {
                    const std::string &cur = g_editor.lines[cy];
                    if (cx >= (int)cur.length() && cy + 1 < (int)g_editor.lines.size()) { cy++; cx = 0; }
                    else if (cx < (int)cur.length()) cx = utf8Next(cur, cx);
                }
                m.ok = true; m.cy = cy; m.cx = cx; return true;
            }
            return viLineRelMotion(1, count, m);
        case 'k': case KEY_UP:
            if (vertical) {
                for (int i = 0; i < count; i++) {
                    if (cx <= 0 && cy > 0) { cy--; cx = (int)g_editor.lines[cy].length(); }
                    else if (cx > 0) cx = utf8Prev(g_editor.lines[cy], cx);
                }
                m.ok = true; m.cy = cy; m.cx = cx; return true;
            }
            return viLineRelMotion(-1, count, m);
        case '0':
            m.ok = true; m.cy = cy; m.cx = 0; return true;
        case '^':
            m.ok = true; m.cy = cy; m.cx = viFirstNonBlank(line); return true;
        case '$':
            m.ok = true; m.cy = cy; m.cx = (int)line.length(); m.inclusive = true; return true;
        case 'w':
            viWordForward(cy, cx, count, false); m.ok = true; m.cy = cy; m.cx = cx; return true;
        case 'W':
            viWordForward(cy, cx, count, true); m.ok = true; m.cy = cy; m.cx = cx; return true;
        case 'b':
            viWordBackward(cy, cx, count, false); m.ok = true; m.cy = cy; m.cx = cx; return true;
        case 'B':
            viWordBackward(cy, cx, count, true); m.ok = true; m.cy = cy; m.cx = cx; return true;
        case 'e':
            viWordEnd(cy, cx, count, false); m.ok = true; m.cy = cy; m.cx = cx; m.inclusive = true; return true;
        case 'E':
            viWordEnd(cy, cx, count, true); m.ok = true; m.cy = cy; m.cx = cx; m.inclusive = true; return true;
        case 'G':
            // 显式 count 由调用方用 viGotoLineMotion() 处理。
            viGotoLineMotion(0, m);
            return true;
        case 0x0A: case 0x0D: {
            int ny = cy + count - 1;
            if (ny >= (int)g_editor.lines.size()) ny = (int)g_editor.lines.size() - 1;
            m.ok = true; m.linewise = true; m.cy = ny;
            m.cx = viFirstNonBlank(g_editor.lines[ny]);
            return true;
        }
        case KEY_HOME:
            m.ok = true; m.cy = cy; m.cx = 0; return true;
        case KEY_END:
            m.ok = true; m.cy = cy; m.cx = (int)line.length(); m.inclusive = true; return true;
        case 0x04: case 0x15: case 0x06: case 0x02:
        case KEY_PAGE_DOWN: case KEY_PAGE_UP: {
            int rows = editorPageRows();
            int amount;
            if (key == 0x04 || key == 0x15) amount = std::max(1, rows / 2);
            else amount = rows;
            int dir = (key == 0x04 || key == 0x06 || key == KEY_PAGE_DOWN) ? 1 : -1;
            ViMotion tmp;
            if (!viLineRelMotion(dir, amount * count, tmp)) return false;
            m = tmp;
            return true;
        }
        case '%':
            return viMatchBracket(m);
        case ';': case ',': {
            char cmd = g_editor.vi.lastFindCmd;
            char ch = g_editor.vi.lastFindChar;
            if (cmd == 0 || ch == 0) return false;
            if (key == ',') {
                if (cmd == 'f') cmd = 'F';
                else if (cmd == 'F') cmd = 'f';
                else if (cmd == 't') cmd = 'T';
                else if (cmd == 'T') cmd = 't';
            }
            return viFindMotion(cmd, ch, count, m);
        }
        default:
            return false;
    }
}

static void viApplyMotion(const ViMotion &m) {
    if (!m.ok) return;
    if (m.cy < 0) return;
    if (m.cy >= (int)g_editor.lines.size()) return;
    g_editor.cy = m.cy;
    g_editor.cx = m.cx;
    if (!m.keepCol) g_editor.targetCx = -1;
    viClampNormal();
    if (g_editor.vi.mode != ViMode::Normal) g_editor.hasSelection = true;
}

// ── 插入/普通模式切换 ────────────────────────────────────────────────────
static void viEnterInsert() {
    ViState &v = g_editor.vi;
    v.mode = ViMode::Insert;
    v.op = ViOperator::None;
    v.count = v.opCount = 0;
    v.wantRegister = false;
    v.reg = 0;
    v.gPending = false;
    v.findCmd = 0;
    v.replacePending = false;
    v.cmdlineActive = false;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    g_editor.imeActive = v.insertImePref;
    g_ime.setActive(g_editor.imeActive);
    if (g_editor.imeActive) g_ime.setDocumentContext(editorImeContextText());
}

static void viLeaveInsert() {
    ViState &v = g_editor.vi;
    v.insertImePref = g_editor.imeActive;
    g_ime.cancelComposition();
    g_editor.imeActive = false;
    g_ime.setActive(false);
    v.mode = ViMode::Normal;
    v.count = v.opCount = 0;
    v.op = ViOperator::None;
    g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    // vim: 退出插入时光标左移一个字符
    if (g_editor.cx > 0) g_editor.cx = utf8Prev(g_editor.lines[g_editor.cy], g_editor.cx);
    else if (g_editor.cy > 0) {
        g_editor.cy--;
        g_editor.cx = viLastCharPos(g_editor.lines[g_editor.cy]);
    }
    viClampNormal();
}

static void viEnterNormal(bool clearSel) {
    ViState &v = g_editor.vi;
    v.mode = ViMode::Normal;
    v.op = ViOperator::None;
    v.count = v.opCount = 0;
    v.wantRegister = false;
    v.gPending = false;
    v.findCmd = 0;
    v.replacePending = false;
    v.cmdlineActive = false;
    if (clearSel) g_editor.hasSelection = false;
    g_editor.targetCx = -1;
    viClampNormal();
}

static void viStartVisual(ViMode mode) {
    ViState &v = g_editor.vi;
    v.mode = mode;
    v.op = ViOperator::None;
    v.count = v.opCount = 0;
    v.wantRegister = false;
    v.gPending = false;
    v.findCmd = 0;
    v.replacePending = false;
    g_editor.selAnchorCy = g_editor.cy;
    g_editor.selAnchorCx = g_editor.cx;
    g_editor.hasSelection = true;
    g_editor.targetCx = -1;
}

// ── 操作符执行 ───────────────────────────────────────────────────────────
static void viApplyOperator(ViOperator op, const ViMotion &m, bool motionDone) {
    ViState &v = g_editor.vi;
    if (!motionDone || !m.ok) { v.op = ViOperator::None; return; }
    TextPos cur = {g_editor.cy, g_editor.cx};
    TextPos dst = {m.cy, m.cx};
    if (op != ViOperator::Yank) recordUndoSnapshot(UndoGroup::Structural);

    if (m.linewise) {
        int a = std::min(cur.cy, dst.cy);
        int b = std::max(cur.cy, dst.cy);
        if (a < 0) a = 0;
        if (b >= (int)g_editor.lines.size()) b = (int)g_editor.lines.size() - 1;
        if (op == ViOperator::Yank) {
            std::string text = viLineRangeText(a, b);
            viWriteRegister(v.reg, text, true);
            g_editor.cy = a;
            g_editor.cx = viFirstNonBlank(g_editor.lines[a]);
        } else if (op == ViOperator::Delete) {
            std::string text = viLineRangeText(a, b);
            viWriteRegister(v.reg, text, true);
            viDeleteLines(a, b);
        } else if (op == ViOperator::Change) {
            std::string text = viLineRangeText(a, b);
            viWriteRegister(v.reg, text, true);
            viChangeLines(a, b);
            viEnterInsert();
            return;
        } else {
            viIndentLines(a, b, op == ViOperator::IndentRight);
        }
    } else {
        TextPos s, e;
        if (posLess(cur, dst)) { s = cur; e = dst; }
        else { s = dst; e = cur; }
        if (m.inclusive && e.cx < (int)g_editor.lines[e.cy].length())
            e.cx = utf8Next(g_editor.lines[e.cy], e.cx);
        if (s.cy == e.cy && s.cx == e.cx) { v.op = ViOperator::None; return; }
        if (op == ViOperator::Yank) {
            std::string text = viCharRangeText(s, e);
            viWriteRegister(v.reg, text, false);
            g_editor.cy = s.cy;
            g_editor.cx = s.cx;
        } else if (op == ViOperator::Delete) {
            std::string text = viCharRangeText(s, e);
            viWriteRegister(v.reg, text, false);
            viDeleteCharwise(s, e);
        } else if (op == ViOperator::Change) {
            std::string text = viCharRangeText(s, e);
            viWriteRegister(v.reg, text, false);
            viDeleteCharwise(s, e);
            viEnterInsert();
            return;
        } else {
            viIndentLines(s.cy, e.cy, op == ViOperator::IndentRight);
            return;
        }
    }
    viClampNormal();
    v.op = ViOperator::None;
    v.count = v.opCount = 0;
    v.wantRegister = false;
    v.reg = 0;
}

// 双击操作符: 当前行起 count 行的整行操作。
static ViMotion viDoubleOperatorMotion(int count) {
    ViMotion m;
    int a = g_editor.cy;
    int b = a + (count > 0 ? count : 1) - 1;
    if (b >= (int)g_editor.lines.size()) b = (int)g_editor.lines.size() - 1;
    m.ok = true;
    m.linewise = true;
    m.cy = b;
    m.cx = (int)g_editor.lines[b].length();
    return m;
}

// ── 普通模式直接命令 ─────────────────────────────────────────────────────
static void viPaste(bool after) {
    ViState &v = g_editor.vi;
    std::string text;
    bool linewise = false;
    if (!viReadRegister(v.reg, text, linewise) || text.empty()) {
        v.reg = 0;
        return;
    }
    recordUndoSnapshot(UndoGroup::Structural);
    int outCy = g_editor.cy, outCx = g_editor.cx;
    if (linewise) {
        std::vector<std::string> ins;
        size_t pos = 0;
        while (pos < text.length()) {
            size_t nl = text.find('\n', pos);
            ins.push_back(nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        if (ins.size() > 1 && ins.back().empty()) ins.pop_back();
        int at = g_editor.cy + (after ? 1 : 0);
        if (at > (int)g_editor.lines.size()) at = (int)g_editor.lines.size();
        insertLinesAt(at, ins, outCy, outCx);
        g_editor.cy = outCy;
        g_editor.cx = viFirstNonBlank(g_editor.lines[outCy]);
    } else {
        int atCx = g_editor.cx;
        if (after && atCx < (int)g_editor.lines[g_editor.cy].length())
            atCx = utf8Next(g_editor.lines[g_editor.cy], atCx);
        insertMultilineAt(g_editor.cy, atCx, text, outCy, outCx);
        g_editor.cy = outCy;
        g_editor.cx = outCx;
        if (g_editor.cx > 0) g_editor.cx = utf8Prev(g_editor.lines[g_editor.cy], g_editor.cx);
    }
    viMarkChanged();
    viClampNormal();
    v.reg = 0;
}

static void viJoinLines(int count) {
    int n = count > 0 ? count : 2;
    int a = g_editor.cy;
    int b = std::min(a + n - 1, (int)g_editor.lines.size() - 1);
    if (b <= a) return;
    recordUndoSnapshot(UndoGroup::Structural);
    std::string joined = g_editor.lines[a];
    int curPos = (int)joined.length();
    for (int i = a + 1; i <= b; i++) {
        std::string next = g_editor.lines[i];
        size_t ws = 0;
        while (ws < next.length() && (next[ws] == ' ' || next[ws] == '\t')) ws++;
        next.erase(0, ws);
        if (!next.empty()) {
            if (!joined.empty() && joined.back() != ' ') joined += ' ';
            if (curPos == (int)joined.length()) curPos = (int)joined.length();
            joined += next;
        }
    }
    g_editor.lines[a] = joined;
    if (b > a) {
        g_editor.lines.erase(g_editor.lines.begin() + a + 1, g_editor.lines.begin() + b + 1);
        foldLinesErased(a + 1, b - a);
    }
    g_editor.cy = a;
    g_editor.cx = curPos > 0 ? utf8Prev(g_editor.lines[a], std::min(curPos, (int)g_editor.lines[a].length())) : 0;
    viMarkChanged();
    viClampNormal();
}

static bool viReplaceChars(int count, int ch) {
    if (count <= 0) count = 1;
    if (ch < 0x20 || ch > 0x7E) return false;
    std::string &line = g_editor.lines[g_editor.cy];
    int p = g_editor.cx;
    int end = p;
    int n = 0;
    while (n < count && end < (int)line.length()) { end = utf8Next(line, end); n++; }
    if (n == 0) return false;
    recordUndoSnapshot(UndoGroup::Structural);
    std::string repl(n, (char)ch);
    line = line.substr(0, p) + repl + line.substr(end);
    g_editor.cx = p + n;
    if (g_editor.cx > 0) g_editor.cx = utf8Prev(line, g_editor.cx);
    viMarkChanged();
    return true;
}

// ── 搜索 ─────────────────────────────────────────────────────────────────
// 逐行扫描 term 的所有非重叠匹配(term 不含 '\n'), 不复制整篇文档、
// 也不为每个匹配建表; 只统计总数并记下光标前/后的首个匹配序号。
static void viScanMatches(const std::string &term, int curOff, int &total,
                          int &firstFwd, int &prevBwd) {
    total = 0;
    firstFwd = prevBwd = -1;
    if (term.empty()) return;
    int cum = 0;
    for (int y = 0; y < (int)g_editor.lines.size(); y++) {
        const std::string &s = g_editor.lines[y];
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t f = s.find(term, pos);
            if (f == std::string::npos) break;
            int off = cum + (int)f;
            if (off > curOff && firstFwd < 0) firstFwd = total;
            if (off < curOff) prevBwd = total;
            total++;
            pos = f + term.size();
        }
        cum += (int)s.size() + 1;
    }
}

// 第二遍取第 idx 个匹配的文档偏移; 调用前已确认 idx < total。
static int viNthMatchOffset(const std::string &term, int idx) {
    int cum = 0, seen = 0;
    for (int y = 0; y < (int)g_editor.lines.size(); y++) {
        const std::string &s = g_editor.lines[y];
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t f = s.find(term, pos);
            if (f == std::string::npos) break;
            if (seen == idx) return cum + (int)f;
            seen++;
            pos = f + term.size();
        }
        cum += (int)s.size() + 1;
    }
    return -1;
}

static bool viSearchApply(const std::string &term, bool forward, int count) {
    if (term.empty()) return false;
    int curOff = posToDocOffset(g_editor.cy, g_editor.cx);
    int total, firstFwd, prevBwd;
    viScanMatches(term, curOff, total, firstFwd, prevBwd);
    if (total == 0) { viSetMsg("未找到: " + term); return false; }
    int idx = forward ? (firstFwd >= 0 ? firstFwd : 0)
                      : (prevBwd >= 0 ? prevBwd : total - 1);
    // count 已夹在 999999 内, 但乘/加仍用 long long 避免溢出。
    long long t = (long long)idx + (forward ? 1LL : -1LL) * ((long long)count - 1);
    t %= total;
    if (t < 0) t += total;
    int off = viNthMatchOffset(term, (int)t);
    if (off < 0) { viSetMsg("未找到: " + term); return false; }
    int ny, nx;
    docOffsetToPos(off, ny, nx);
    g_editor.cy = ny;
    g_editor.cx = nx;
    g_editor.targetCx = -1;
    g_editor.hasSelection = false;
    viClampNormal();
    return true;
}

// ── 命令行 ───────────────────────────────────────────────────────────────
static void viStartCmdline(char type) {
    ViState &v = g_editor.vi;
    v.cmdlineActive = true;
    v.cmdlineType = type;
    v.cmdline.clear();
    v.cmdlineCur = 0;
    v.cmdlineIme = false;
    g_ime.cancelComposition();
    g_ime.setActive(false);
    g_editor.imeActive = false;
}
static void viStopCmdline() {
    ViState &v = g_editor.vi;
    v.cmdlineActive = false;
    v.cmdline.clear();
    v.cmdlineCur = 0;
    v.cmdlineIme = false;
    g_ime.cancelComposition();
    g_ime.setActive(false);
}
// 命令栏上限: 按键重复/输入法上屏都可能无限追加, 也会让绘制里的
// substr+textWidth 循环变成 O(n²)。
static const size_t kViCmdlineMax = 128;
static void viCmdlineInsert(const std::string &s) {
    ViState &v = g_editor.vi;
    if (v.cmdlineCur < 0) v.cmdlineCur = 0;
    if (v.cmdlineCur > (int)v.cmdline.length()) v.cmdlineCur = (int)v.cmdline.length();
    size_t room = v.cmdline.length() >= kViCmdlineMax ? 0 : kViCmdlineMax - v.cmdline.length();
    if (room == 0) return;
    size_t take = s.size();
    if (take > room) {
        // 按码点边界截断, 不能把 UTF-8 字符切成半个。
        take = 0;
        while (take < s.size() && take < room) {
            int nx = utf8Next(s, (int)take);
            if ((size_t)nx > room) break;
            take = (size_t)nx;
        }
    }
    if (take == 0) return;
    v.cmdline.insert(v.cmdlineCur, s.substr(0, take));
    v.cmdlineCur += (int)take;
}

static AppState viQuitEditor(ScreenContext &ctx, bool force) {
    if (!force && g_editor.modifiedSinceSave) {
        viSetMsg("有未保存更改 (用 :q!)");
        return APP_EDITOR;
    }
    if (inQuickFileSession() || inFileEditSession()) {
        ctx.nextState = APP_SETTINGS;
        return APP_SETTINGS;
    }
    ctx.nextState = ctx.prevState;
    return ctx.prevState;
}

static AppState viExecCommand(const std::string &raw, ScreenContext &ctx) {
    std::string cmd = raw;
    size_t b = cmd.find_first_not_of(" \t");
    size_t e = cmd.find_last_not_of(" \t");
    if (b == std::string::npos) cmd.clear();
    else cmd = cmd.substr(b, e - b + 1);

    if (cmd == "w") {
        if (saveCurrentContent()) {
            g_editor.modifiedSinceSave = false;
            g_editor.autoSaveTime = 0;
            viSetMsg("已保存");
        } else {
            viSetMsg("保存失败");
        }
        return APP_EDITOR;
    }
    if (cmd == "q") return viQuitEditor(ctx, false);
    if (cmd == "q!") return viQuitEditor(ctx, true);
    if (cmd == "wq") {
        if (!saveCurrentContent()) { viSetMsg("保存失败"); return APP_EDITOR; }
        g_editor.modifiedSinceSave = false;
        g_editor.autoSaveTime = 0;
        return viQuitEditor(ctx, true);
    }
    if (cmd == "x") {
        if (g_editor.modifiedSinceSave) {
            if (!saveCurrentContent()) { viSetMsg("保存失败"); return APP_EDITOR; }
            g_editor.modifiedSinceSave = false;
            g_editor.autoSaveTime = 0;
        }
        return viQuitEditor(ctx, true);
    }
    if (cmd == "$") {
        g_editor.cy = (int)g_editor.lines.size() - 1;
        g_editor.cx = viFirstNonBlank(g_editor.lines[g_editor.cy]);
        g_editor.targetCx = -1;
        return APP_EDITOR;
    }
    if (!cmd.empty() && cmd.find_first_not_of("0123456789") == std::string::npos) {
        // atoi 在超长数字串上溢出(UB); 命令栏虽有上限, 仍按 clamp 方式解析。
        int n = 0;
        for (char c : cmd) {
            if (n >= kViMaxCount) break;
            n = n * 10 + (c - '0');
            if (n > kViMaxCount) n = kViMaxCount;
        }
        if (n > 0) {
            if (n > (int)g_editor.lines.size()) n = (int)g_editor.lines.size();
            g_editor.cy = n - 1;
            g_editor.cx = viFirstNonBlank(g_editor.lines[g_editor.cy]);
            g_editor.targetCx = -1;
        }
        return APP_EDITOR;
    }
    if (cmd == "set nu" || cmd == "set number") {
        g_settings.setString("show_line_numbers", "1");
        g_editor.cachedGutterCells = -1;
        markDirty();
        viSetMsg(editorVertical() ? "竖排不显示行号" : "行号已开启");
        return APP_EDITOR;
    }
    if (cmd == "set nonu" || cmd == "set nonumber") {
        g_settings.setString("show_line_numbers", "0");
        g_editor.cachedGutterCells = -1;
        markDirty();
        viSetMsg("行号已关闭");
        return APP_EDITOR;
    }
    if (cmd == "set") {
        viSetMsg(g_settings.showLineNumbers() ? "nu" : "nonu");
        return APP_EDITOR;
    }
    if (cmd.empty()) return APP_EDITOR;
    viSetMsg("未知命令: " + cmd);
    return APP_EDITOR;
}

static AppState viCmdlineKey(int key, ScreenContext &ctx) {
    ViState &v = g_editor.vi;

    if (key == KEY_IME_TOGGLE) {
        if (v.cmdlineType == ':') return APP_EDITOR;  // : 命令永远原样 ASCII
        v.cmdlineIme = !v.cmdlineIme;
        if (!v.cmdlineIme) g_ime.cancelComposition();
        g_ime.setActive(v.cmdlineIme);
        if (v.cmdlineIme) g_ime.setDocumentContext(editorImeContextText());
        return APP_EDITOR;
    }
    if (v.cmdlineIme &&
        (key == KEY_FULLWIDTH_TOGGLE || key == KEY_TRAD_TOGGLE || key == KEY_LSHIFT_TAP)) {
        std::string out;
        g_ime.handleKey(key, out);  // 仅切换输入法状态, 不产生上屏
        if (!out.empty()) viCmdlineInsert(out);
        return APP_EDITOR;
    }
    if (v.cmdlineIme && g_ime.composing()) {
        if (key == 0x1B) { g_ime.cancelComposition(); return APP_EDITOR; }
        if (key == 0x0A || key == 0x0D) {
            std::string out;
            g_ime.handleKey(key, out);
            if (!out.empty()) viCmdlineInsert(out);
            return APP_EDITOR;
        }
        if ((key >= 0x20 && key <= 0x7E) || key == 0x7F || key == 0x08) {
            std::string out;
            if (g_ime.handleKey(key, out)) {
                if (!out.empty()) viCmdlineInsert(out);
                return APP_EDITOR;
            }
        }
    } else if (v.cmdlineIme && key >= 0x20 && key <= 0x7E) {
        std::string out;
        if (g_ime.handleKey(key, out)) {
            if (!out.empty()) viCmdlineInsert(out);
            return APP_EDITOR;
        }
    }

    if (key == 0x1B) { viStopCmdline(); return APP_EDITOR; }
    if (key == 0x0A || key == 0x0D) {
        std::string cmd = v.cmdline;
        char type = v.cmdlineType;
        viStopCmdline();
        if (type == ':') return viExecCommand(cmd, ctx);
        if (cmd.empty()) cmd = v.lastSearch;
        if (cmd.empty()) { viSetMsg("无上次搜索"); return APP_EDITOR; }
        v.lastSearch = cmd;
        v.lastSearchForward = (type == '/');
        viSearchApply(cmd, type == '/', 1);
        return APP_EDITOR;
    }
    if (key == 0x7F || key == 0x08) {
        if (v.cmdlineCur > 0) {
            int p = utf8Prev(v.cmdline, v.cmdlineCur);
            v.cmdline.erase(p, v.cmdlineCur - p);
            v.cmdlineCur = p;
        }
        return APP_EDITOR;
    }
    if (key == KEY_LEFT) {
        if (v.cmdlineCur > 0) v.cmdlineCur = utf8Prev(v.cmdline, v.cmdlineCur);
        return APP_EDITOR;
    }
    if (key == KEY_RIGHT) {
        if (v.cmdlineCur < (int)v.cmdline.length())
            v.cmdlineCur = utf8Next(v.cmdline, v.cmdlineCur);
        return APP_EDITOR;
    }
    if (key == KEY_HOME) { v.cmdlineCur = 0; return APP_EDITOR; }
    if (key == KEY_END) { v.cmdlineCur = (int)v.cmdline.length(); return APP_EDITOR; }
    if (key >= 0x20 && key <= 0x7E) {
        viCmdlineInsert(std::string(1, (char)key));
        return APP_EDITOR;
    }
    return APP_EDITOR;
}

// ── 视觉模式命令 ─────────────────────────────────────────────────────────
static void viVisualDeleteOrChange(bool change) {
    ViState &v = g_editor.vi;
    TextPos s, e;
    viVisualRange(s, e);
    if (s.cy == e.cy && s.cx == e.cx && v.mode != ViMode::VisualLine) {
        viEnterNormal(true);
        return;
    }
    recordUndoSnapshot(UndoGroup::Structural);
    if (v.mode == ViMode::VisualLine) {
        int b = e.cy;
        if (b < s.cy) b = s.cy;
        std::string text = viLineRangeText(s.cy, b);
        viWriteRegister(v.reg, text, true);
        if (change) {
            viChangeLines(s.cy, b);
            viEnterInsert();
            return;
        }
        viDeleteLines(s.cy, b);
    } else {
        std::string text = viCharRangeText(s, e);
        viWriteRegister(v.reg, text, false);
        if (change) {
            viDeleteCharwise(s, e);
            viEnterInsert();
            return;
        }
        viDeleteCharwise(s, e);
    }
    viEnterNormal(true);
    v.reg = 0;
}

static void viVisualYank() {
    ViState &v = g_editor.vi;
    TextPos s, e;
    viVisualRange(s, e);
    if (v.mode == ViMode::VisualLine) {
        int b = e.cy;
        if (b < s.cy) b = s.cy;
        viWriteRegister(v.reg, viLineRangeText(s.cy, b), true);
    } else {
        viWriteRegister(v.reg, viCharRangeText(s, e), false);
    }
    int y = s.cy, x = s.cx;
    viEnterNormal(true);
    g_editor.cy = y;
    g_editor.cx = x;
    viClampNormal();
    v.reg = 0;
}

static void viVisualReplace() {
    ViState &v = g_editor.vi;
    std::string text;
    bool linewise = false;
    if (!viReadRegister(v.reg, text, linewise) || text.empty()) { v.reg = 0; return; }
    TextPos s, e;
    viVisualRange(s, e);
    recordUndoSnapshot(UndoGroup::Structural);
    if (linewise) {
        std::vector<std::string> ins;
        size_t pos = 0;
        while (pos < text.length()) {
            size_t nl = text.find('\n', pos);
            ins.push_back(nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        // 行寄存器的尾随换行拆出一个空串; 但 "\n"(单个空行)要保留成一行。
        if (ins.size() > 1 && ins.back().empty()) ins.pop_back();
        int b = e.cy;
        if (b < s.cy) b = s.cy;
        g_editor.lines.erase(g_editor.lines.begin() + s.cy, g_editor.lines.begin() + b + 1);
        foldLinesErased(s.cy, b - s.cy + 1);
        if (g_editor.lines.empty()) g_editor.lines.push_back("");
        int outCy = s.cy, outCx = 0;
        insertLinesAt(s.cy, ins, outCy, outCx);
        g_editor.cy = outCy;
        g_editor.cx = viFirstNonBlank(g_editor.lines[outCy]);
    } else if (v.mode == ViMode::VisualLine) {
        int b = e.cy;
        if (b < s.cy) b = s.cy;
        g_editor.lines.erase(g_editor.lines.begin() + s.cy, g_editor.lines.begin() + b + 1);
        foldLinesErased(s.cy, b - s.cy + 1);
        if (g_editor.lines.empty()) g_editor.lines.push_back("");
        int outCy = s.cy, outCx = 0;
        if (s.cy >= (int)g_editor.lines.size()) {
            // 选区在文档末尾: 在最后一行之后另起一行粘贴。
            int last = (int)g_editor.lines.size() - 1;
            insertMultilineAt(last, (int)g_editor.lines[last].length(), "\n" + text, outCy, outCx);
        } else {
            insertMultilineAt(s.cy, 0, text, outCy, outCx);
        }
        g_editor.cy = outCy;
        g_editor.cx = outCx;
    } else {
        viDeleteCharwise(s, e);
        int outCy = g_editor.cy, outCx = g_editor.cx;
        insertMultilineAt(g_editor.cy, g_editor.cx, text, outCy, outCx);
        g_editor.cy = outCy;
        g_editor.cx = outCx;
        if (outCx > 0) g_editor.cx = utf8Prev(g_editor.lines[outCy], outCx);
    }
    viMarkChanged();
    viEnterNormal(true);
    v.reg = 0;
}

// 视觉模式下的按键处理。
static void viVisualKey(int key, bool &needDraw, bool &handled) {
    ViState &v = g_editor.vi;
    needDraw = true;
    int count = v.count > 0 ? v.count : 1;

    if (key >= '1' && key <= '9') {
        viCountDigit(v.count, key - '0');
        return;
    }
    if (key == '0' && v.count > 0) { viCountDigit(v.count, 0); return; }
    if (key == '"') { v.wantRegister = true; return; }

    if (v.wantRegister) {
        v.reg = ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
                 (key >= '0' && key <= '9') || key == '+' || key == '*') ? (char)key : 0;
        v.wantRegister = false;
        return;
    }
    if (v.findCmd) {
        ViMotion m;
        if (key >= 0x20 && key <= 0x7E && viFindMotion(v.findCmd, (char)key, count, m)) {
            v.lastFindCmd = v.findCmd;
            v.lastFindChar = (char)key;
            viApplyMotion(m);
        }
        v.findCmd = 0;
        v.count = 0;
        return;
    }

    switch (key) {
        case 0x1B:
            viEnterNormal(true);
            return;
        case 'v':
            if (v.mode == ViMode::VisualLine) { v.mode = ViMode::Visual; return; }
            viEnterNormal(true);
            return;
        case 'V':
            if (v.mode == ViMode::Visual) { v.mode = ViMode::VisualLine; return; }
            viEnterNormal(true);
            return;
        case 'o': {
            int ay = g_editor.selAnchorCy, ax = g_editor.selAnchorCx;
            g_editor.selAnchorCy = g_editor.cy;
            g_editor.selAnchorCx = g_editor.cx;
            g_editor.cy = ay;
            g_editor.cx = ax;
            viClampNormal();
            return;
        }
        case 'd': case 'x': viVisualDeleteOrChange(false); return;
        case 'c': viVisualDeleteOrChange(true); return;
        case 'y': viVisualYank(); return;
        case 'p': case 'P': viVisualReplace(); return;
        case '>': case '<': {
            TextPos s, e;
            viVisualRange(s, e);
            int b = e.cy;
            if (b < s.cy) b = s.cy;
            recordUndoSnapshot(UndoGroup::Structural);
            viIndentLines(s.cy, b, key == '>');
            viEnterNormal(true);
            return;
        }
        case '~': {
            TextPos s, e;
            viVisualRange(s, e);
            recordUndoSnapshot(UndoGroup::Structural);
            viToggleCaseRange(s, e);
            viEnterNormal(true);
            return;
        }
        case 0x03: viVisualYank(); return;      // Ctrl+C
        case 0x18: viVisualDeleteOrChange(false); return;  // Ctrl+X
        case 0x16: viVisualReplace(); return;   // Ctrl+V
        case 0x01: {                            // Ctrl+A: 全选
            v.mode = ViMode::Visual;
            g_editor.selAnchorCy = 0;
            g_editor.selAnchorCx = 0;
            g_editor.cy = (int)g_editor.lines.size() - 1;
            g_editor.cx = viLastCharPos(g_editor.lines[g_editor.cy]);
            viClampNormal();
            g_editor.hasSelection = true;
            return;
        }
        default: break;
    }

    ViMotion m;
    if (key == 'G' && v.count > 0) { viGotoLineMotion(v.count, m); }
    else if (key == KEY_SHIFT_LEFT) { viMotionKey(KEY_LEFT, count, m); }
    else if (key == KEY_SHIFT_RIGHT) { viMotionKey(KEY_RIGHT, count, m); }
    else if (key == KEY_SHIFT_UP) { viMotionKey(KEY_UP, count, m); }
    else if (key == KEY_SHIFT_DOWN) { viMotionKey(KEY_DOWN, count, m); }
    else if (viMotionKey(key, count, m)) { /* handled below */ }
    else {
        v.count = 0;
        // 未列出的 Ctrl 键交回原快捷键(Ctrl+S/Q/O 等), 与 normal 一致。
        if (key > 0 && key < 0x20) handled = false;
        return;
    }
    if (m.ok) viApplyMotion(m);
    v.count = 0;
}

// ── 普通模式按键 ─────────────────────────────────────────────────────────
static void viNormalKey(int key, ScreenContext &ctx, bool &needDraw, AppState &out,
                        bool &handled) {
    ViState &v = g_editor.vi;
    needDraw = true;
    int count = v.count > 0 ? v.count : 1;

    // 数字计数
    if (key >= '1' && key <= '9') { viCountDigit(v.count, key - '0'); return; }
    if (key == '0' && v.count > 0) { viCountDigit(v.count, 0); return; }

    // 寄存器前缀
    if (v.wantRegister) {
        v.reg = ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
                 (key >= '0' && key <= '9') || key == '+' || key == '*') ? (char)key : 0;
        v.wantRegister = false;
        return;
    }
    if (key == '"') { v.wantRegister = true; return; }

    // g 前缀
    if (v.gPending) {
        v.gPending = false;
        if (key == 'g') {
            int n = v.count > 0 ? v.count : 1;
            if (n > (int)g_editor.lines.size()) n = (int)g_editor.lines.size();
            g_editor.cy = n - 1;
            g_editor.cx = viFirstNonBlank(g_editor.lines[g_editor.cy]);
            g_editor.targetCx = -1;
            v.count = 0;
        }
        return;
    }
    if (key == 'g') { v.gPending = true; return; }

    // f/t 等待目标字符
    if (v.findCmd) {
        char cmd = v.findCmd;
        v.findCmd = 0;
        if (key < 0x20 || key > 0x7E) { v.count = 0; return; }
        ViMotion m;
        if (viFindMotion(cmd, (char)key, count, m)) {
            v.lastFindCmd = cmd;
            v.lastFindChar = (char)key;
            viApplyMotion(m);
        }
        v.count = 0;
        return;
    }
    if (key == 'f' || key == 'F' || key == 't' || key == 'T') {
        v.findCmd = (char)key;
        return;
    }

    // r 等待替换字符
    if (v.replacePending) {
        v.replacePending = false;
        if (key >= 0x20 && key <= 0x7E) viReplaceChars(count, key);
        v.count = 0;
        return;
    }
    if (key == 'r') { v.replacePending = true; return; }

    // 操作符待决
    if (v.op != ViOperator::None) {
        if (key == '"') { v.wantRegister = true; return; }
        if (key == 'd' || key == 'c' || key == 'y' || key == '>' || key == '<') {
            ViOperator same = v.op;
            bool matches = (key == 'd' && same == ViOperator::Delete) ||
                           (key == 'c' && same == ViOperator::Change) ||
                           (key == 'y' && same == ViOperator::Yank) ||
                           (key == '>' && same == ViOperator::IndentRight) ||
                           (key == '<' && same == ViOperator::IndentLeft);
            if (matches) {
                int n = viCountMul(v.opCount > 0 ? v.opCount : 1, count);
                viApplyOperator(same, viDoubleOperatorMotion(n), true);
                v.count = v.opCount = 0;
                v.reg = 0;
                return;
            }
            v.op = ViOperator::None;
            v.count = v.opCount = 0;
            return;
        }
        if (key >= '1' && key <= '9') {
            viCountDigit(v.count, key - '0');
            return;
        }
        if (key == '0' && v.count > 0) {
            viCountDigit(v.count, 0);
            return;
        }
        ViMotion m;
        int eff = viCountMul(v.opCount > 0 ? v.opCount : 1, count);
        int motionKey = key;
        if (key == KEY_SHIFT_LEFT) motionKey = KEY_LEFT;
        else if (key == KEY_SHIFT_RIGHT) motionKey = KEY_RIGHT;
        else if (key == KEY_SHIFT_UP) motionKey = KEY_UP;
        else if (key == KEY_SHIFT_DOWN) motionKey = KEY_DOWN;
        // cw 在非空白字符上等价 ce; dw/yw/cw 在行内最后一个词时不吞下一行换行。
        bool ok;
        if (key == 'G' && (v.count > 0 || v.opCount > 0)) {
            viGotoLineMotion(eff, m);
            ok = true;
        } else if ((key == 'w' || key == 'W') && v.op == ViOperator::Change &&
                   !viSpaceAt(g_editor.lines[g_editor.cy], g_editor.cx)) {
            ok = viMotionKey(key == 'w' ? 'e' : 'E', eff, m);
        } else {
            ok = viMotionKey(motionKey, eff, m);
        }
        if (ok && (key == 'w' || key == 'W') && eff == 1 && !m.linewise &&
            m.cy > g_editor.cy) {
            m.cy = g_editor.cy;
            m.cx = (int)g_editor.lines[g_editor.cy].length();
            m.inclusive = false;
        }
        if (ok) {
            viApplyOperator(v.op, m, true);
            v.count = v.opCount = 0;
            v.reg = 0;
        } else {
            v.op = ViOperator::None;
            v.count = v.opCount = 0;
        }
        return;
    }

    // 操作符前缀
    if (key == 'd' || key == 'c' || key == 'y' || key == '>' || key == '<') {
        v.op = (key == 'd') ? ViOperator::Delete :
               (key == 'c') ? ViOperator::Change :
               (key == 'y') ? ViOperator::Yank :
               (key == '>') ? ViOperator::IndentRight : ViOperator::IndentLeft;
        v.opCount = v.count;
        v.count = 0;
        return;
    }

    switch (key) {
        case 0x1B:
            viEnterNormal(true);
            v.count = v.opCount = 0;
            v.reg = 0;
            return;
        case 'i':
            viEnterInsert();
            return;
        case 'a':
            if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length())
                g_editor.cx = utf8Next(g_editor.lines[g_editor.cy], g_editor.cx);
            viEnterInsert();
            return;
        case 'I':
            g_editor.cx = viFirstNonBlank(g_editor.lines[g_editor.cy]);
            viEnterInsert();
            return;
        case 'A':
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
            viEnterInsert();
            return;
        case 'o': {
            recordUndoSnapshot(UndoGroup::Structural);
            std::vector<std::string> ins(1, "");
            int outCy = 0, outCx = 0;
            int at = g_editor.cy + 1;
            if (at > (int)g_editor.lines.size()) at = (int)g_editor.lines.size();
            insertLinesAt(at, ins, outCy, outCx);
            g_editor.cy = outCy;
            g_editor.cx = 0;
            viMarkChanged();
            viEnterInsert();
            return;
        }
        case 'O': {
            recordUndoSnapshot(UndoGroup::Structural);
            std::vector<std::string> ins(1, "");
            int outCy = 0, outCx = 0;
            insertLinesAt(g_editor.cy, ins, outCy, outCx);
            g_editor.cy = outCy;
            g_editor.cx = 0;
            viMarkChanged();
            viEnterInsert();
            return;
        }
        case 'v': viStartVisual(ViMode::Visual); return;
        case 'V': viStartVisual(ViMode::VisualLine); return;
        case 'x': {
            std::string &line = g_editor.lines[g_editor.cy];
            int end = g_editor.cx;
            for (int i = 0; i < count && end < (int)line.length(); i++) end = utf8Next(line, end);
            if (end > g_editor.cx) {
                recordUndoSnapshot(UndoGroup::Delete);
                std::string text = line.substr(g_editor.cx, end - g_editor.cx);
                viWriteRegister(v.reg, text, false);
                viDeleteCharwise({g_editor.cy, g_editor.cx}, {g_editor.cy, end});
                viClampNormal();
            }
            v.reg = 0; v.count = 0;
            return;
        }
        case 'X': {
            std::string &line = g_editor.lines[g_editor.cy];
            int start = g_editor.cx;
            for (int i = 0; i < count && start > 0; i++) start = utf8Prev(line, start);
            if (start < g_editor.cx) {
                recordUndoSnapshot(UndoGroup::Delete);
                std::string text = line.substr(start, g_editor.cx - start);
                viWriteRegister(v.reg, text, false);
                viDeleteCharwise({g_editor.cy, start}, {g_editor.cy, g_editor.cx});
                viClampNormal();
            }
            v.reg = 0; v.count = 0;
            return;
        }
        case 's': {
            std::string &line = g_editor.lines[g_editor.cy];
            int end = g_editor.cx;
            for (int i = 0; i < count && end < (int)line.length(); i++) end = utf8Next(line, end);
            if (end > g_editor.cx) {
                recordUndoSnapshot(UndoGroup::Structural);
                std::string text = line.substr(g_editor.cx, end - g_editor.cx);
                viWriteRegister(v.reg, text, false);
                viDeleteCharwise({g_editor.cy, g_editor.cx}, {g_editor.cy, end});
            }
            v.reg = 0; v.count = 0;
            viEnterInsert();
            return;
        }
        case 'S': {
            recordUndoSnapshot(UndoGroup::Structural);
            viWriteRegister(v.reg, viLineRangeText(g_editor.cy, g_editor.cy), true);
            viChangeLines(g_editor.cy, g_editor.cy);
            v.reg = 0; v.count = 0;
            viEnterInsert();
            return;
        }
        case 'D': {
            recordUndoSnapshot(UndoGroup::Delete);
            std::string &line = g_editor.lines[g_editor.cy];
            if (g_editor.cx < (int)line.length()) {
                viWriteRegister(v.reg, line.substr(g_editor.cx), false);
                viDeleteCharwise({g_editor.cy, g_editor.cx}, {g_editor.cy, (int)line.length()});
                viClampNormal();
            }
            v.reg = 0; v.count = 0;
            return;
        }
        case 'C': {
            recordUndoSnapshot(UndoGroup::Structural);
            std::string &line = g_editor.lines[g_editor.cy];
            if (g_editor.cx < (int)line.length()) {
                viWriteRegister(v.reg, line.substr(g_editor.cx), false);
                viDeleteCharwise({g_editor.cy, g_editor.cx}, {g_editor.cy, (int)line.length()});
            }
            v.reg = 0; v.count = 0;
            viEnterInsert();
            return;
        }
        case 'Y':
            viWriteRegister(v.reg, viLineRangeText(g_editor.cy,
                std::min(g_editor.cy + count - 1, (int)g_editor.lines.size() - 1)), true);
            v.reg = 0; v.count = 0;
            return;
        case 'p': viPaste(true); v.count = 0; return;
        case 'P': viPaste(false); v.count = 0; return;
        case 'u':
            undoEditor();
            viEnterNormal(true);
            v.count = 0;
            return;
        case 0x12: case KEY_REDO:
            redoEditor();
            viEnterNormal(true);
            v.count = 0;
            return;
        case '~': {
            std::string &line = g_editor.lines[g_editor.cy];
            int end = g_editor.cx;
            for (int i = 0; i < count && end < (int)line.length(); i++) end = utf8Next(line, end);
            if (end > g_editor.cx) {
                recordUndoSnapshot(UndoGroup::Structural);
                viToggleCaseRange({g_editor.cy, g_editor.cx}, {g_editor.cy, end});
            }
            v.count = 0;
            return;
        }
        case 'J':
            viJoinLines(count);
            v.count = 0;
            return;
        case '/': case '?': case ':':
            viStartCmdline((char)key);
            v.count = 0;
            return;
        case 'n':
        case 'N': {
            if (v.lastSearch.empty()) { viSetMsg("无上次搜索"); v.count = 0; return; }
            bool forward = (key == 'n') ? v.lastSearchForward : !v.lastSearchForward;
            viSearchApply(v.lastSearch, forward, count);
            v.count = 0;
            return;
        }
        case 0x01: {  // Ctrl+A 全选并进入 Visual
            g_editor.vi.mode = ViMode::Visual;
            g_editor.selAnchorCy = 0;
            g_editor.selAnchorCx = 0;
            g_editor.cy = (int)g_editor.lines.size() - 1;
            g_editor.cx = viLastCharPos(g_editor.lines[g_editor.cy]);
            viClampNormal();
            g_editor.hasSelection = true;
            v.count = 0;
            return;
        }
        case 0x16: viPaste(true); v.count = 0; return;  // Ctrl+V = p
        case 0x03: case 0x18: v.count = 0; return;      // normal 下无操作
        case KEY_SHIFT_LEFT: case KEY_SHIFT_RIGHT: case KEY_SHIFT_UP: case KEY_SHIFT_DOWN: {
            viStartVisual(ViMode::Visual);
            ViMotion m;
            int mk = key == KEY_SHIFT_LEFT ? KEY_LEFT : key == KEY_SHIFT_RIGHT ? KEY_RIGHT :
                     key == KEY_SHIFT_UP ? KEY_UP : KEY_DOWN;
            if (viMotionKey(mk, count, m)) viApplyMotion(m);
            v.count = 0;
            return;
        }
        case KEY_IME_TOGGLE:
            v.count = 0;
            return;
        default: break;
    }

    ViMotion m;
    if (viMotionKey(key, count, m)) {
        viApplyMotion(m);
        v.count = 0;
        return;
    }

    // 未列出的 Ctrl 键交回原快捷键(Ctrl+S/Q/O/E/Y/Z 等)
    if (key > 0 && key < 0x20) {
        handled = false;
        return;
    }
    // 其他可打印/特殊键在 normal 下吞掉, 不进入正文
    v.count = 0;
}

// ── vi 分发入口 ──────────────────────────────────────────────────────────
static AppState screen_editor_vi_handle(int key, ScreenContext &ctx, bool &handled) {
    handled = false;
    if (!g_settings.viMode() || key == 0) return APP_EDITOR;
    ViState &v = g_editor.vi;

    if (v.cmdlineActive) {
        handled = true;
        return viCmdlineKey(key, ctx);
    }

    if (v.mode == ViMode::Insert) {
        if (key == 0x1B) {
            handled = true;
            if (g_ime.composing()) g_ime.cancelComposition();
            else viLeaveInsert();
        }
        return APP_EDITOR;
    }

    // normal / visual: 输入法一律关闭
    handled = true;
    if (g_editor.imeActive) {
        g_editor.imeActive = false;
        g_ime.setActive(false);
    }
    g_editor.hasSelection = (v.mode == ViMode::Visual || v.mode == ViMode::VisualLine);

    bool needDraw = true;
    AppState out = APP_EDITOR;
    if (v.mode == ViMode::Normal) {
        viNormalKey(key, ctx, needDraw, out, handled);
    } else {
        viVisualKey(key, needDraw, handled);
    }
    return out;
}

// ── Screen entry points ──────────────────────────────────────────────────
void screen_editor_init(ScreenContext &ctx) {
    g_editor.lines.clear();
    g_editor.autoSaveTime = 0;
    g_editor.savedFilename = ctx.editFilename;

    // 快捷/文件编辑主会话: 有传入内容(灵感/日记编辑)时不走主文件会话
    bool quickFile = g_quickEdit && ctx.editContent.empty() && g_editor.savedFilename.empty();
    bool fileEditFile = g_fileEdit && ctx.editContent.empty() && g_editor.savedFilename.empty();
    if (quickFile) {
        loadQuickEditFile();
    } else if (fileEditFile) {
        loadFileEditCurrent();
    } else if (!ctx.editContent.empty()) {
        size_t pos = 0;
        while (pos < ctx.editContent.length()) {
            size_t nl = ctx.editContent.find('\n', pos);
            g_editor.lines.push_back((nl == std::string::npos) ? ctx.editContent.substr(pos) : ctx.editContent.substr(pos, nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        while (g_editor.lines.size() > 1 && g_editor.lines.back().empty())
            g_editor.lines.pop_back();
        g_editor.cx = (int)g_editor.lines.back().length();
        g_editor.cy = (int)g_editor.lines.size() - 1;
        ctx.editContent.clear();
    } else {
        g_editor.lines.push_back("");
        g_editor.cx = g_editor.cy = 0;
    }

    g_editor.scroll = 0;
    g_editor.targetCx = -1;
    // 进入编辑器默认打开中文输入法(半角拼音),与输入模式(打字机/正常)无关。
    g_editor.imeActive = true;
    g_ime.setFullwidth(false);
    g_ime.setEnglish(false);
    g_ime.setActive(true);
    g_ime.setDocumentContext(editorImeContextText());
    g_editor.vi = ViState{};
    g_editor.cachedGutterCells = -1;
    if (g_settings.viMode()) {
        g_editor.vi.mode = ViMode::Normal;
        g_editor.vi.insertImePref = g_editor.imeActive;
        g_editor.imeActive = false;
        g_ime.setActive(false);
        if (g_editor.cx > 0) g_editor.cx = utf8Prev(g_editor.lines[g_editor.cy], g_editor.cx);
    }
    g_editor.confirmSave = false;
    g_editor.modifiedSinceSave = false;
    g_editor.drawnOnce = false;
    markDirty();
    g_editor.promptText = ctx.promptText;
    g_editor.titleOverride = ctx.editorTitle;
    g_editor.promptMode = ctx.promptMode && !quickFile && !fileEditFile;
    ctx.editFilename.clear();
    ctx.editorTitle.clear();

    // 重置查找/替换对话框(重开编辑器时关闭)
    g_editor.search.active = false;
    g_editor.search.imeActive = false;
    g_editor.search.matches.clear();
    g_editor.search.cur = -1;

    // 重置快捷键帮助对话框
    g_editor.helpActive = false;
    g_editor.helpScroll = 0;
    g_editor.fileMgr.active = false;
    g_editor.fileMgr.mode = FileMgrMode::Browse;
    g_editor.cachedGutterCells = -1;
    clearUndoHistory();

    std::string recoveryContent, recoveryMeta;
    if (g_settings.recoveryDraft() && g_journal.loadRecoveryDraft(recoveryContent, recoveryMeta)) {
        g_editor.recoveryPrompt = true;
        g_editor.recoveryContent = recoveryContent;
        g_editor.recoveryMeta = recoveryMeta;
    } else {
        g_editor.recoveryPrompt = false;
        g_editor.recoveryContent.clear();
        g_editor.recoveryMeta.clear();
    }
    g_editor.lastRecoveryHash = 0;
    g_editor.promptGenerating = false;
    s_promptTaskDone = false;
}

AppState screen_editor_handle(int key, ScreenContext &ctx) {
    const auto& vrows = getVrows();

    if (g_editor.promptGenerating) {
        if (!s_promptTaskDone) {
            drawPromptGenerating();
            return APP_EDITOR;
        }
        g_editor.promptGenerating = false;
        lockPromptResult();
        DeepseekResult result = s_promptTaskResult;
        unlockPromptResult();
        if (result.success && !result.content.empty()) {
            g_editor.promptMode = true;
            g_editor.promptText = result.content;
        } else {
            if (g_editor.promptText.empty()) g_editor.promptText = "今天发生了什么？";
            if (!result.content.empty()) {
                ctx.statusMessage = result.content;
                ctx.statusDuration = 30;
            }
        }
        s_promptTaskContext.clear();
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }

    if (g_editor.recoveryPrompt) {
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            std::string fn = metaValue(g_editor.recoveryMeta, "filename");
            if (!fn.empty()) g_editor.savedFilename = fn;
            std::string filePath = metaValue(g_editor.recoveryMeta, "file_path");
            if (inFileEditSession() && !filePath.empty()) fileEditSetCurrentPath(filePath);
            loadLinesIntoEditor(g_editor.recoveryContent);
            g_editor.scroll = 0;
            g_editor.targetCx = -1;
            g_editor.modifiedSinceSave = true;
            g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
            g_editor.recoveryPrompt = false;
            g_editor.recoveryContent.clear();
            g_editor.recoveryMeta.clear();
            markDirty();
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        if (key == 0x1B || key == 'n' || key == 'N') {
            g_journal.clearRecoveryDraft();
            g_editor.recoveryPrompt = false;
            g_editor.recoveryContent.clear();
            g_editor.recoveryMeta.clear();
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        ui_clear(); drawEditor(); drawRecoveryDialog(); ui_commit();
        return APP_EDITOR;
    }

    if (g_editor.confirmSave) {
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            g_editor.confirmSave = false;
            return finishEditor(ctx);
        }
        if (key == 0x1B || key == 'n' || key == 'N') {
            g_editor.confirmSave = false;
            ctx.nextState = ctx.prevState;
            return ctx.prevState;
        }
        ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
        return APP_EDITOR;
    }

    if (g_editor.fileMgr.active) {
        return screen_editor_file_manager_handle(key, ctx);
    }
    if (key == 0x05 && inFileEditSession()) {  // Ctrl+E → 文件管理器
        fileMgrOpen();
        drawFileManagerPanel();
        return APP_EDITOR;
    }

    // 查找/替换对话框 (Ctrl+/) — 模态子状态,处理所有按键
    if (g_editor.search.active) {
        return screen_editor_search_handle(key, ctx);
    }
    if (key == KEY_SEARCH) {
        searchOpen();
        ui_clear(); drawSearchPanel(); ui_commit();
        return APP_EDITOR;
    }

    // 快捷键帮助对话框 (Ctrl+?) — 模态子状态
    if (g_editor.helpActive) {
        return screen_editor_help_handle(key, ctx);
    }
    if (key == KEY_HELP) {
        g_editor.helpActive = true;
        g_editor.helpScroll = 0;
        ui_clear(); drawHelpPanel(); ui_commit();
        return APP_EDITOR;
    }

    // vi 模式: 必须在裸 Esc 退出与 IME 门之前分发。
    if (g_settings.viMode()) {
        bool viHandled = false;
        AppState st = screen_editor_vi_handle(key, ctx, viHandled);
        if (viHandled) {
            if (st != APP_EDITOR) return st;
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
    }

    if (key == 0x1B) {
        if (inQuickFileSession() || inFileEditSession()) {
            // 快捷/文件编辑: 自动保存后跳到设置面板
            if (saveCurrentContent(false)) g_journal.clearRecoveryDraft();
            g_editor.modifiedSinceSave = false;
            ctx.nextState = APP_SETTINGS;
            return APP_SETTINGS;
        }
        bool hasContent = g_editor.lines.size() > 1 ||
            (g_editor.lines.size() == 1 && !g_editor.lines[0].empty());
        if (hasContent && g_editor.modifiedSinceSave) {
            g_editor.confirmSave = true;
            ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
            return APP_EDITOR;
        }
        ctx.nextState = ctx.prevState; return ctx.prevState;
    }

    // Ctrl+O → 选区润色(需先用Shift+方向键选择文字)
    if (key == 0x0F) {
        if (!g_editor.hasSelection) {
            ctx.statusMessage = "请先用Shift+方向键选择文字";
            ctx.statusDuration = 30;
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        screen_polish_set_scope(POLISH_SELECTION);
        return APP_POLISH;
    }

    // Ctrl+A → 全选
    if (key == 0x01) {
        if (g_editor.lines.size() == 1 && g_editor.lines[0].empty()) {
            // 空文档不建立空选区
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        g_editor.selAnchorCy = 0;
        g_editor.selAnchorCx = 0;
        g_editor.cy = (int)g_editor.lines.size() - 1;
        g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
        g_editor.hasSelection = true;
        g_editor.targetCx = -1;
        markDirty();
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }

    if (g_editor.imeActive && key != 0) {
        std::string imeOut;
        int64_t perfT0 = EDITOR_PERF_NOW();
        if (g_ime.handleKey(key, imeOut)) {
            int64_t perfT1 = EDITOR_PERF_NOW();
            std::string imeStatus = g_ime.takeStatusMessage();
            if (!imeStatus.empty()) {
                ctx.statusMessage = imeStatus;
                ctx.statusDuration = 30;
            }
            // 英文模式连续上屏两个候选词且中间无空格时, 自动补空格分隔
            if (g_ime.english() && !imeOut.empty() && !g_editor.hasSelection) {
                const std::string &line = g_editor.lines[g_editor.cy];
                if (g_editor.cx > 0) {
                    char prev = line[g_editor.cx - 1];
                    if (prev != ' ' && prev != '\t') imeOut.insert(imeOut.begin(), ' ');
                }
            }
            // 中文音效触发:key=每个被 IME 消费的键一声;count=上屏按字数连响;single=上屏一声
            if (g_settings.clickChineseMode() == "key") {
                typingClickPlay(1);
            } else if (g_settings.clickChineseMode() == "count") {
                int n = utf8Count(imeOut); if (n > 0) typingClickPlay(n);
            } else {
                if (!imeOut.empty()) typingClickPlay(1);
            }
            editorInsertText(imeOut);
            int64_t perfT2 = EDITOR_PERF_NOW();
            // 刚上屏的词要立刻进文档上下文: 同一篇里再次输入同一个人名/术语时它就该
            // 排在前面。空串(取消组合等)不必重扫。
            if (!imeOut.empty()) g_ime.setDocumentContext(editorImeContextText());
            int64_t perfT3 = EDITOR_PERF_NOW();
            ui_clear();
            int64_t perfT3a = EDITOR_PERF_NOW();
            drawEditor();
            int64_t perfT3b = EDITOR_PERF_NOW();
            ui_commit();
            int64_t perfT4 = EDITOR_PERF_NOW();
#if PJOURNAL_IME_PERF_LOG
            {
                int64_t total = perfT4 - perfT0;
                if (total >= EDITOR_PERF_SLOW_US) {
                    size_t docBytes = 0;
                    for (const auto &l : g_editor.lines) docBytes += l.size();
                    int64_t layoutUs = (s_perfLayoutEnd > s_perfDrawStart)
                                           ? s_perfLayoutEnd - s_perfDrawStart : 0;
                    int64_t drawUs = perfT3b - perfT3a;
                    ESP_LOGW(EDITOR_PERF_TAG,
                             "commit 合计=%lldus ime=%lld insert=%lld ctx=%lld 重绘=%lld "
                             "[清屏=%lld 绘制=%lld(布局=%lld) 提交=%lld] | 上屏=%uB 行=%u 正文=%uB",
                             (long long)total,
                             (long long)(perfT1 - perfT0),
                             (long long)(perfT2 - perfT1),
                             (long long)(perfT3 - perfT2),
                             (long long)(perfT4 - perfT3),
                             (long long)(perfT3a - perfT3),
                             (long long)drawUs,
                             (long long)layoutUs,
                             (long long)(perfT4 - perfT3b),
                             (unsigned)imeOut.size(),
                             (unsigned)g_editor.lines.size(),
                             (unsigned)docBytes);
                }
            }
#endif
            (void)perfT0; (void)perfT1; (void)perfT2; (void)perfT3;
            (void)perfT3a; (void)perfT3b; (void)perfT4;
            return APP_EDITOR;
        }
    }

    if (key == 9) {
        recordUndoSnapshot(UndoGroup::Structural);
        g_editor.lines[g_editor.cy].insert(g_editor.cx, 4, ' ');
        g_editor.cx += 4;
        g_editor.targetCx = -1;
        markDirty();
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x0E) { // Ctrl+N → 下一个快捷编辑文件
        if (inQuickFileSession()) {
            quickEditSwitchTo((quickEditIndex() + 1) % 10);
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
    }
    if (key == 0x10) { // Ctrl+P
        if (inQuickFileSession()) {  // 快捷编辑: 上一个文件
            quickEditSwitchTo((quickEditIndex() + 9) % 10);
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        std::string exp = g_settings.personalExperience();
        std::string hob = g_settings.personalHobbies();
        s_promptTaskContext.clear();
        if (!exp.empty()) s_promptTaskContext += "我的经历:" + exp + ";";
        if (!hob.empty()) s_promptTaskContext += "我的爱好:" + hob + ";";
        if (s_promptTaskContext.empty()) s_promptTaskContext = "一个普通用户";
        lockPromptResult();
        s_promptTaskResult = {false, ""};
        unlockPromptResult();
        s_promptTaskDone = false;
        g_editor.promptGenerating = true;
        TaskHandle_t h = nullptr;
        if (xTaskCreate(runPromptTask, "prompt_gen", 8192, nullptr, 1, &h) != pdPASS) {
            g_editor.promptGenerating = false;
            ctx.statusMessage = "系统繁忙,请重试";
            ctx.statusDuration = 30;
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        drawPromptGenerating();
        return APP_EDITOR;
    }
    if (key == 0x13) {
        std::string text = currentEditorText();
        if (!text.empty() || inQuickFileSession() || inFileEditSession()) {
            if (saveCurrentContent()) ctx.statusMessage = "已保存";
            else ctx.statusMessage = "保存失败";
        }
        ui_clear(); drawEditor(); ui_commit();
        g_editor.autoSaveTime = 0;
        g_editor.modifiedSinceSave = false;
        return APP_EDITOR;
    }  // Ctrl+S
    if (key == 0x11) {
        if (inQuickFileSession() || inFileEditSession()) {
            if (saveCurrentContent(false)) g_journal.clearRecoveryDraft();
            g_editor.modifiedSinceSave = false;
            ctx.nextState = APP_SETTINGS;
            return APP_SETTINGS;
        }
        ctx.nextState = ctx.prevState; return ctx.prevState;
    }  // Ctrl+Q
    if (key == 0x06) {  // Ctrl+F
        ctx.nextState = APP_SYNC_SEND_FLOMO;
        return APP_SYNC_SEND_FLOMO;
    }
    if (key == 0x1A) {  // Ctrl+Z
        if (!undoEditor()) {
            ctx.statusMessage = "没有可撤销内容";
            ctx.statusDuration = 30;
        }
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == KEY_REDO || key == 0x12) {  // Ctrl+Shift+Z / Ctrl+R
        if (!redoEditor()) {
            ctx.statusMessage = "没有可重做内容";
            ctx.statusDuration = 30;
        }
        ui_clear(); drawEditor(); ui_commit();
        return APP_EDITOR;
    }
    if (key == 0x19) {  // Ctrl+Y → 当前日记历史版本
        if (inQuickFileSession()) {
            ctx.statusMessage = "快捷编辑历史暂未支持";
            ctx.statusDuration = 30;
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        if (!saveCurrentContent()) {
            ctx.statusMessage = "保存失败，无法查看历史";
            ctx.statusDuration = 30;
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
        g_editor.modifiedSinceSave = false;
        ctx.selectedEntry = g_editor.savedFilename;
        ctx.prevState = APP_EDITOR;
        ctx.nextState = APP_HISTORY;
        return APP_HISTORY;
    }
    if (key >= KEY_FILE_BASE && key <= KEY_FILE_BASE + 9) {  // Ctrl+0-9 直接切换文件
        if (inQuickFileSession()) {
            quickEditSwitchTo(key - KEY_FILE_BASE);
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
    }

    // ── Heading fold toggle (Ctrl+T) ─────────────────────────────────
    if (key == 0x14) { // Ctrl+T — 折叠/展开当前标题下的正文
        const auto &mi = getMdInfo(g_settings.markdownRender());
        if (g_editor.cy >= 0 && g_editor.cy < (int)mi.size() && mi[g_editor.cy].headingLevel > 0) {
            if (!g_editor.foldedHeadings.erase(g_editor.cy))
                g_editor.foldedHeadings.insert(g_editor.cy);
            g_editor.vrowsDirty = true;  // 纯视图态,不进撤销快照
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    // ── Clipboard operations (Ctrl+C, Ctrl+X, Ctrl+V) ────────────────
    if (key == 0x03) { // Ctrl+C — copy
        if (g_editor.hasSelection) {
            g_clipboard = getSelectedText();
            clearSelection();
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x18) { // Ctrl+X — cut
        if (g_editor.hasSelection) {
            recordUndoSnapshot(UndoGroup::Structural);
            g_clipboard = getSelectedText();
            deleteSelection();
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x16) { // Ctrl+V — paste
        if (!g_clipboard.empty()) {
            recordUndoSnapshot(UndoGroup::Structural);
            if (g_editor.hasSelection) {
                deleteSelection(); // removes selection, then paste at cursor
            }
            int outCy = g_editor.cy, outCx = g_editor.cx;
            insertMultilineAt(g_editor.cy, g_editor.cx, g_clipboard, outCy, outCx);
            g_editor.cy = outCy;
            g_editor.cx = outCx;
            g_editor.targetCx = -1;
            markDirty();
            g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
            g_editor.modifiedSinceSave = true;
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    if (editorVertical() && (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT ||
                             key == KEY_RIGHT || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN)) {
        clearSelection();
        VerticalLayoutMetrics vm = editorVerticalVm();
        bool mdOn = g_settings.markdownRender();
        mdSetRenderEnabled(mdOn);
        const auto &mdInfo = getMdInfo(mdOn);
        auto hidden = mdFoldHiddenLines(g_editor.lines, mdOn ? &mdInfo : nullptr,
                                        &g_editor.foldedHeadings);
        auto data = buildVerticalData(g_editor.lines, vm.rows, &hidden,
                                      mdOn ? &mdInfo : nullptr, &g_editor.foldedHeadings);
        if (key == KEY_UP) {
            moveCursorVerticalInline(-1);
        } else if (key == KEY_DOWN) {
            moveCursorVerticalInline(1);
        } else if (key == KEY_LEFT) {
            moveCursorVerticalColumn(1, data);
        } else if (key == KEY_RIGHT) {
            moveCursorVerticalColumn(-1, data);
        } else if (key == KEY_PAGE_UP) {
            moveCursorVerticalColumn(-vm.cols, data);
        } else if (key == KEY_PAGE_DOWN) {
            moveCursorVerticalColumn(vm.cols, data);
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    // ── Shift+arrow: extend selection ─────────────────────────────────
    if (key == KEY_SHIFT_LEFT) {
        if (g_editor.cx > 0) {
            extendSelection();
            g_editor.cx--;
            while (g_editor.cx > 0 && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx--;
        } else if (g_editor.cy > 0) {
            extendSelection();
            g_editor.cy--;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
        }
        g_editor.targetCx = -1;
        markDirty();
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == KEY_SHIFT_RIGHT) {
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            extendSelection();
            g_editor.cx++;
            while (g_editor.cx < (int)g_editor.lines[g_editor.cy].length() && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx++;
        } else if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            extendSelection();
            g_editor.cy++;
            g_editor.cx = 0;
        }
        g_editor.targetCx = -1;
        markDirty();
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == KEY_SHIFT_UP) {
        if (g_editor.cy > 0) {
            extendSelection();
        }
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR > 0) {
            auto &prev = vrows[curVR - 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % editorTextCells();
            g_editor.cy = prev.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], prev.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = cellsToByte(g_editor.lines[g_editor.cy], prev.start, prev.end, targetCells);
        }
        markDirty();
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == KEY_SHIFT_DOWN) {
        if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            extendSelection();
        }
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
            int visualCol = g_editor.targetCx % editorTextCells();
            g_editor.cy = next.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], next.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = std::min(cellsToByte(g_editor.lines[g_editor.cy], next.start, next.end, targetCells),
                                   (int)g_editor.lines[g_editor.cy].length());
        }
        markDirty();
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }

    // Navigation & editing
    if (key == 0x0A || key == 0x0D) { // Enter
        recordUndoSnapshot(UndoGroup::Structural);
        if (g_editor.hasSelection) deleteSelection();
        // 列表行在行尾回车时自动续行:下一行带上同款列表标记
        std::string prefix;
        if (g_editor.cx >= (int)g_editor.lines[g_editor.cy].length()) {
            const std::string &cur = g_editor.lines[g_editor.cy];
            MdListMarker m = mdListMarker(cur);
            if (m.ok) {
                bool emptyItem = cur.substr(m.start + m.len).find_first_not_of(" \t") == std::string::npos;
                if (!emptyItem) {
                    std::string lead = cur.substr(0, m.start);  // 嵌套缩进
                    if (m.task) {
                        prefix = lead + "- [ ] ";
                    } else if (m.ordered) {
                        int d = m.start;
                        while (d < (int)cur.length() && cur[d] >= '0' && cur[d] <= '9') d++;
                        if (d == m.start) {  // 中文序号:一、二、十、… 递增(一→二→…→十→十一)
                            int nlen = 0;
                            int n = mdCnNumValue(cur, m.start, nlen);
                            if (n >= 0)
                                prefix = lead + mdCnNumeral(n + 1) + cur.substr(m.start + nlen, m.len - nlen);
                            else
                                prefix = lead + cur.substr(m.start, m.len);
                        } else {
                            int n = 0;
                            for (int k = m.start; k < d; k++) n = n * 10 + (cur[k] - '0');
                            n++;
                            char num[16];
                            snprintf(num, sizeof(num), "%d", n);
                            prefix = lead + num + cur.substr(d, m.len - (d - m.start));
                        }
                    } else {
                        prefix = lead + cur.substr(m.start, 1) + " ";  // 保留 -/*/+
                    }
                }
            }
        }
        std::string rest = g_editor.lines[g_editor.cy].substr(g_editor.cx);
        g_editor.lines[g_editor.cy] = g_editor.lines[g_editor.cy].substr(0, g_editor.cx);
        g_editor.cx = 0; g_editor.cy++;
        g_editor.lines.insert(g_editor.lines.begin() + g_editor.cy, prefix + rest);
        foldLinesInserted(g_editor.cy, 1);
        g_editor.cx = (int)prefix.length();  // 光标落在续行标记之后
        g_editor.targetCx = -1;
        markDirty();
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
    } else if (key == 0x7F || key == 0x08) { // Backspace
        if (g_editor.hasSelection) {
            recordUndoSnapshot(UndoGroup::Delete);
            deleteSelection();
        } else if (g_editor.cx > 0) {
            recordUndoSnapshot(UndoGroup::Delete);
            int prev = g_editor.cx - 1;
            while (prev > 0 && ((unsigned char)g_editor.lines[g_editor.cy][prev] & 0xC0) == 0x80) prev--;
            g_editor.lines[g_editor.cy].erase(prev, g_editor.cx - prev);
            g_editor.cx = prev;
            if (g_editor.imeActive) g_ime.handleHostBackspace();
        } else if (g_editor.cy > 0) {
            recordUndoSnapshot(UndoGroup::Delete);
            g_editor.cx = (int)g_editor.lines[g_editor.cy-1].length();
            g_editor.lines[g_editor.cy-1] += g_editor.lines[g_editor.cy];
            g_editor.lines.erase(g_editor.lines.begin() + g_editor.cy);
            foldLinesErased(g_editor.cy, 1);
            g_editor.cy--;
        }
        g_editor.targetCx = -1;
        markDirty();
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
    } else if (key >= 0x20 && key <= 0x7E) { // ASCII printable
        recordUndoSnapshot(UndoGroup::Typing);
        if (g_editor.hasSelection) deleteSelection();
        g_editor.lines[g_editor.cy].insert(g_editor.cx, 1, (char)key);
        g_editor.cx++;
        g_editor.targetCx = -1;
        markDirty();
        g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
        g_editor.modifiedSinceSave = true;
        typingClickPlay(1);
    } else if (key == KEY_LEFT) {
        clearSelection();
        if (g_editor.cx > 0) {
            g_editor.cx--;
            while (g_editor.cx > 0 && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx--;
        } else if (g_editor.cy > 0) {
            g_editor.cy--;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_RIGHT) {
        clearSelection();
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            g_editor.cx++;
            while (g_editor.cx < (int)g_editor.lines[g_editor.cy].length() && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx++;
        } else if (g_editor.cy < (int)g_editor.lines.size() - 1) {
            g_editor.cy++;
            g_editor.cx = 0;
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_UP) {
        clearSelection();
        int curVR = -1;
        for (int vi = 0; vi < (int)vrows.size(); vi++) {
            if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
                curVR = vi; break;
            }
        }
        if (curVR > 0) {
            auto &prev = vrows[curVR - 1];
            if (g_editor.targetCx < 0)
                g_editor.targetCx = byteToCells(g_editor.lines[g_editor.cy], g_editor.cx);
            int visualCol = g_editor.targetCx % editorTextCells();
            g_editor.cy = prev.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], prev.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = cellsToByte(g_editor.lines[g_editor.cy], prev.start, prev.end, targetCells);
        }
    } else if (key == KEY_DOWN) {
        clearSelection();
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
            int visualCol = g_editor.targetCx % editorTextCells();
            g_editor.cy = next.lineIdx;
            int vrowStartCells = byteToCells(g_editor.lines[g_editor.cy], next.start);
            int targetCells = vrowStartCells + visualCol;
            g_editor.cx = std::min(cellsToByte(g_editor.lines[g_editor.cy], next.start, next.end, targetCells),
                                   (int)g_editor.lines[g_editor.cy].length());
        }
    } else if (key == KEY_HOME) {
        clearSelection();
        g_editor.cx = 0;
        g_editor.targetCx = -1;
    } else if (key == KEY_END) {
        clearSelection();
        g_editor.cx = (int)g_editor.lines[g_editor.cy].length();
        g_editor.targetCx = -1;
    } else if (key == KEY_PAGE_UP) {
        clearSelection();
        moveCursorVertical(-editorPageRows(), vrows);
    } else if (key == KEY_PAGE_DOWN) {
        clearSelection();
        moveCursorVertical(editorPageRows(), vrows);
    }

    ui_clear(); drawEditor(); ui_commit();
    return APP_EDITOR;
}

// ── Idle tick (no key) ────────────────────────────────────────────────────
// main loop calls this instead of screen_editor_handle(0, ctx). Runs the
// auto-save schedule and only repaints when the screen may be stale, avoiding
// a full redraw every 50ms idle tick.
bool screen_editor_idle(ScreenContext &ctx, bool forceRedraw) {
    if (g_editor.promptGenerating) {
        if (s_promptTaskDone) {
            g_editor.promptGenerating = false;
            lockPromptResult();
            DeepseekResult result = s_promptTaskResult;
            unlockPromptResult();
            if (result.success && !result.content.empty()) {
                g_editor.promptMode = true;
                g_editor.promptText = result.content;
            } else {
                if (g_editor.promptText.empty()) g_editor.promptText = "今天发生了什么？";
                // 空闲路径也要把失败原因显示出来, 否则看起来像"什么都没发生"。
                if (!result.content.empty()) {
                    ctx.statusMessage = result.content;
                    ctx.statusDuration = 30;
                }
            }
            s_promptTaskContext.clear();
            ui_clear(); drawEditor(); ui_commit();
            return true;
        }
        drawPromptGenerating();
        return true;
    }
    if (g_editor.recoveryPrompt) {
        if (forceRedraw || !g_editor.drawnOnce) {
            ui_clear(); drawEditor(); drawRecoveryDialog(); ui_commit();
            return true;
        }
        return false;
    }
    // Auto-save on idle ticks (快捷编辑/文件编辑始终自动保存)
    if (g_editor.autoSaveTime > 0 && esp_timer_get_time() > g_editor.autoSaveTime) {
        g_editor.autoSaveTime = 0;
        bool shouldCommit = inQuickFileSession() || inFileEditSession() || g_settings.autoSave();
        // 诊断: 这是编辑时**最高频**的 SD 写 —— autoSaveTime 每次按键都重新武装成
        // "3 秒后", 所以只要打字间歇超过 3 秒就会触发一次整篇重写。用户报的"输入
        // 词组偶尔卡一下"最可能就落在这一笔上, 而不是用户词典那几条。
        if (shouldCommit) {
            IO_PROBE("编辑器自动保存(整篇)", "");
            if (saveCurrentContent(false)) {
                g_editor.modifiedSinceSave = false;
            } else if (g_editor.modifiedSinceSave && g_settings.recoveryDraft()) {
                saveRecoveryDraftIfChanged();
            }
        } else if (g_editor.modifiedSinceSave && g_settings.recoveryDraft()) {
            IO_PROBE("编辑器恢复草稿", "");
            saveRecoveryDraftIfChanged();
        }
    }
    if (g_editor.fileMgr.active) {
        if (forceRedraw || !g_editor.drawnOnce) {
            drawFileManagerPanel();
            return true;
        }
        return false;
    }
    // 查找/替换对话框打开时,面板只在按键时变化;空闲重绘走面板
    if (g_editor.search.active) {
        if (forceRedraw || !g_editor.drawnOnce) {
            ui_clear(); drawSearchPanel(); ui_commit();
            return true;
        }
        return false;
    }
    // 快捷键帮助对话框同理
    if (g_editor.helpActive) {
        if (forceRedraw || !g_editor.drawnOnce) {
            ui_clear(); drawHelpPanel(); ui_commit();
            return true;
        }
        return false;
    }
    if (forceRedraw || !g_editor.drawnOnce) {
        ui_clear(); drawEditor(); ui_commit();
        return true;
    }
    return false;
}

void screen_editor_reset_drawn() {
    g_editor.drawnOnce = false;
}

// 查找/替换对话框是否打开(供 main.cpp 屏蔽全局按键)
bool app_editor_search_active() {
    return g_editor.search.active;
}

// 快捷键帮助对话框是否打开(供 main.cpp 屏蔽全局按键)
bool app_editor_help_active() {
    return g_editor.helpActive;
}

// vi normal/visual 或命令行激活时, 全局 Ctrl+Space 不得偷偷打开输入法;
// '/' '?' 命令栏自己的 Ctrl+Space 由编辑器处理。
bool app_editor_vi_ime_blocked() {
    if (!g_settings.viMode() || s_skipStatusBarAndIme) return false;
    if (g_editor.vi.cmdlineActive) return true;
    return g_editor.vi.mode != ViMode::Insert;
}

// 命令行激活时, 物理按键浮层与全局 Ctrl+I 不应抢键。
bool app_editor_vi_cmdline_active() {
    return g_settings.viMode() && g_editor.vi.cmdlineActive;
}

// ── App-level helpers ────────────────────────────────────────────────────
bool app_ime_active() {
    return g_editor.imeActive;
}

void app_toggle_ime() {
    g_editor.imeActive = !g_editor.imeActive;
    g_ime.setActive(g_editor.imeActive);
    if (g_editor.imeActive) g_ime.setDocumentContext(editorImeContextText());
}

bool app_ime_fullwidth() {
    return g_ime.fullwidth();
}

void app_toggle_fullwidth() {
    g_ime.toggleFullwidth();
}

void app_toggle_trad() {
    g_ime.toggleTrad();
}

void app_toggle_english() {
    g_ime.toggleEnglish();
}

void app_toggle_ime_delete_mode() {
    g_ime.toggleDeleteMode();
}

static bool g_editorNeedsReinit = false;

void app_editor_request_reinit() {
    g_editorNeedsReinit = true;
}

bool app_editor_needs_reinit() {
    if (g_editorNeedsReinit) {
        g_editorNeedsReinit = false;
        return true;
    }
    return false;
}

void app_editor_stash_session() {
    if (s_hasStashedEditor) return;
    s_stashedEditor = g_editor;
    s_hasStashedEditor = true;
    g_ime.cancelComposition();
}

void app_editor_restore_stashed_session() {
    if (!s_hasStashedEditor) return;
    g_editor = s_stashedEditor;
    s_hasStashedEditor = false;
    g_editor.drawnOnce = false;
    g_ime.setActive(g_editor.imeActive);
    if (g_editor.imeActive) g_ime.setDocumentContext(editorImeContextText());
    markDirty();
}

bool app_editor_has_stashed_session() {
    return s_hasStashedEditor;
}

std::string app_get_editor_text() {
    return currentEditorText();
}

// 在 (cy,cx) 插入可含 '\n' 的文本, 跨行时拆成多条硬行。不记撤销、不置脏、不设
// 自动保存, 由调用方负责; 行数变化自动平移折叠标题。
static void insertMultilineAt(int cy, int cx, const std::string &text,
                              int &outCy, int &outCx) {
    if (cy < 0 || cy >= (int)g_editor.lines.size()) {
        outCy = cy < 0 ? 0 : (int)g_editor.lines.size() - 1;
        outCx = 0;
        return;
    }
    // 防御越界: std::string::insert 的 pos 超过长度会抛异常并直接 abort。
    if (cx < 0) cx = 0;
    if (cx > (int)g_editor.lines[cy].length()) cx = (int)g_editor.lines[cy].length();
    outCy = cy;
    outCx = cx;
    if (text.empty()) return;
    if (text.find('\n') == std::string::npos) {
        g_editor.lines[cy].insert(cx, text);
        outCx = cx + (int)text.length();
        return;
    }
    std::vector<std::string> ins;
    size_t pos = 0;
    while (pos < text.length()) {
        size_t nl = text.find('\n', pos);
        ins.push_back(nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (ins.empty()) return;
    if (ins.size() > 1 && ins.back().empty()) ins.pop_back();

    std::string tail = g_editor.lines[cy].substr(cx);
    g_editor.lines[cy] = g_editor.lines[cy].substr(0, cx) + ins[0];
    if (ins.size() == 1) {
        g_editor.lines[cy] += tail;
        outCx = cx + (int)ins[0].length();
        return;
    }
    int insertAt = cy + 1;
    for (size_t i = 1; i + 1 < ins.size(); i++)
        g_editor.lines.insert(g_editor.lines.begin() + insertAt++, ins[i]);
    g_editor.lines.insert(g_editor.lines.begin() + insertAt, ins.back() + tail);
    foldLinesInserted(cy + 1, (int)ins.size() - 1);
    outCy = insertAt;
    outCx = (int)ins.back().length();
}

// 在硬行索引 at 处插入若干整行。不记撤销、不置脏; 光标落到首条插入行行首。
static void insertLinesAt(int at, const std::vector<std::string> &lines,
                          int &outCy, int &outCx) {
    if (at < 0) at = 0;
    if (at > (int)g_editor.lines.size()) at = (int)g_editor.lines.size();
    g_editor.lines.insert(g_editor.lines.begin() + at, lines.begin(), lines.end());
    if (!lines.empty()) foldLinesInserted(at, (int)lines.size());
    outCy = at;
    outCx = 0;
}

// Insert text at the cursor, shared by IME commit and voice dictation.
void editorInsertText(const std::string &text) {
    if (text.empty()) return;
    recordUndoSnapshot(UndoGroup::Typing);
    if (g_editor.hasSelection) deleteSelection();
    int outCy = g_editor.cy, outCx = g_editor.cx;
    insertMultilineAt(g_editor.cy, g_editor.cx, text, outCy, outCx);
    g_editor.cy = outCy;
    g_editor.cx = outCx;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

// Replace the entire editor text (AI polish confirm). Cursor moves to the end.
void editorReplaceAllText(const std::string &text) {
    recordUndoSnapshot(UndoGroup::Structural);
    loadLinesIntoEditor(text);
    g_editor.scroll = 0;
    g_editor.targetCx = -1;
    g_editor.hasSelection = false;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}

// Currently selected text (empty when no selection).
std::string app_get_selected_text() {
    return getSelectedText();
}

// Replace only the selected text (selection polish confirm). Cursor ends at
// the end of the inserted text; the selection is cleared.
void editorReplaceSelection(const std::string &text) {
    // deleteSelection() removes the selection, joins its lines and puts the
    // cursor at the selection start — the natural insertion point.
    recordUndoSnapshot(UndoGroup::Structural);
    if (g_editor.hasSelection) deleteSelection();
    if (!text.empty()) {
        int outCy = g_editor.cy, outCx = g_editor.cx;
        insertMultilineAt(g_editor.cy, g_editor.cx, text, outCy, outCx);
        g_editor.cy = outCy;
        g_editor.cx = outCx;
    }
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}
