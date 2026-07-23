#pragma once

#include <cstdint>
#include <cstddef>

// Font renderer: reads from embedded terminus28.fnt / terminus22.fnt / terminus20.fnt blobs
class FontRenderer {
public:
    bool begin();

    // Switch between 24pt and 28pt font
    bool setSize(int fontSize);

    // Draw UTF-8 text at (x, y). y is baseline.
    // Returns width consumed.
    int drawText(int x, int y, const char *text, bool invert = false);

    // Get text width in pixels (UTF-8)
    int textWidth(const char *text);

    // Get glyph advance width
    int charWidth(uint32_t codepoint);

    // Font metrics
    int lineHeight() const { return line_height_; }
    int ascent() const { return ascent_; }
    int descent() const { return descent_; }
    int fontSize() const { return font_size_; }

    // Cell-based layout helpers (monospace assumption)
    int cjkAdvance() const { return line_height_; }     // fullwidth advance (pixels)
    int halfAdvance() const { return line_height_ / 2; } // halfwidth advance (pixels)

    // Check if font is loaded
    bool loaded() const { return loaded_; }

    // Decode a single UTF-8 character from *str, advance str pointer
    static uint32_t utf8Decode(const char *&str);

private:
    struct GlyphMeta {
        uint16_t width;
        uint16_t height;
        int8_t x_off;
        int8_t y_off;
        uint8_t advance;
        uint32_t bitmap_offset;
    };

    // Find glyph metadata for a codepoint
    const GlyphMeta *findGlyph(uint32_t cp);

    // Draw a single glyph at (x, y)
    void drawGlyph(int x, int y, const GlyphMeta *meta, bool invert);

    // Parse font header from a blob pointer
    bool parseBlob(const uint8_t *blob, size_t sz);

    const uint8_t *blob_ = nullptr;
    const uint8_t *blob_22_ = nullptr;
    const uint8_t *blob_20_ = nullptr;
    const uint8_t *blob_28_ = nullptr;
    bool loaded_ = false;
    int font_size_ = 28;
    int line_height_ = 28;
    int ascent_ = 22;
    int descent_ = 6;
    int glyph_count_ = 0;

    // Table pointers
    const uint32_t *ascii_table_ = nullptr;
    struct CjkBlock { uint32_t start_cp, end_cp, first_meta; };
    const CjkBlock *cjk_blocks_ = nullptr;
    int cjk_block_count_ = 0;
    const CjkBlock *other_blocks_ = nullptr;
    int other_block_count_ = 0;
    const uint8_t *meta_array_ = nullptr;
    const uint8_t *bitmap_data_ = nullptr;
};

extern FontRenderer g_font;
