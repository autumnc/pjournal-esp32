#include "font_renderer.h"
#include <cstring>
#include <esp_log.h>

static const char *TAG = "Font";
FontRenderer g_font;

// Embedded font data from CMakeLists EMBED_FILES
extern const uint8_t terminus28_fnt_start[] asm("_binary_terminus28_fnt_start");
extern const uint8_t terminus28_fnt_end[]   asm("_binary_terminus28_fnt_end");
extern const uint8_t terminus22_fnt_start[] asm("_binary_terminus22_fnt_start");
extern const uint8_t terminus22_fnt_end[]   asm("_binary_terminus22_fnt_end");
extern const uint8_t terminus16_fnt_start[] asm("_binary_terminus16_fnt_start");
extern const uint8_t terminus16_fnt_end[]   asm("_binary_terminus16_fnt_end");

bool FontRenderer::begin() {
    blob_28_ = terminus28_fnt_start;
    blob_22_ = terminus22_fnt_start;
    blob_16_ = terminus16_fnt_start;
    // Default to 28pt
    return setSize(28);
}

bool FontRenderer::setSize(int fontSize) {
    if (fontSize == font_size_) return true;

    const uint8_t *blob = nullptr;
    size_t sz = 0;

    if (fontSize == 28) {
        blob = blob_28_;
        sz = terminus28_fnt_end - terminus28_fnt_start;
    } else if (fontSize == 22) {
        blob = blob_22_;
        sz = terminus22_fnt_end - terminus22_fnt_start;
    } else if (fontSize == 16) {
        blob = blob_16_;
        sz = terminus16_fnt_end - terminus16_fnt_start;
    } else {
        return false;
    }
    if (!blob) return false;

    return parseBlob(blob, sz);
}

bool FontRenderer::parseBlob(const uint8_t *blob, size_t sz) {
    blob_ = blob;

    if (sz < 34 || memcmp(blob_, "PJFN", 4) != 0) {
        ESP_LOGE(TAG, "Bad font magic");
        return false;
    }

    // Read header
    line_height_ = blob_[6] | (blob_[7] << 8);
    ascent_      = blob_[8] | (blob_[9] << 8);
    descent_     = blob_[10] | (blob_[11] << 8);
    glyph_count_ = blob_[12] | (blob_[13] << 8);
    font_size_   = line_height_;

    uint32_t ascii_off  = *(const uint32_t *)(blob_ + 14);
    uint32_t cjk_off    = *(const uint32_t *)(blob_ + 18);
    uint32_t meta_off   = *(const uint32_t *)(blob_ + 22);
    uint32_t data_off   = *(const uint32_t *)(blob_ + 26);
    uint32_t other_off  = *(const uint32_t *)(blob_ + 30);

    uint32_t hdr_adj = 10;
    ascii_table_ = (const uint32_t *)(blob_ + ascii_off + hdr_adj);
    cjk_block_count_ = *(const uint16_t *)(blob_ + cjk_off + hdr_adj);
    cjk_blocks_ = (const CjkBlock *)(blob_ + cjk_off + hdr_adj + 2);
    meta_array_ = blob_ + meta_off + hdr_adj;
    bitmap_data_ = blob_ + data_off + hdr_adj;

    const uint8_t *other_ptr = blob_ + other_off + hdr_adj;
    other_block_count_ = *(const uint16_t *)other_ptr;
    other_blocks_ = (const CjkBlock *)(other_ptr + 2);

    loaded_ = true;
    ESP_LOGI(TAG, "Font %dpt loaded: %d glyphs, line=%d asc=%d desc=%d",
             font_size_, glyph_count_, line_height_, ascent_, descent_);
    return true;
}

uint32_t FontRenderer::utf8Decode(const char *&str) {
    if (!str || !*str) return 0;
    uint8_t c = (uint8_t)*str;
    if (c < 0x80) {
        str++;
        return c;
    }
    if ((c & 0xE0) == 0xC0) {
        if ((str[1] & 0xC0) != 0x80) { str++; return 0; }
        uint32_t cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(str[1] & 0x3F);
        str += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0) {
        if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) { str++; return 0; }
        uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(str[1] & 0x3F) << 6) | (uint32_t)(str[2] & 0x3F);
        str += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0) {
        if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || (str[3] & 0xC0) != 0x80) { str++; return 0; }
        str += 2;
        return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(str[-1] & 0x3F) << 12) | ((uint32_t)(str[0] & 0x3F) << 6) | (uint32_t)(str[1] & 0x3F);
    }
    str++;
    return 0;
}

