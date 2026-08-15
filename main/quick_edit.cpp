#include "quick_edit.h"
#include "settings_manager.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_log.h>

static const char *TAG = "QuickEdit";
static const char *QUICK_INDEX_KEY = "quick_index";

bool g_quickEdit = false;

bool quickEditInit() {
    for (int i = 0; i < 10; i++) {
        std::string path = quickEditFilePath(i);
        FILE *f = fopen(path.c_str(), "a");  // 不存在则创建空文件
        if (f) {
            fclose(f);
        } else {
            ESP_LOGE(TAG, "Cannot create %s", path.c_str());
        }
    }
    ESP_LOGI(TAG, "Quick edit files ready, current=%d", quickEditIndex());
    return true;
}

int quickEditIndex() {
    std::string v = g_settings.getString(QUICK_INDEX_KEY, "0");
    int idx = atoi(v.c_str());
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;
    return idx;
}

void quickEditSetIndex(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 9) idx = 9;
    g_settings.setString(QUICK_INDEX_KEY, std::to_string(idx));
}

void quickEditNext() { quickEditSetIndex((quickEditIndex() + 1) % 10); }
void quickEditPrev() { quickEditSetIndex((quickEditIndex() + 9) % 10); }

std::string quickEditFilePath(int idx) {
    char buf[16];
    snprintf(buf, sizeof(buf), "/sdcard/%d.txt", idx);
    return buf;
}

std::string quickEditLoad(int idx) {
    FILE *f = fopen(quickEditFilePath(idx).c_str(), "r");
    if (!f) return "";
    std::string result;
    char buf[256];
    int n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = 0;
        result += buf;
    }
    fclose(f);
    return result;
}

bool quickEditSave(int idx, const std::string &text) {
    FILE *f = fopen(quickEditFilePath(idx).c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s", quickEditFilePath(idx).c_str());
        return false;
    }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return true;
}
