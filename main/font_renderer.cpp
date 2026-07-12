#include "font_renderer.h"
#include <cstring>
#include <esp_log.h>

static const char *TAG = "Font";
FontRenderer g_font;

// Embedded font data from CMakeLists EMBED_FILES
extern const uint8_t terminus28_fnt_start[] asm("_binary_terminus28_fnt_start");
extern const uint8_t terminus28_fnt_end[]   asm("_binary_terminus28_fnt_end");

bool FontRenderer::begin() {
    blob_ = terminus28_fnt_start;
    size_t sz = terminus28_fnt_end - terminus28_fnt_start;

    if (sz < 24 || memcmp(blob_, "PJFN", 4) != 0) {
        ESP_LOGE(TAG, "Bad font magic");
        return false;
    }

    // Read header (format: magic[4], header_size[2], line_height[2], ascent[2],
    //                    descent[2], glyph_count[2], 4×offset[4])
    line_height_ = blob_[6] | (blob_[7] << 8);
    ascent_      = blob_[8] | (blob_[9] << 8);
    descent_     = blob_[10] | (blob_[11] << 8);
    glyph_count_ = blob_[12] | (blob_[13] << 8);

    uint32_t ascii_off  = *(const uint32_t *)(blob_ + 14);
    uint32_t cjk_off    = *(const uint32_t *)(blob_ + 18);
    uint32_t meta_off   = *(const uint32_t *)(blob_ + 22);
    uint32_t data_off   = *(const uint32_t *)(blob_ + 26);
    uint32_t other_off  = *(const uint32_t *)(blob_ + 30);

    // bdf_to_fnt.py stores all offsets relative to base=24, actual
    // header is 34 bytes (added other_offset). Correct by adding the difference.
    uint32_t hdr_adj = 10; // actual header (34) - stored base (24)
    ascii_table_ = (const uint32_t *)(blob_ + ascii_off + hdr_adj);
    cjk_block_count_ = *(const uint16_t *)(blob_ + cjk_off + hdr_adj);
    cjk_blocks_ = (const CjkBlock *)(blob_ + cjk_off + hdr_adj + 2);
    meta_array_ = blob_ + meta_off + hdr_adj;
    bitmap_data_ = blob_ + data_off + hdr_adj;

    // Other block table (non-ASCII, non-CJK glyphs)
    const uint8_t *other_ptr = blob_ + other_off + hdr_adj;
    other_block_count_ = *(const uint16_t *)other_ptr;
    other_blocks_ = (const CjkBlock *)(other_ptr + 2);

    loaded_ = true;
    ESP_LOGI(TAG, "Font loaded: %d glyphs, line=%d asc=%d desc=%d",
             glyph_count_, line_height_, ascent_, descent_);
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
        str += 4;
        return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(str[-3] & 0x3F) << 12) | ((uint32_t)(str[-2] & 0x3F) << 6) | (uint32_t)(str[-1] & 0x3F);
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
        // Binary search blocks
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
        w += charWidth(cp);
    }
    return w;
}

// External reference to U8G2 for drawing
extern "C" {
    extern void u8g2_SetDrawColor(void *u8g2, int color);
    extern void u8g2_DrawPixel(void *u8g2, int x, int y);
    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);
    extern void *u8g2_st7305_get_u8g2(void *dev);
}
// We need a global u8g2 pointer. Set by main.
extern void *g_u8g2;

void FontRenderer::drawGlyph(int x, int y, const GlyphMeta *meta, bool invert) {
    if (!g_u8g2 || !meta) return;

    int bw = meta->width;
    int bh = meta->height;
    int xo = meta->x_off;
    int yo = meta->y_off;
    int row_bytes = (bw + 7) / 8;
    const uint8_t *bits = bitmap_data_ + meta->bitmap_offset;

    int draw_x = x + xo;
    // y_off = pixels from baseline to bottom of bitmap (positive = above baseline)
    int draw_y = y - yo - bh;

    for (int row = 0; row < bh; row++) {
        for (int col = 0; col < bw; col++) {
            int byte_idx = row * row_bytes + col / 8;
            int bit = 7 - (col % 8);
            bool on = (bits[byte_idx] >> bit) & 1;
            if (invert) on = !on;
            if (on) {
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
        auto *meta = findGlyph(cp);
        if (meta) {
            drawGlyph(x, y, meta, invert);
            x += meta->advance;
        } else {
            x += line_height_ / 2; // fallback space for missing glyph
        }
    }
    return x - orig_x;
}