const FontRenderer::GlyphMeta *FontRenderer::findGlyph(uint32_t cp) {
    if (!loaded_) return nullptr;

    // ASCII direct table
    if (cp >= 0x20 && cp <= 0x7E) {
        uint32_t idx = ascii_table_[cp - 0x20];
        if (idx == 0xFFFFFFFF) return nullptr;
        return (const GlyphMeta *)(meta_array_ + idx * 12);
    }

    // CJK table
    if (cp >= 0x3400 && cp <= 0x9FFF) {
        int lo = 0, hi = cjk_block_count_;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (cp > cjk_blocks_[mid].end_cp) lo = mid + 1;
            else hi = mid;
        }
        if (lo < cjk_block_count_ && cp >= cjk_blocks_[lo].start_cp && cp <= cjk_blocks_[lo].end_cp) {
            uint32_t meta_idx = cjk_blocks_[lo].first_meta + (cp - cjk_blocks_[lo].start_cp);
            if (meta_idx < (uint32_t)glyph_count_) {
                return (const GlyphMeta *)(meta_array_ + meta_idx * 12);
            }
        }
    }

    // Other blocks (fullwidth, CJK punctuation, etc.)
    if (other_block_count_ > 0) {
        int lo = 0, hi = other_block_count_;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (cp > other_blocks_[mid].end_cp) lo = mid + 1;
            else hi = mid;
        }
        if (lo < other_block_count_ && cp >= other_blocks_[lo].start_cp && cp <= other_blocks_[lo].end_cp) {
            uint32_t meta_idx = other_blocks_[lo].first_meta + (cp - other_blocks_[lo].start_cp);
            if (meta_idx < (uint32_t)glyph_count_) {
                return (const GlyphMeta *)(meta_array_ + meta_idx * 12);
            }
        }
    }

    return nullptr;
}

int FontRenderer::charWidth(uint32_t cp) {
    auto *m = findGlyph(cp);
    return m ? m->advance : line_height_;
}

int FontRenderer::textWidth(const char *text) {
    int w = 0;
    while (*text) {
        uint32_t cp = utf8Decode(text);
        if (cp == 0) continue;
        auto *sym = getSymbolGlyph(cp, font_size_);
        if (sym) {
            w += sym->advance;
        } else {
            auto *m = findGlyph(cp);
            if (m) {
                w += m->advance;
            } else {
                w += line_height_ / 2;
            }
        }
    }
    return w;
}

// External reference to U8G2 for drawing
extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawPixel(void *u8g2, int x, int y);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
    // DrawBitmap is MSB-first (bit 7 = leftmost column), matching our glyph
    // data. u8g2_DrawXBM is LSB-first and would mirror every 8px block.
    extern void u8g2_SetBitmapMode(void *u8g2, uint8_t is_transparent);
    extern void u8g2_DrawBitmap(void *u8g2, int x, int y, int cnt, int h, const uint8_t *bitmap);
    extern void *u8g2_st7305_get_u8g2(void *dev);
}
struct u8g2_struct;
typedef struct u8g2_struct u8g2_t;
extern u8g2_t *g_u8g2;

// True if the glyph's ink is made only of long horizontal bars (e.g. 一、二、三).
// Such glyphs need an extra +1px vertical stroke in bold: the horizontal shift alone
// only widens them, so the weight is invisible. Glyphs with vertical strokes or
// diagonals (四、十、八、大…) already look bold from the horizontal shift and would
// get too thick if we added a vertical stroke too.
// A row is "bar-like" if its longest horizontal run spans >= half the glyph width.
// The glyph qualifies if no column has a vertical run > 3 (no vertical stroke), and
// every ink row is either bar-like or a tiny (<=4px) tick adjacent to a bar (the font
// draws small end-ticks above CJK bars, e.g. row 0 of 一).
static bool bitmapIsHorizontalOnly(const uint8_t *bits, int bw, int bh, int row_bytes) {
    bool isBar[32] = {false};
    int maxVRun = 0;

    for (int row = 0; row < bh; row++) {
        int best = 0, run = 0;
        for (int col = 0; col < bw; col++) {
            bool on = (bits[row * row_bytes + col / 8] >> (7 - (col % 8))) & 1;
            if (on) { run++; if (run > best) best = run; }
            else run = 0;
        }
        if (best * 2 >= bw) isBar[row] = true;
    }

    for (int col = 0; col < bw; col++) {
        int run = 0;
        for (int row = 0; row < bh; row++) {
            bool on = (bits[row * row_bytes + col / 8] >> (7 - (col % 8))) & 1;
            run = on ? run + 1 : 0;
            if (run > maxVRun) maxVRun = run;
        }
    }
    if (maxVRun > 3) return false;

    for (int row = 0; row < bh; row++) {
        int ink = 0;
        for (int col = 0; col < bw; col++)
            ink += (bits[row * row_bytes + col / 8] >> (7 - (col % 8))) & 1;
        if (ink == 0 || isBar[row]) continue;
        bool adj = (row > 0 && isBar[row - 1]) || (row + 1 < bh && isBar[row + 1]);
        if (!adj || ink > 4) return false;
    }
    return true;
}

