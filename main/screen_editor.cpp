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

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *u8g2, int x, int y, int w, int h);
}

#include "clipboard.h"

#define IME_CODE_Y (STATUS_Y - 2*FONT_H + g_font.ascent())
#define IME_CAND_Y (STATUS_Y - FONT_H + g_font.ascent() - 3)
#define EDITOR_MAX_CELLS (SCREEN_W / g_font.halfAdvance())

// ── Editor state ─────────────────────────────────────────────────────────

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
    int cachedWordCount = 0;
    bool wordCountDirty = true;
    bool mdInfoDirty = true;
    std::vector<MdLineInfo> cachedMdInfo;
    bool cachedMdOn = false;
    int64_t autoSaveTime = 0;
    bool modifiedSinceSave = false;
    std::string savedFilename;

    // Selection
    bool hasSelection = false;
    int selAnchorCy = 0, selAnchorCx = 0;
    // Whether the editor content is currently on screen. Idle ticks skip the
    // full redraw once it is; reset when another screen paints over it.
    bool drawnOnce = false;
} g_editor;

// Mark every line-derived cache (vrows, word count, markdown info) stale.
static void markDirty() {
    g_editor.vrowsDirty = true;
    g_editor.wordCountDirty = true;
    g_editor.mdInfoDirty = true;
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
    if (g_editor.vrowsDirty) {
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

static void loadQuickEditFile() {
    loadLinesIntoEditor(quickEditLoad(quickEditIndex()));
    g_editor.scroll = 0;
    g_editor.targetCx = -1;
    markDirty();
    g_editor.modifiedSinceSave = false;
    g_editor.autoSaveTime = 0;
}

// 是否为快捷编辑主会话(直接编辑 /sdcard/{n}.txt)。g_quickEdit 模式下若正
// 在编辑灵感/日记内容(savedFilename 非空),则不属于快捷文件会话。
static bool inQuickFileSession() {
    return g_quickEdit && g_editor.savedFilename.empty();
}

static void quickEditSwitchTo(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;
    if (idx == quickEditIndex()) return;
    quickEditSave(quickEditIndex(), currentEditorText());
    quickEditSetIndex(idx);
    loadQuickEditFile();
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
        mdDrawVrow(4, y + i * LINE_SPACING, g_editor.lines[vr.lineIdx], vr.start, vr.end, mdInfo[vr.lineIdx]);
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
            std::string sel = g_editor.lines[lineIdx].substr(hlStart, hlEnd - hlStart);
            int xOff = 4 + mdVrowX(g_editor.lines[lineIdx], mdi, hlStart, rowStart);
            int selW = g_font.textWidth(sel.c_str());
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
        int cx = 4 + mdVrowX(line, mdi, g_editor.cx, vr.start);
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

// ── Editor save helper ────────────────────────────────────────────────────
static bool saveCurrentContent() {
    std::string text = currentEditorText();
    if (inQuickFileSession()) {
        // 快捷编辑: 直接写回 /sdcard/{n}.txt, 允许保存空文件
        return quickEditSave(quickEditIndex(), text);
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
    return g_journal.saveEntryRaw(g_editor.savedFilename, fullText);
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
}

AppState screen_editor_handle(int key, ScreenContext &ctx) {
    const auto& vrows = getVrows();

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
            quickEditNext();
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
    }
    if (key == 0x10) { // Ctrl+P
        if (inQuickFileSession()) {  // 快捷编辑: 上一个文件
            quickEditPrev();
            ui_clear(); drawEditor(); ui_commit();
            return APP_EDITOR;
        }
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
        if (inQuickFileSession()) {
            // 快捷编辑: 自动保存后跳到设置面板
            quickEditSave(quickEditIndex(), currentEditorText());
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
            quickEditSave(quickEditIndex(), currentEditorText());
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
            g_clipboard = getSelectedText();
            deleteSelection();
        }
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x16) { // Ctrl+V — paste
        if (!g_clipboard.empty()) {
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
            deleteSelection();
        } else if (g_editor.cx > 0) {
            int prev = g_editor.cx - 1;
            while (prev > 0 && ((unsigned char)g_editor.lines[g_editor.cy][prev] & 0xC0) == 0x80) prev--;
            g_editor.lines[g_editor.cy].erase(prev, g_editor.cx - prev);
            g_editor.cx = prev;
        } else if (g_editor.cy > 0) {
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
    // Auto-save on idle ticks (快捷编辑始终自动保存)
    if (g_editor.autoSaveTime > 0 && esp_timer_get_time() > g_editor.autoSaveTime) {
        g_editor.autoSaveTime = 0;
        if (inQuickFileSession() || g_settings.autoSave()) {
            if (saveCurrentContent()) g_editor.modifiedSinceSave = false;
        }
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
