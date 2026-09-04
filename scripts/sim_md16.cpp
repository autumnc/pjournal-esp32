// Host simulation of FontRenderer::drawSymbolGlyph + drawTextStyled for
// markdown segments at 16pt vs 22pt, using the real symbol_glyphs.h data.
#include "../main/symbol_glyphs.h"
#include <cstdio>
#include <cstring>
#include <string>

static const int W = 200, H = 24;
static uint8_t fb[H][W];  // 1 = ink

// u8g2_DrawBitmap equivalent: MSB-first, transparent mode (1-bits drawn)
static void drawBitmap(int x, int y, int cnt, int h, const uint8_t *bits) {
    for (int r = 0; r < h; r++)
        for (int b = 0; b < cnt; b++) {
            uint8_t v = bits[r * cnt + b];
            for (int bit = 0; bit < 8; bit++)
                if ((v >> (7 - bit)) & 1) {
                    int px = x + b * 8 + bit, py = y + r;
                    if (px >= 0 && px < W && py >= 0 && py < H) fb[py][px] = 1;
                }
        }
}

// FontRenderer::drawSymbolGlyph
static void drawSymbolGlyph(int x, int y, const SymbolGlyph *sym) {
    drawBitmap(x + sym->x_off, y - sym->y_off, (sym->width + 7) / 8, sym->height, sym->bitmap);
}

// drawTextStyled without invert/strike/emph; bold = double draw at +1px;
// underline = HLine at baseline+5 across text width.
static int drawTextStyled(int x, int y, const char *text, const SymbolGlyph *(*lookup)(uint32_t), int halfAdv, bool bold, bool underline) {
    int orig = x, w = 0;
    for (const char *p = text; *p;) {
        // utf8 decode
        uint32_t cp = (unsigned char)*p;
        int len = 1;
        if (cp >= 0xF0) { cp = ((cp & 7) << 18) | (((unsigned char)p[1] & 63) << 12) | (((unsigned char)p[2] & 63) << 6) | ((unsigned char)p[3] & 63); len = 4; }
        else if (cp >= 0xE0) { cp = ((cp & 15) << 12) | (((unsigned char)p[1] & 63) << 6) | ((unsigned char)p[2] & 63); len = 3; }
        else if (cp >= 0xC0) { cp = ((cp & 31) << 6) | ((unsigned char)p[1] & 63); len = 2; }
        p += len;
        const SymbolGlyph *sym = lookup(cp);
        int adv;
        if (sym) {
            if (bold) drawSymbolGlyph(x + 1, y, sym);
            drawSymbolGlyph(x, y, sym);
            adv = sym->advance;
        } else {
            adv = halfAdv;  // space / any non-symbol
        }
        w += adv;
        x += adv;
    }
    if (underline)
        for (int px = orig; px < orig + w; px++) fb[y + 5][px] = 1;
    return w;
}

static const SymbolGlyph *lk16(uint32_t cp) { return getSymbolGlyph(cp, 16); }
static const SymbolGlyph *lk22(uint32_t cp) { return getSymbolGlyph(cp, 22); }
static const SymbolGlyph *lk28(uint32_t cp) { return getSymbolGlyph(cp, 28); }

static void render(const char *title, const char *text, int size, const SymbolGlyph *(*lookup)(uint32_t), int halfAdv, bool bold, bool underline) {
    memset(fb, 0, sizeof(fb));
    drawTextStyled(2, 15, text, lookup, halfAdv, bold, underline);
    printf("%s (size=%d, text=\"%s\")\n", title, size, text);
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) putchar(fb[r][c] ? '#' : '.');
        putchar('\n');
    }
    printf("\n");
}

int main() {
    // U+1F786 空心圆, U+F0131 待办空框, U+F0132 完成实心带勾, U+2022 bullet(放大)
    render("16pt bullet", " \xE2\x80\xA2 ", 16, lk16, 8, false, false);
    render("16pt circle (secondary)", " \xF0\x9F\x9E\x86 ", 16, lk16, 8, false, false);
    render("16pt task unchecked F0131", " \xF3\xB0\x84\xB1 ", 16, lk16, 8, false, false);
    render("16pt task done F0132", " \xF3\xB0\x84\xB2 ", 16, lk16, 8, false, false);
    render("16pt heading HL1", "\xF3\xB0\x8E\xA4", 16, lk16, 8, true, true);
    render("22pt bullet", " \xE2\x80\xA2 ", 22, lk22, 11, false, false);
    render("22pt circle (secondary)", " \xF0\x9F\x9E\x86 ", 22, lk22, 11, false, false);
    render("22pt task unchecked F0131", " \xF3\xB0\x84\xB1 ", 22, lk22, 11, false, false);
    render("22pt task done F0132", " \xF3\xB0\x84\xB2 ", 22, lk22, 11, false, false);
    render("22pt heading HL1", "\xF3\xB0\x8E\xA4", 22, lk22, 11, true, true);
    render("28pt bullet", " \xE2\x80\xA2 ", 28, lk28, 14, false, false);
    render("28pt circle (secondary)", " \xF0\x9F\x9E\x86 ", 28, lk28, 14, false, false);
    render("28pt task unchecked F0131", " \xF3\xB0\x84\xB1 ", 28, lk28, 14, false, false);
    render("28pt task done F0132", " \xF3\xB0\x84\xB2 ", 28, lk28, 14, false, false);
    return 0;
}