void FontRenderer::drawGlyph(int x, int y, const GlyphMeta *meta, bool invert) {
    if (!g_u8g2 || !meta) return;

    int bw = meta->width;
    int bh = meta->height;
    int row_bytes = (bw + 7) / 8;
    const uint8_t *bits = bitmap_data_ + meta->bitmap_offset;

    int draw_x = x + meta->x_off;
    int draw_y = y - meta->y_off - bh;

    // 常规绘制:整块位图写入,替代逐像素 DrawPixel(结果逐位一致)。
    // DrawBitmap 按 MSB 优先读位,与字形数据一致;必须先开透明模式,
    // 否则 0 位会被反色填充。开启透明对全工程安全(仅此处用到位图绘制)。
    if (!invert) {
        u8g2_SetBitmapMode(g_u8g2, 1);
        u8g2_DrawBitmap(g_u8g2, draw_x, draw_y, row_bytes, bh, bits);
        return;
    }

    // 反色绘制仅 GTD 优先级徽标等少量场合使用,保持原逐像素语义。
    // 当前位图绘制只描 ink 位,无法表达"描反色"语义,故此处不变。
    for (int row = 0; row < bh; row++) {
        for (int col = 0; col < bw; col++) {
            int byte_idx = row * row_bytes + col / 8;
            int bit = 7 - (col % 8);
            bool on = (bits[byte_idx] >> bit) & 1;
            if (!on) {
                u8g2_DrawPixel(g_u8g2, draw_x + col, draw_y + row);
            }
        }
    }
}

void FontRenderer::drawSymbolGlyph(int x, int y, const SymbolGlyph *sym, bool invert) {
    if (!g_u8g2 || !sym) return;
    int bw = sym->width;
    int bh = sym->height;
    int row_bytes = (bw + 7) / 8;
    const uint8_t *bits = sym->bitmap;

    int draw_x = x + sym->x_off;
    int draw_y = y - sym->y_off;

    if (!invert) {
        u8g2_SetBitmapMode(g_u8g2, 1);
        u8g2_DrawBitmap(g_u8g2, draw_x, draw_y, row_bytes, bh, bits);
        return;
    }

    for (int row = 0; row < bh; row++) {
        for (int col = 0; col < bw; col++) {
            int byte_idx = row * row_bytes + col / 8;
            int bit = 7 - (col % 8);
            bool on = (bits[byte_idx] >> bit) & 1;
            if (!on) {
                u8g2_DrawPixel(g_u8g2, draw_x + col, draw_y + row);
            }
        }
    }
}

int FontRenderer::drawText(int x, int y, const char *text, bool invert) {
    int orig_x = x;
    while (*text) {
        uint32_t cp = utf8Decode(text);
        if (cp == 0) continue;
        auto *sym = getSymbolGlyph(cp, font_size_);
        if (sym) {
            drawSymbolGlyph(x, y, sym, invert);
            x += sym->advance;
        } else {
            auto *meta = findGlyph(cp);
            if (meta) {
                drawGlyph(x, y, meta, invert);
                x += meta->advance;
            } else {
                x += line_height_ / 2;
            }
        }
    }
    return x - orig_x;
}

int FontRenderer::drawTextStyled(int x, int y, const char *text, const TextStyle &ts) {
    int orig_x = x;
    int w = textWidth(text);
    if (ts.invert) {
        int asc = ascent_;
        u8g2_SetDrawColor(g_u8g2, 0);  // dark box
        u8g2_DrawBox(g_u8g2, x, y - asc, w, line_height_);
        u8g2_SetDrawColor(g_u8g2, 1);  // light glyphs
    }
    while (*text) {
        uint32_t cp = utf8Decode(text);
        if (cp == 0) continue;
        int adv;
        auto *sym = getSymbolGlyph(cp, font_size_);
        if (sym) {
            if (ts.bold) drawSymbolGlyph(x + 1, y, sym, false);
            drawSymbolGlyph(x, y, sym, false);
            adv = sym->advance;
        } else {
            auto *meta = findGlyph(cp);
            if (meta) {
                if (ts.bold) drawGlyph(x + 1, y, meta, false);
                // 仅对纯横画字(一、二、三)额外向下描1px:水平位移只让它们变宽不变厚,
                // 看不出加粗;含竖/斜笔画的字(四、十、八…)水平位移已足够,再加就过粗。
                if (ts.bold &&
                    bitmapIsHorizontalOnly(bitmap_data_ + meta->bitmap_offset,
                                           meta->width, meta->height,
                                           (meta->width + 7) / 8))
                    drawGlyph(x, y + 1, meta, false);
                drawGlyph(x, y, meta, false);
                adv = meta->advance;
            } else {
                adv = line_height_ / 2;
            }
        }
        if (ts.emph && cp != 0x20) {
            int cx = x + adv / 2;
            int cy = y + descent_ + 1;  // in the 4px gap below the em box
            u8g2_DrawBox(g_u8g2, cx - 1, cy, 3, 2);
        }
        x += adv;
    }
    if (ts.underline) u8g2_DrawHLine(g_u8g2, orig_x, y + 5, w);
    if (ts.strike) {
        int sy = y - ascent_ + line_height_ / 2;
        u8g2_DrawHLine(g_u8g2, orig_x, sy, w);
        u8g2_DrawHLine(g_u8g2, orig_x, sy + 1, w);
    }
    if (ts.invert) u8g2_SetDrawColor(g_u8g2, 0);  // restore ink color
    return w;
}
