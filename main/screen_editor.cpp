#include "screen_editor.h"
#include "screen_polish.h"
#include "font_renderer.h"
#include "journal_storage.h"
#include "deepseek_client.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "quick_edit.h"
#include "ui_helpers.h"
#include "markdown_render.h"
#include "ime/IME.h"
#include <cstdio>
#include <cstring>
#include <ctime>
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

#define IME_CODE_Y (STATUS_BAR_Y - 2*FONT_H + g_font.ascent())
#define IME_CAND_Y (STATUS_BAR_Y - FONT_H + g_font.ascent() - 3)
#define EDITOR_MAX_CELLS (SCREEN_W / g_font.halfAdvance())

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

static struct {
    std::vector<std::string> lines;
    int cx = 0, cy = 0;
    int scroll = 0;
    int targetCx = -1;
    std::string promptText;
    bool promptMode = false;
    bool imeActive = false;
    bool confirmSave = false;
    bool vrowsDirty = true;
    std::vector<VRow> cachedVrows;
    bool cachedFirstLineIndent = false;
    int cachedWordCount = 0;
    bool wordCountDirty = true;
    bool mdInfoDirty = true;
    std::vector<MdLineInfo> cachedMdInfo;
    bool cachedMdOn = false;
    std::vector<EditorSnapshot> undoStack;
    std::vector<EditorSnapshot> redoStack;
    size_t undoBytes = 0;
    size_t redoBytes = 0;
    UndoGroup lastUndoGroup = UndoGroup::None;
    int64_t lastUndoTime = 0;
    int64_t autoSaveTime = 0;
    bool modifiedSinceSave = false;
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
} g_editor;

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
    if (end.cy > start.cy)
        g_editor.lines.erase(g_editor.lines.begin() + start.cy + 1,
                             g_editor.lines.begin() + end.cy + 1);
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
    int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
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

