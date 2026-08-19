#include "screen_polish_prompt.h"
#include "font_renderer.h"
#include "settings_manager.h"
#include "ime/IME.h"
#include "ui_helpers.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
}

// 正文区域底部:给底部 IME 候选条(约 67px)留白,合成时不遮挡最后一行。
#define PP_BODY_BOTTOM (SCREEN_H - 67 - 10)

// ── State ─────────────────────────────────────────────────────────────────
static struct {
    std::string buf;
    int cur = 0;       // 字节偏移
    int scroll = 0;    // 首个可见 vrow 索引
    bool imeActive = false;
} g;

// 按 '\n' 切逻辑行,返回每行的起始字节偏移。
static void splitLines(const std::string &s, std::vector<std::string> &lines, std::vector<int> &starts) {
    lines.clear(); starts.clear();
    size_t pos = 0;
    while (pos <= s.length()) {
        size_t nl = s.find('\n', pos);
        lines.push_back((nl == std::string::npos) ? s.substr(pos) : s.substr(pos, nl - pos));
        starts.push_back((int)pos);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

// 定位字节偏移 cur 所在逻辑行,输出行号、行起始偏移、行内格列位置。
static void locate(const std::string &s, int cur, int &lineIdx, int &lineStart, int &xCells) {
    std::vector<std::string> lines; std::vector<int> starts;
    splitLines(s, lines, starts);
    for (size_t i = 0; i < lines.size(); i++) {
        int ls = starts[i];
        int le = ls + (int)lines[i].length();
        if (cur >= ls && cur <= le) {
            lineIdx = (int)i; lineStart = ls;
            xCells = byteToCells(lines[i], cur - ls);
            return;
        }
        if (cur < ls) break;
    }
    if (!lines.empty()) {
        int i = (int)lines.size() - 1;
        lineIdx = i; lineStart = starts[i];
        xCells = byteToCells(lines[i], cur - lineStart);
    }
}

static int prevChar(int cur, const std::string &s) {
    if (cur <= 0) return 0;
    int p = cur - 1;
    while (p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) p--;
    return p;
}

static int nextChar(int cur, const std::string &s) {
    if (cur >= (int)s.length()) return (int)s.length();
    int n = cur + 1;
    while (n < (int)s.length() && ((unsigned char)s[n] & 0xC0) == 0x80) n++;
    return n;
}

// ── Drawing ───────────────────────────────────────────────────────────────
static void drawPromptEditor() {
    ui_clear();
    ui_draw_text_centered(28, "润色提示词", false, true);
    u8g2_DrawHLine(g_u8g2, 0, 28 + g_font.descent() + 4, SCREEN_W);

    std::vector<std::string> lines; std::vector<int> starts;
    splitLines(g.buf, lines, starts);
    std::vector<VRow> vrows = buildVrows(lines);

    // 定位光标所在 vrow
    int lineIdx, lineStart, xCells;
    locate(g.buf, g.cur, lineIdx, lineStart, xCells);
    int curVR = -1;
    for (int i = 0; i < (int)vrows.size(); i++) {
        if (vrows[i].lineIdx == lineIdx && vrows[i].start <= (g.cur - lineStart) &&
            (g.cur - lineStart) <= vrows[i].end) {
            curVR = i; break;
        }
    }
    if (curVR < 0) curVR = 0;

    int top = 28 + g_font.descent() + 10;
    int vis = (PP_BODY_BOTTOM - top + LINE_SPACING - 1) / LINE_SPACING;
    if (vis < 1) vis = 1;
    if (g.scroll > curVR) g.scroll = curVR;
    if (curVR >= g.scroll + vis) g.scroll = curVR - vis + 1;
    if (g.scroll < 0) g.scroll = 0;

    for (int i = 0; i < vis && (g.scroll + i) < (int)vrows.size(); i++) {
        const VRow &vr = vrows[g.scroll + i];
        std::string row = lines[vr.lineIdx].substr(vr.start, vr.end - vr.start);
        if (row.empty()) row = " ";
        ui_draw_text(8, top + i * LINE_SPACING, row.c_str());
    }
    if (vrows.empty()) ui_draw_text(8, top, " ");

    int dispRow = curVR - g.scroll;
    int x = 8 + (byteToCells(lines[lineIdx], g.cur - lineStart) -
                 byteToCells(lines[lineIdx], vrows[curVR].start));
    std::string curCh = g.buf.substr(g.cur, 1);
    int cw = (curCh.empty() || curCh == "\n") ? 8 : g_font.textWidth(curCh.c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, x, top + dispRow * LINE_SPACING + 4, cw, 3);
    u8g2_SetDrawColor(g_u8g2, 1);

    if (g.imeActive && g_ime.composing()) {
        drawIMEUI(SCREEN_H - 67 - 4);
    } else {
        ui_draw_status("Ctrl+S保存 Esc取消", "");
    }
    ui_commit();
}

// ── Screen entry points ───────────────────────────────────────────────────
void screen_polish_prompt_init() {
    g.buf = g_settings.polishPrompt();
    g.cur = (int)g.buf.length();
    g.scroll = 0;
    g.imeActive = false;
    g_ime.setActive(false);
}

AppState screen_polish_prompt_handle(int key, ScreenContext &ctx) {
    if (g.imeActive && key != 0) {
        std::string imeOut;
        if (g_ime.handleKey(key, imeOut)) {
            if (!imeOut.empty()) {
                g.buf.insert(g.cur, imeOut);
                g.cur += (int)imeOut.length();
            }
            drawPromptEditor();
            return APP_POLISH_PROMPT;
        }
    }
    if (key == KEY_IME_TOGGLE) {
        g.imeActive = !g.imeActive;
        g_ime.setActive(g.imeActive);
        drawPromptEditor();
        return APP_POLISH_PROMPT;
    }
    if (key == KEY_FULLWIDTH_TOGGLE) {
        g_ime.toggleFullwidth();
        drawPromptEditor();
        return APP_POLISH_PROMPT;
    }
    if (key == 0x13) {  // Ctrl+S 保存
        g_settings.setPolishPrompt(g.buf);
        g_ime.setActive(false);
        ctx.statusMessage = "润色提示词已保存";
        ctx.statusDuration = 30;
        return APP_SETTINGS;
    }
    if (key == 0x1B) {  // Esc 取消
        g_ime.setActive(false);
        return APP_SETTINGS;
    }

    if (key == 0x0A || key == 0x0D) {  // Enter 换行
        g.buf.insert(g.cur, 1, '\n');
        g.cur++;
    } else if (key == 0x7F || key == 0x08) {  // Backspace
        if (g.cur > 0) {
            int p = prevChar(g.cur, g.buf);
            g.buf.erase(p, g.cur - p);
            g.cur = p;
        }
    } else if (key == KEY_LEFT) {
        g.cur = prevChar(g.cur, g.buf);
    } else if (key == KEY_RIGHT) {
        g.cur = nextChar(g.cur, g.buf);
    } else if (key == KEY_UP) {
        int lineIdx, lineStart, xCells;
        locate(g.buf, g.cur, lineIdx, lineStart, xCells);
        if (lineIdx > 0) {
            std::vector<std::string> lines; std::vector<int> starts;
            splitLines(g.buf, lines, starts);
            const std::string &target = lines[lineIdx - 1];
            g.cur = starts[lineIdx - 1] +
                cellsToByte(target, 0, (int)target.length(), xCells);
        }
    } else if (key == KEY_DOWN) {
        int lineIdx, lineStart, xCells;
        locate(g.buf, g.cur, lineIdx, lineStart, xCells);
        std::vector<std::string> lines; std::vector<int> starts;
        splitLines(g.buf, lines, starts);
        if (lineIdx < (int)lines.size() - 1) {
            const std::string &target = lines[lineIdx + 1];
            g.cur = starts[lineIdx + 1] +
                cellsToByte(target, 0, (int)target.length(), xCells);
        }
    } else if (key >= 0x20 && key <= 0x7E) {
        g.buf.insert(g.cur, 1, (char)key);
        g.cur++;
    }

    drawPromptEditor();
    return APP_POLISH_PROMPT;
}
