#include "screen_editor.h"
#include "font_renderer.h"
#include "journal_storage.h"
#include "deepseek_client.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "ime/IME.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern void *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *u8g2, int x, int y, int w, int h);
}

#define IME_CODE_Y (STATUS_Y - 2*FONT_H + 22)
#define IME_CAND_Y (STATUS_Y - FONT_H + 19)
#define EDITOR_MAX_CELLS 28

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
} g_editor;

static const std::vector<VRow>& getVrows() {
    if (g_editor.vrowsDirty) {
        g_editor.cachedVrows = buildVrows(g_editor.lines);
        g_editor.vrowsDirty = false;
    }
    return g_editor.cachedVrows;
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

// ── Editor drawing ────────────────────────────────────────────────────────
static void drawEditor() {
    int y = 28;

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
                y += FONT_H;
            }
        }
        u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
        y += FONT_H;
    }

    const auto& vrows = getVrows();
    bool composing = g_ime.composing();
    int contentEndY = composing ? IME_CODE_Y : STATUS_Y;
    int visibleVrows = (contentEndY - y + FONT_H - 1) / FONT_H;
    if (visibleVrows < 1) visibleVrows = 1;

    int cursorVR = -1;
    for (int vi = 0; vi < (int)vrows.size(); vi++) {
        if (vrows[vi].lineIdx == g_editor.cy && vrows[vi].start <= g_editor.cx && g_editor.cx <= vrows[vi].end) {
            cursorVR = vi;
            break;
        }
    }

    int normalVisibleVrows = (STATUS_Y - y + FONT_H - 1) / FONT_H;
    int effectiveVisibleVrows = composing ? (normalVisibleVrows - 2) : normalVisibleVrows;
    if (effectiveVisibleVrows < 1) effectiveVisibleVrows = 1;

    if (cursorVR < g_editor.scroll) g_editor.scroll = cursorVR;
    if (cursorVR >= g_editor.scroll + effectiveVisibleVrows)
        g_editor.scroll = cursorVR - effectiveVisibleVrows + 1;
    if (g_editor.scroll < 0) g_editor.scroll = 0;

    for (int i = 0; i < visibleVrows && (g_editor.scroll + i) < (int)vrows.size(); i++) {
        auto &vr = vrows[g_editor.scroll + i];
        std::string text = g_editor.lines[vr.lineIdx].substr(vr.start, vr.end - vr.start);
        ui_draw_text(4, y + i * FONT_H, text.c_str());
    }

    if (cursorVR >= 0 && cursorVR >= g_editor.scroll && cursorVR < g_editor.scroll + visibleVrows) {
        auto &vr = vrows[cursorVR];
        const std::string &line = g_editor.lines[vr.lineIdx];
        std::string prefix = line.substr(vr.start, g_editor.cx - vr.start);
        int cx = 4 + g_font.textWidth(prefix.c_str());
        int cy_draw = y + (cursorVR - g_editor.scroll) * FONT_H;
        int cw = 14;
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
        int total = g_ime.totalCandidates();
        int pageSize = g_ime.pageSize();
        int curPage = g_ime.currentPage();
        int totalPages = (total + pageSize - 1) / pageSize;
        if (totalPages < 1) totalPages = 1;
        char pageInfo[32];
        snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);
        int sepY = IME_CODE_Y - 4;
        int codeBaseline = sepY - 7;
        {
            int cw = g_font.textWidth(code.c_str()) + 8;
            u8g2_SetDrawColor(g_u8g2, 1);
            u8g2_DrawBox(g_u8g2, 4, codeBaseline - 22, cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, codeBaseline, code.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
        {
            int tw = g_font.textWidth(pageInfo);
            int pw = tw + 8;
            int px = SCREEN_W - pw - 4;
            u8g2_SetDrawColor(g_u8g2, 1);
            u8g2_DrawBox(g_u8g2, px, codeBaseline - 22, pw, FONT_H);
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
            u8g2_DrawBox(g_u8g2, 4, IME_CAND_Y - 22, cw, FONT_H);
            u8g2_SetDrawColor(g_u8g2, 0);
            g_font.drawText(4, IME_CAND_Y, candLine.c_str(), false);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
    }

    const char *mode = g_editor.promptMode ? "提示写作" : "自由写作";
    int wc = getWordCount();
    char left[48];
    snprintf(left, sizeof(left), "%s", mode);
    int bpct = battery_pct();
    const char *imeLabel = g_editor.imeActive ? "[中]" : "EN";
    char right[64];
    if (bpct >= 0)
        snprintf(right, sizeof(right), "%d字 %d%% %s", wc, bpct, imeLabel);
    else
        snprintf(right, sizeof(right), "%d字 -- %s", wc, imeLabel);

    ui_draw_status(left, right);
}

static void drawConfirmDialog() {
    int bw = 280, bh = 120;
    int bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2 - 20;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, bw, bh);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, bw, bh);
    g_font.drawText(bx + 20, by + 28, "是否保存当前内容？");
    g_font.drawText(bx + 20, by + 58, "Enter=保存");
    g_font.drawText(bx + 20, by + 88, "ESC=放弃");
    u8g2_SetDrawColor(g_u8g2, 1);
}

