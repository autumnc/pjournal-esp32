#include "journal_template.h"

#include "safe_file.h"
#include "settings_manager.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_log.h>

static const char *TAG = "JournalTemplate";

// 由 main/CMakeLists.txt 的 target_add_binary_data 生成（Templates/*.md）。
extern "C" {
extern const uint8_t pj_tpl0_start[] asm("_binary_pj_default_template_0_start");
extern const uint8_t pj_tpl0_end[]   asm("_binary_pj_default_template_0_end");
extern const uint8_t pj_tpl1_start[] asm("_binary_pj_default_template_1_start");
extern const uint8_t pj_tpl1_end[]   asm("_binary_pj_default_template_1_end");
extern const uint8_t pj_tpl2_start[] asm("_binary_pj_default_template_2_start");
extern const uint8_t pj_tpl2_end[]   asm("_binary_pj_default_template_2_end");
extern const uint8_t pj_tpl3_start[] asm("_binary_pj_default_template_3_start");
extern const uint8_t pj_tpl3_end[]   asm("_binary_pj_default_template_3_end");
extern const uint8_t pj_tpl4_start[] asm("_binary_pj_default_template_4_start");
extern const uint8_t pj_tpl4_end[]   asm("_binary_pj_default_template_4_end");
extern const uint8_t pj_tpl5_start[] asm("_binary_pj_default_template_5_start");
extern const uint8_t pj_tpl5_end[]   asm("_binary_pj_default_template_5_end");
}

struct BuiltinTemplate {
    const char *name;
    const uint8_t *data;
    size_t size;
};

// 顺序必须与 main/CMakeLists.txt 的 PJOURNAL_DEFAULT_TEMPLATE_NAMES 一致。
static const BuiltinTemplate kBuiltinTemplates[] = {
    {"子弹笔记日记.md", pj_tpl0_start, (size_t)(pj_tpl0_end - pj_tpl0_start)},
    {"晨间日记.md", pj_tpl1_start, (size_t)(pj_tpl1_end - pj_tpl1_start)},
    {"极简日记模板.md", pj_tpl2_start, (size_t)(pj_tpl2_end - pj_tpl2_start)},
    {"项目学习日记.md", pj_tpl3_start, (size_t)(pj_tpl3_end - pj_tpl3_start)},
    {"马伯庸日记模板.md", pj_tpl4_start, (size_t)(pj_tpl4_end - pj_tpl4_start)},
    {"马伯庸日记模板-紧凑.md", pj_tpl5_start, (size_t)(pj_tpl5_end - pj_tpl5_start)},
};

const char *kJOURNAL_TEMPLATE_DIR = "/sdcard/Templates";

static const char *TEMPLATE_SETTING_KEY = "journal_template";
// 「无模板」的哨兵值。用不可能是合法文件名的写法，和"从未选择"（空值，
// 默认取列表第一个）区分开，老设备升级后模板日记仍然默认开启。
static const char *TEMPLATE_NONE_VALUE = "__none__";

// 模板会被整份读进内存，限制大小避免 SD 卡上的异常文件拖垮内部 RAM。
static const off_t kMaxTemplateBytes = 64 * 1024;

static bool hasMarkdownExt(const std::string &name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    std::string ext = name.substr(dot);
    for (char &c : ext)
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    return ext == ".md" || ext == ".markdown";
}

static bool safeTemplateName(const std::string &name) {
    if (name.empty() || name == "." || name == "..") return false;
    return name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}

