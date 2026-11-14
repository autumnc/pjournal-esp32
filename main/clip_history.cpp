#include "clip_history.h"

#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>

static const char *TAG = "ClipHist";

struct ClipEntry {
    char *data;
    uint32_t len;
    bool linewise;
};

static ClipEntry *s_entries = nullptr;
static int s_count = 0;

static bool ensureStorage() {
    if (s_entries) return true;
    const size_t bytes = sizeof(ClipEntry) * CLIPHIST_MAX_ENTRIES;
    s_entries = (ClipEntry *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_entries) {
        ESP_LOGW(TAG, "PSRAM alloc failed, falling back to internal RAM");
        s_entries = (ClipEntry *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (!s_entries) {
        ESP_LOGE(TAG, "history storage alloc failed");
        return false;
    }
    memset(s_entries, 0, bytes);
    ESP_LOGI(TAG, "history storage ready, psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

// 截到 32 KB，并保证不把多字节 UTF-8 字符劈开。
static size_t cappedLen(const char *data, size_t size) {
    if (size <= CLIPHIST_MAX_BYTES) return size;
    size_t n = CLIPHIST_MAX_BYTES;
    // 先退掉末尾的续字节；此时 n-1 是某个字符的首字节（或 ASCII）。
    while (n > 0 && ((unsigned char)data[n - 1] & 0xC0) == 0x80) --n;
    if (n == 0) return 0;
    unsigned char lead = (unsigned char)data[n - 1];
    size_t need = 1;
    if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    // 该字符在截断点内没有放全，连首字节一起丢掉。
    if (need > CLIPHIST_MAX_BYTES - (n - 1)) --n;
    return n;
}

static void evictOldest() {
    if (s_count <= 0) return;
    int last = s_count - 1;
    heap_caps_free(s_entries[last].data);
    s_entries[last] = {nullptr, 0, false};
    --s_count;
}

void cliphist_record(const std::string &text, bool linewise) {
    if (text.empty()) return;
    if (!ensureStorage()) return;

    const size_t n = cappedLen(text.data(), text.size());
    if (n == 0) return;

    // 去重：内容相同就把旧描述符移到最前，不重新分配。
    for (int i = 0; i < s_count; ++i) {
        if (s_entries[i].len == n && memcmp(s_entries[i].data, text.data(), n) == 0) {
            ClipEntry hit = s_entries[i];
            hit.linewise = linewise;
            memmove(&s_entries[1], &s_entries[0], sizeof(ClipEntry) * i);
            s_entries[0] = hit;
            return;
        }
    }

    char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        // PSRAM 紧张时先腾掉最旧一条再退回内部 RAM 试一次。
        if (s_count == CLIPHIST_MAX_ENTRIES) evictOldest();
        buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGW(TAG, "drop clipboard entry (%u bytes): alloc failed", (unsigned)n);
            return;
        }
    }

    if (s_count == CLIPHIST_MAX_ENTRIES) evictOldest();
    memmove(&s_entries[1], &s_entries[0], sizeof(ClipEntry) * s_count);
    memcpy(buf, text.data(), n);
    buf[n] = '\0';
    s_entries[0] = {buf, (uint32_t)n, linewise};
    ++s_count;
}

int cliphist_count() { return s_entries ? s_count : 0; }

const char *cliphist_text(int i) {
    if (!s_entries || i < 0 || i >= s_count) return nullptr;
    return s_entries[i].data;
}

uint32_t cliphist_len(int i) {
    if (!s_entries || i < 0 || i >= s_count) return 0;
    return s_entries[i].len;
}

bool cliphist_linewise(int i) {
    if (!s_entries || i < 0 || i >= s_count) return false;
    return s_entries[i].linewise;
}

bool cliphist_add_raw(const char *data, size_t len, bool linewise) {
    if (!data || len == 0) return false;
    if (!ensureStorage()) return false;

    const size_t n = cappedLen(data, len);
    if (n == 0) return false;

    // 与 cliphist_record 相同的去重语义：命中则移到最前，不重新分配。
    for (int i = 0; i < s_count; ++i) {
        if (s_entries[i].len == n && memcmp(s_entries[i].data, data, n) == 0) {
            ClipEntry hit = s_entries[i];
            hit.linewise = linewise;
            memmove(&s_entries[1], &s_entries[0], sizeof(ClipEntry) * i);
            s_entries[0] = hit;
            return true;
        }
    }

    char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        if (s_count == CLIPHIST_MAX_ENTRIES) evictOldest();
        buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGW(TAG, "drop web entry (%u bytes): alloc failed", (unsigned)n);
            return false;
        }
    }

    if (s_count == CLIPHIST_MAX_ENTRIES) evictOldest();
    memmove(&s_entries[1], &s_entries[0], sizeof(ClipEntry) * s_count);
    memcpy(buf, data, n);
    buf[n] = '\0';
    s_entries[0] = {buf, (uint32_t)n, linewise};
    ++s_count;
    return true;
}

bool cliphist_replace_raw(int i, const char *data, size_t len) {
    if (!s_entries || !data || i < 0 || i >= s_count) return false;
    const size_t n = cappedLen(data, len);
    if (n == 0) return false;

    // 先分配新内容再释放旧内容：分配失败时原条目保持不变。
    char *buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)heap_caps_malloc(n + 1, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "replace entry %d (%u bytes): alloc failed", i, (unsigned)n);
        return false;
    }
    memcpy(buf, data, n);
    buf[n] = '\0';
    heap_caps_free(s_entries[i].data);
    s_entries[i].data = buf;
    s_entries[i].len = (uint32_t)n;
    return true;
}

void cliphist_remove(int i) {
    if (!s_entries || i < 0 || i >= s_count) return;
    heap_caps_free(s_entries[i].data);
    if (i + 1 < s_count) {
        memmove(&s_entries[i], &s_entries[i + 1], sizeof(ClipEntry) * (s_count - i - 1));
    }
    --s_count;
    s_entries[s_count] = {nullptr, 0, false};
}