static const std::vector<VRow>& getVrows() {
    bool firstLineIndent = g_settings.firstLineIndent();
    if (g_editor.vrowsDirty || g_editor.cachedFirstLineIndent != firstLineIndent) {
        g_editor.cachedFirstLineIndent = firstLineIndent;
        g_editor.cachedVrows = buildVrows(g_editor.lines);
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

static bool saveRecoveryDraftIfChanged() {
    std::string text = currentEditorText();
    std::string meta;
    meta += std::string("mode=") + (inQuickFileSession() ? "quick" : "journal") + "\n";
    meta += "filename=" + g_editor.savedFilename + "\n";
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
    auto &sh = g_editor.search;
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
    std::string imeLabel;
    if (!sh.imeActive) imeLabel = "EN";
    else if (g_ime.english()) imeLabel = "[英]";
    else {
        imeLabel = "[中]";
        imeLabel += g_ime.fullwidth() ? "\xe2\x97\x8f" : "\xe2\x97\x90";
        imeLabel += g_ime.trad() ? "繁" : "简";
    }
    std::string right = imeLabel;
    std::string bt = battery_icon_status_text();
    if (!bt.empty()) right += " " + bt;
    ui_draw_status(sh.focusRep ? "替换字段" : "查找字段", right.c_str());

    if (sh.imeActive && g_ime.composing()) drawIMEUI(SCREEN_H - 67 - 4);
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
    "Ctrl+I灵感面板",
    "Ctrl+F发送Flomo",
    "Ctrl+Y历史版本",
    "Ctrl+N/P快捷编辑文件",
    "Ctrl+Space输入法开关",
    "Shift+Space全半角切换",
    "Ctrl+Shift+F简繁",
    "左Shift临时英文",
    "Home/End 行首/行尾",
    "PgUp/PgDn 翻页",
    "双击BOOT全文润色",
    "双击USER语音听写",
    "Ctrl+0-9快捷编辑文件切换",
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

// ── Editor drawing ────────────────────────────────────────────────────────
// When true, drawEditor renders the content but skips the IME strip and the
// status bar — used by the voice dictation screen as a transparent background.
static bool s_skipStatusBarAndIme = false;

static void drawEditor() {
    g_editor.drawnOnce = true;
    int y = FONT_H;

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
                y += LINE_SPACING;
            }
        }
        u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
        y += LINE_SPACING;
    }

    const auto& vrows = getVrows();
    bool composing = g_ime.composing() && !s_skipStatusBarAndIme;
    int contentEndY = composing ? IME_CODE_Y : STATUS_Y;
    int visibleVrows = (contentEndY - y + LINE_SPACING - 1) / LINE_SPACING;
    if (visibleVrows < 1) visibleVrows = 1;

    int cursorVR = -1;
    for (int vi = 0; vi < (int)vrows.size(); vi++) {
        if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
            cursorVR = vi;
            break;
        }
    }

    int normalVisibleVrows = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
    int effectiveVisibleVrows = composing ? (normalVisibleVrows - 2) : normalVisibleVrows;
    if (effectiveVisibleVrows < 1) effectiveVisibleVrows = 1;

    if (cursorVR < g_editor.scroll) g_editor.scroll = cursorVR;
    if (cursorVR >= g_editor.scroll + effectiveVisibleVrows)
        g_editor.scroll = cursorVR - effectiveVisibleVrows + 1;
    if (g_editor.scroll < 0) g_editor.scroll = 0;

    bool mdOn = g_settings.markdownRender();
    mdSetRenderEnabled(mdOn);
    const std::vector<MdLineInfo> &mdInfo = getMdInfo(mdOn);
    for (int i = 0; i < visibleVrows && (g_editor.scroll + i) < (int)vrows.size(); i++) {
        auto &vr = vrows[g_editor.scroll + i];
        int mdCursor = (vr.lineIdx == g_editor.cy) ? g_editor.cx : -1;
        mdDrawVrow(4, y + i * LINE_SPACING, g_editor.lines[vr.lineIdx], vr.start, vr.end,
                   mdInfo[vr.lineIdx], vr.indentCells, mdCursor);
    }

    // Selection highlight
    if (g_editor.hasSelection) {
        TextPos selStart, selEnd;
        getSelRange(selStart, selEnd);
        for (int i = 0; i < visibleVrows && (g_editor.scroll + i) < (int)vrows.size(); i++) {
            int vrIdx = g_editor.scroll + i;
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
            int mdCursor = (lineIdx == g_editor.cy) ? g_editor.cx : -1;
            int xOff = 4 + mdVrowX(g_editor.lines[lineIdx], mdi, hlStart, rowStart,
                                   vr.indentCells, mdCursor);
            int selEndX = 4 + mdVrowX(g_editor.lines[lineIdx], mdi, hlEnd, rowStart,
                                      vr.indentCells, mdCursor);
            int selW = selEndX - xOff;
            int ly = y + i * LINE_SPACING;
            u8g2_SetDrawColor(g_u8g2, 2);  // XOR mode
            u8g2_DrawBox(g_u8g2, xOff, ly - g_font.ascent(), selW, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 1);  // restore
        }
    }

    if (cursorVR >= 0 && cursorVR >= g_editor.scroll && cursorVR < g_editor.scroll + visibleVrows) {
        auto &vr = vrows[cursorVR];
        const std::string &line = g_editor.lines[vr.lineIdx];
        const MdLineInfo &mdi = mdInfo[vr.lineIdx];
        int cx = 4 + mdVrowX(line, mdi, g_editor.cx, vr.start, vr.indentCells, g_editor.cx);
        int cy_draw = y + (cursorVR - g_editor.scroll) * LINE_SPACING;
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
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawBox(g_u8g2, cx, cy_draw + 4, cw, 3);
        u8g2_SetDrawColor(g_u8g2, 1);
    }

    if (composing) {
        std::string code = g_ime.displayCode();
        int pageSize = g_ime.pageSize();
        int curPage = g_ime.currentPage();
        int totalPages = g_ime.totalPages();
        if (totalPages < 1) totalPages = 1;
        char pageInfo[32];
        snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
        int sepY = IME_CODE_Y - 4;
        int codeBaseline = sepY - 7;
        {
            int cw = g_font.textWidth(code.c_str()) + 8;
            u8g2_SetDrawColor(g_u8g2, 1);
            u8g2_DrawBox(g_u8g2, 4, codeBaseline - g_font.ascent(), cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, codeBaseline, code.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
        {
            int tw = g_font.textWidth(pageInfo);
            int pw = tw + 8;
            int px = SCREEN_W - pw - 4;
            u8g2_SetDrawColor(g_u8g2, 1);
            u8g2_DrawBox(g_u8g2, px, codeBaseline - g_font.ascent(), pw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(px + 4, codeBaseline, pageInfo, false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
        u8g2_SetDrawColor(g_u8g2, 0);
        u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);
        u8g2_SetDrawColor(g_u8g2, 1);

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
            u8g2_DrawBox(g_u8g2, 4, IME_CAND_Y - g_font.ascent(), cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, IME_CAND_Y, candLine.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
    }

    if (!s_skipStatusBarAndIme) {
        int wc = getWordCount();
        char left[48];
        if (inQuickFileSession()) {
            snprintf(left, sizeof(left), "[%d]", quickEditIndex());
        } else {
            const char *mode = g_editor.promptMode ? "提示写作" : "自由写作";
            snprintf(left, sizeof(left), "%s", mode);
        }
        std::string imeLabel;
        if (!g_editor.imeActive) imeLabel = "EN";
        else if (g_ime.english()) imeLabel = "[英]";
        else {
            imeLabel = "[中]";
            imeLabel += g_ime.fullwidth() ? "\xe2\x97\x8f" : "\xe2\x97\x90"; // ● or ◐
            imeLabel += g_ime.trad() ? "繁" : "简";
        }
        std::string right = std::to_string(wc) + "字 " + imeLabel;
        std::string bt = battery_icon_status_text();
        if (!bt.empty()) right += " " + bt;

        ui_draw_status(left, right.c_str());
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

// ── Screen entry points ──────────────────────────────────────────────────
void screen_editor_init(ScreenContext &ctx) {
    g_editor.lines.clear();
    g_editor.autoSaveTime = 0;
    g_editor.savedFilename = ctx.editFilename;

    // 快捷编辑主会话: 直接加载 /sdcard/{n}.txt; 有传入内容(灵感/日记编辑)时不走快捷文件
    bool quickFile = g_quickEdit && ctx.editContent.empty() && g_editor.savedFilename.empty();
    if (quickFile) {
        loadQuickEditFile();
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
    g_editor.imeActive = false;
    g_ime.setActive(false);
    g_ime.setFullwidth(false);
    g_ime.setEnglish(false);
    g_editor.confirmSave = false;
    g_editor.modifiedSinceSave = false;
    g_editor.drawnOnce = false;
    markDirty();
    g_editor.promptText = ctx.promptText;
    g_editor.promptMode = ctx.promptMode && !quickFile;
    ctx.editFilename.clear();

    // 重置查找/替换对话框(重开编辑器时关闭)
    g_editor.search.active = false;
    g_editor.search.imeActive = false;
    g_editor.search.matches.clear();
    g_editor.search.cur = -1;

    // 重置快捷键帮助对话框
    g_editor.helpActive = false;
    g_editor.helpScroll = 0;
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

    if (key == 0x1B) {
        if (inQuickFileSession()) {
            // 快捷编辑: 自动保存后跳到设置面板
            if (quickEditSave(quickEditIndex(), currentEditorText())) g_journal.clearRecoveryDraft();
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
        if (g_ime.handleKey(key, imeOut)) {
            editorInsertText(imeOut);
            ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
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
        if (!text.empty()) {
            if (saveCurrentContent()) ctx.statusMessage = "已保存";
            else ctx.statusMessage = "保存失败";
        }
        ui_clear(); drawEditor(); ui_commit();
        g_editor.autoSaveTime = 0;
        g_editor.modifiedSinceSave = false;
        return APP_EDITOR;
    }  // Ctrl+S
    if (key == 0x11) {
        if (inQuickFileSession()) {
            if (quickEditSave(quickEditIndex(), currentEditorText())) g_journal.clearRecoveryDraft();
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
            g_editor.lines[g_editor.cy].insert(g_editor.cx, g_clipboard);
            g_editor.cx += (int)g_clipboard.length();
            g_editor.targetCx = -1;
            markDirty();
            g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
            g_editor.modifiedSinceSave = true;
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
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
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
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
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
        } else if (g_editor.cy > 0) {
            recordUndoSnapshot(UndoGroup::Delete);
            g_editor.cx = (int)g_editor.lines[g_editor.cy-1].length();
            g_editor.lines[g_editor.cy-1] += g_editor.lines[g_editor.cy];
            g_editor.lines.erase(g_editor.lines.begin() + g_editor.cy);
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
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
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
            int visualCol = g_editor.targetCx % EDITOR_MAX_CELLS;
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
    (void)ctx;
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
    // Auto-save on idle ticks (快捷编辑始终自动保存)
    if (g_editor.autoSaveTime > 0 && esp_timer_get_time() > g_editor.autoSaveTime) {
        g_editor.autoSaveTime = 0;
        bool shouldCommit = inQuickFileSession() || g_settings.autoSave();
        if (shouldCommit) {
            if (saveCurrentContent(false)) {
                g_editor.modifiedSinceSave = false;
            } else if (g_editor.modifiedSinceSave && g_settings.recoveryDraft()) {
                saveRecoveryDraftIfChanged();
            }
        } else if (g_editor.modifiedSinceSave && g_settings.recoveryDraft()) {
            saveRecoveryDraftIfChanged();
        }
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

// ── App-level helpers ────────────────────────────────────────────────────
bool app_ime_active() {
    return g_editor.imeActive;
}

void app_toggle_ime() {
    g_editor.imeActive = !g_editor.imeActive;
    g_ime.setActive(g_editor.imeActive);
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

std::string app_get_editor_text() {
    return currentEditorText();
}

// Insert text at the cursor, shared by IME commit and voice dictation.
void editorInsertText(const std::string &text) {
    if (text.empty()) return;
    recordUndoSnapshot(UndoGroup::Typing);
    if (g_editor.hasSelection) deleteSelection();
    g_editor.lines[g_editor.cy].insert(g_editor.cx, text);
    g_editor.cx += (int)text.length();
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
        // Split on '\n' (drop a trailing empty segment, like loadLinesIntoEditor)
        // so a multi-line polish result splits cleanly across lines.
        std::vector<std::string> ins;
        size_t pos = 0;
        while (pos < text.length()) {
            size_t nl = text.find('\n', pos);
            ins.push_back(nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        if (ins.size() > 1 && ins.back().empty()) ins.pop_back();

        std::string tail = g_editor.lines[g_editor.cy].substr(g_editor.cx);
        g_editor.lines[g_editor.cy] = g_editor.lines[g_editor.cy].substr(0, g_editor.cx) + ins[0];
        if (ins.size() == 1) {
            g_editor.lines[g_editor.cy] += tail;
            g_editor.cx = (int)g_editor.lines[g_editor.cy].length() - (int)tail.length();
        } else {
            int insertAt = g_editor.cy + 1;
            for (size_t i = 1; i < ins.size() - 1; i++)
                g_editor.lines.insert(g_editor.lines.begin() + insertAt++, ins[i]);
            g_editor.lines.insert(g_editor.lines.begin() + insertAt, ins.back() + tail);
            g_editor.cy = insertAt;
            g_editor.cx = (int)ins.back().length();
        }
    }
    g_editor.targetCx = -1;
    markDirty();
    g_editor.autoSaveTime = esp_timer_get_time() + 3000000;
    g_editor.modifiedSinceSave = true;
}