static void replaceAll(std::string &text, const std::string &from, const std::string &to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::vector<std::string> journalTemplateList() {
    std::vector<std::string> out;
    DIR *d = opendir(kJOURNAL_TEMPLATE_DIR);
    if (!d) return out;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name.empty() || name[0] == '.' || !hasMarkdownExt(name)) continue;
        std::string path = std::string(kJOURNAL_TEMPLATE_DIR) + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string journalTemplateDisplayName(const std::string &filename) {
    size_t dot = filename.rfind('.');
    return dot == std::string::npos ? filename : filename.substr(0, dot);
}

std::string journalTemplateSelected() {
    return g_settings.getString(TEMPLATE_SETTING_KEY);
}

void journalTemplateSelect(const std::string &filename) {
    if (safeTemplateName(filename)) g_settings.setString(TEMPLATE_SETTING_KEY, filename);
}

bool journalTemplateEnabled() {
    return journalTemplateSelected() != TEMPLATE_NONE_VALUE;
}

void journalTemplateSelectNone() {
    g_settings.setString(TEMPLATE_SETTING_KEY, TEMPLATE_NONE_VALUE);
}

std::string journalTemplateExpand(const std::string &text, time_t now) {
    struct tm tmv = {};
    localtime_r(&now, &tmv);

    char date[16];
    strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
    static const char *weekdays[] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
    };
    const char *weekday = weekdays[tmv.tm_wday >= 0 && tmv.tm_wday < 7 ? tmv.tm_wday : 0];

    std::string out = text;
    replaceAll(out, "{{date}}", date);
    replaceAll(out, "{{ date }}", date);
    replaceAll(out, "{{weekday}}", weekday);
    replaceAll(out, "{{ weekday }}", weekday);
    return out;
}

bool journalTemplateLoad(const std::string &filename, std::string &out) {
    if (!safeTemplateName(filename) || !hasMarkdownExt(filename)) return false;
    std::string path = std::string(kJOURNAL_TEMPLATE_DIR) + "/" + filename;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size > kMaxTemplateBytes)
        return false;
    std::string text = readWholeFile(path);
    // 兼容 Windows 编辑器的 BOM / CRLF，避免正文里混入不可见字符。
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    out = journalTemplateExpand(text, time(nullptr));
    return true;
}

void journalTemplateEnsureDefaults() {
    struct stat st;
    // 只在 Templates 目录完全不存在时初始化；目录已存在时不动用户的文件
    // （包括用户删光后自己建的空目录）。
    if (stat(kJOURNAL_TEMPLATE_DIR, &st) == 0 && S_ISDIR(st.st_mode)) return;
    if (!ensureDirPath(kJOURNAL_TEMPLATE_DIR)) {
        ESP_LOGE(TAG, "Cannot create %s", kJOURNAL_TEMPLATE_DIR);
        return;
    }

    int written = 0;
    for (const auto &tpl : kBuiltinTemplates) {
        std::string path = std::string(kJOURNAL_TEMPLATE_DIR) + "/" + tpl.name;
        if (fileExists(path)) continue;
        std::string content((const char *)tpl.data, tpl.size);
        if (safeWriteFile(path, content)) written++;
        else ESP_LOGE(TAG, "Failed to write default template %s", path.c_str());
    }
    ESP_LOGI(TAG, "Seeded %d default templates into %s", written,
             kJOURNAL_TEMPLATE_DIR);
}

bool journalTemplateLoadSelected(std::string &out, std::string &error) {
    if (!journalTemplateEnabled()) {
        error = "模板日记已关闭";
        return false;
    }
    std::string filename = journalTemplateSelected();
    if (filename.empty()) {
        std::vector<std::string> list = journalTemplateList();
        if (list.empty()) {
            error = "未找到模板 (SD卡/Templates)";
            return false;
        }
        filename = list.front();
        journalTemplateSelect(filename);
    }
    if (!journalTemplateLoad(filename, out)) {
        // 模板被删除或改名：退回第一个仍存在的模板。
        std::vector<std::string> list = journalTemplateList();
        if (list.empty()) {
            error = "未找到模板 (SD卡/Templates)";
            return false;
        }
        filename = list.front();
        journalTemplateSelect(filename);
        if (!journalTemplateLoad(filename, out)) {
            error = "模板读取失败";
            return false;
        }
    }
    return true;
}