static AppState finishEditor(ScreenContext &ctx) {
    std::string text;
    for (auto &l : g_editor.lines) { text += l; text += '\n'; }
    while (!text.empty() && text.back() == '\n') text.pop_back();
    if (text.empty()) return APP_MAIN;

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

    if (!g_journal.saveEntry(fullText)) {
        ctx.statusMessage = "保存失败，请检查SD卡";
    }
    ctx.nextState = APP_MAIN;
    return APP_MAIN;
}

// ── Screen entry points ──────────────────────────────────────────────────
void screen_editor_init(const ScreenContext &ctx) {
    g_editor.lines.clear(); g_editor.lines.push_back("");
    g_editor.cx = g_editor.cy = g_editor.scroll = 0;
    g_editor.targetCx = -1;
    g_editor.confirmSave = false;
    g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    g_editor.promptText = ctx.promptText;
    g_editor.promptMode = ctx.promptMode;
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
            ctx.nextState = APP_MAIN;
            return APP_MAIN;
        }
        ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
        return APP_EDITOR;
    }

    if (g_editor.imeActive && key != 0) {
        std::string imeOut;
        if (g_ime.handleKey(key, imeOut)) {
            if (!imeOut.empty()) {
                g_editor.lines[g_editor.cy].insert(g_editor.cx, imeOut);
                g_editor.cx += (int)imeOut.length();
                g_editor.targetCx = -1;
                g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
            }
            ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
        }
    }

    if (key == 9) {
        g_editor.lines[g_editor.cy].insert(g_editor.cx, 4, ' ');
        g_editor.cx += 4;
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
        ui_clear(); drawEditor(); ui_commit(); return APP_EDITOR;
    }
    if (key == 0x10) { // Ctrl+P
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
        bool hasContent = g_editor.lines.size() > 1 ||
            (g_editor.lines.size() == 1 && !g_editor.lines[0].empty());
        if (hasContent) {
            g_editor.confirmSave = true;
            ui_clear(); drawEditor(); drawConfirmDialog(); ui_commit();
            return APP_EDITOR;
        }
        ctx.nextState = APP_MAIN; return APP_MAIN;
    }
    if (key == 0x13) return finishEditor(ctx);  // Ctrl+S
    if (key == 0x11) { ctx.nextState = APP_MAIN; return APP_MAIN; }  // Ctrl+Q
    if (key == 0x06) {  // Ctrl+F
        ctx.nextState = APP_SYNC_SEND_FLOMO;
        return APP_SYNC_SEND_FLOMO;
    }

    // Navigation & editing
    if (key == 0x0A || key == 0x0D) { // Enter
        std::string rest = g_editor.lines[g_editor.cy].substr(g_editor.cx);
        g_editor.lines[g_editor.cy] = g_editor.lines[g_editor.cy].substr(0, g_editor.cx);
        g_editor.cx = 0; g_editor.cy++;
        g_editor.lines.insert(g_editor.lines.begin() + g_editor.cy, rest);
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    } else if (key == 0x7F || key == 0x08) { // Backspace
        if (g_editor.cx > 0) {
            int prev = g_editor.cx - 1;
            while (prev > 0 && ((unsigned char)g_editor.lines[g_editor.cy][prev] & 0xC0) == 0x80) prev--;
            g_editor.lines[g_editor.cy].erase(prev, g_editor.cx - prev);
            g_editor.cx = prev;
        }
        else if (g_editor.cy > 0) {
            g_editor.cx = (int)g_editor.lines[g_editor.cy-1].length();
            g_editor.lines[g_editor.cy-1] += g_editor.lines[g_editor.cy];
            g_editor.lines.erase(g_editor.lines.begin() + g_editor.cy);
            g_editor.cy--;
        }
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    } else if (key >= 0x20 && key <= 0x7E) { // ASCII printable
        g_editor.lines[g_editor.cy].insert(g_editor.cx, 1, (char)key);
        g_editor.cx++;
        g_editor.targetCx = -1;
        g_editor.vrowsDirty = true; g_editor.wordCountDirty = true;
    } else if (key == KEY_LEFT) {
        if (g_editor.cx > 0) {
            g_editor.cx--;
            while (g_editor.cx > 0 && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx--;
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_RIGHT) {
        if (g_editor.cx < (int)g_editor.lines[g_editor.cy].length()) {
            g_editor.cx++;
            while (g_editor.cx < (int)g_editor.lines[g_editor.cy].length() && ((unsigned char)g_editor.lines[g_editor.cy][g_editor.cx] & 0xC0) == 0x80) g_editor.cx++;
        }
        g_editor.targetCx = -1;
    } else if (key == KEY_UP) {
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
            g_editor.cy = prev.lineIdx;
            g_editor.cx = cellsToByte(g_editor.lines[prev.lineIdx], prev.start, prev.end, g_editor.targetCx);
        }
    } else if (key == KEY_DOWN) {
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
            g_editor.cy = next.lineIdx;
            g_editor.cx = std::min(cellsToByte(g_editor.lines[next.lineIdx], next.start, next.end, g_editor.targetCx),
                                   (int)g_editor.lines[g_editor.cy].length());
        }
    }

    ui_clear(); drawEditor(); ui_commit();
    return APP_EDITOR;
}

// ── App-level helpers ────────────────────────────────────────────────────
bool app_ime_active() {
    return g_editor.imeActive;
}

void app_toggle_ime() {
    g_editor.imeActive = !g_editor.imeActive;
    g_ime.setActive(g_editor.imeActive);
}

std::string app_get_editor_text() {
    std::string text;
    for (auto &l : g_editor.lines) { text += l; text += '\n'; }
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}
