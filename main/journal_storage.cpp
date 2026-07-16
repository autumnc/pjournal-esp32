#include "journal_storage.h"
#include <cstring>
#include <ctime>
#include <algorithm>
#include <unordered_set>
#include <dirent.h>
#include <sys/stat.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *TAG = "Journal";

static bool isJournalExt(const std::string &fn) {
    auto dot = fn.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = fn.substr(dot);
    return ext == ".txt" || ext == ".md";
}

// SD card mutex (recursive to handle nested public method calls)
static SemaphoreHandle_t s_sd_mutex = nullptr;

// SDMMC pin configuration for ESP32-S3-RLCD-4.2
#define SDMMC_CLK GPIO_NUM_38
#define SDMMC_CMD GPIO_NUM_21
#define SDMMC_D0  GPIO_NUM_39

JournalStorage g_journal;

bool JournalStorage::begin() {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 16;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk   = SDMMC_CLK;
    slot_config.cmd   = SDMMC_CMD;
    slot_config.d0    = SDMMC_D0;
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = nullptr;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %d", ret);
        mounted_ = false;
        return false;
    }

    if (card) {
        sdmmc_card_print_info(stdout, card);
        mounted_ = true;
    }

    if (!s_sd_mutex) s_sd_mutex = xSemaphoreCreateRecursiveMutex();
    ensureDir();
    ESP_LOGI(TAG, "SD card ready at %s", basePath().c_str());
    return true;
}

void JournalStorage::deinit() {
    if (mounted_) {
        esp_vfs_fat_sdcard_unmount("/sdcard", nullptr);
        mounted_ = false;
    }
}

std::string JournalStorage::basePath() {
    return "/sdcard/pjournal";
}

void JournalStorage::ensureDir() {
    mkdir("/sdcard/pjournal", 0777);
}

bool JournalStorage::saveEntry(const std::string &text) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureDir();
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", tm);
    std::string fname = std::string(ts) + ".txt";

    FILE *f = fopen((basePath() + "/" + fname).c_str(), "w");
    if (!f) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return false; }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    ESP_LOGI(TAG, "Saved: %s", fname.c_str());
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return true;
}

bool JournalStorage::saveEntryRaw(const std::string &filename, const std::string &content) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureDir();
    FILE *f = fopen((basePath() + "/" + filename).c_str(), "w");
    if (!f) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return false; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return true;
}

std::vector<JournalEntry> JournalStorage::listEntries() {
    std::vector<JournalEntry> entries;
    if (!mounted_) return entries;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    DIR *dir = opendir(basePath().c_str());
    if (!dir) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return entries; }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (fn[0] == '.' || !isJournalExt(fn)) continue;

        JournalEntry e;
        e.filename = fn;
        e.date = fn.substr(0, 10);

        // Read only first 512 bytes for preview extraction (performance optimization)
        std::string content;
        std::string path = basePath() + "/" + fn;
        FILE *f = fopen(path.c_str(), "r");
        if (f) {
            char buf[512];
            int len = fread(buf, 1, sizeof(buf) - 1, f);
            if (len > 0) {
                buf[len] = 0;
                content = buf;
            }
            fclose(f);
        }

        if (!content.empty()) {
            size_t pos = content.find('\n');
            if (pos != std::string::npos) {
                std::string first = content.substr(0, pos);
                if (first.find("提示词:") == 0)
                    e.title = "提示写作";
                else if (first.find("自由写作") != std::string::npos)
                    e.title = "自由写作";
                size_t body_start = content.find("\n\n");
                if (body_start != std::string::npos && body_start < content.size()) {
                    std::string body = content.substr(body_start + 2);
                    std::string preview_text;
                    size_t start = 0;
                    while (start < body.size()) {
                        size_t nl = body.find('\n', start);
                        std::string line = (nl != std::string::npos) ? body.substr(start, nl - start) : body.substr(start);
                        // Skip metadata lines and prompt label
                        if (!line.empty() &&
                            line.find("日期:") != 0 &&
                            line.find("字数:") != 0 &&
                            line.find("提示词:") != 0 &&
                            line.find("自由写作") == std::string::npos) {
                            preview_text = line;
                            break;
                        }
                        if (nl == std::string::npos) break;
                        start = nl + 1;
                    }
                    e.preview = preview_text.substr(0, 40);
                }
            }
        }
        entries.push_back(e);
    }
    closedir(dir);

    // Sort by filename descending (newest first)
    std::sort(entries.begin(), entries.end(),
        [](const JournalEntry &a, const JournalEntry &b) {
            return a.filename > b.filename;
        });
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return entries;
}

std::vector<std::pair<std::string, time_t>> JournalStorage::listFileMtimes() {
    std::vector<std::pair<std::string, time_t>> result;
    if (!mounted_) return result;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    DIR *dir = opendir(basePath().c_str());
    if (!dir) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return result; }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (fn[0] == '.') continue;
        if (!isJournalExt(fn)) continue;

        struct stat st;
        std::string full = basePath() + "/" + fn;
        if (stat(full.c_str(), &st) == 0) {
            result.push_back({fn, st.st_mtime});
        }
    }
    closedir(dir);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return result;
}

std::string JournalStorage::readEntry(const std::string &filename) {
    if (!mounted_) return "";
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    FILE *f = fopen((basePath() + "/" + filename).c_str(), "r");
    if (!f) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return ""; }
    std::string result;
    char buf[256];
    int n;
    while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = 0;
        result += buf;
    }
    fclose(f);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return result;
}

bool JournalStorage::deleteEntry(const std::string &filename) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    bool ok = remove((basePath() + "/" + filename).c_str()) == 0;
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

bool JournalStorage::hasEntry(const std::string &date) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    DIR *dir = opendir(basePath().c_str());
    if (!dir) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return false; }

    struct dirent *de;
    bool found = false;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (fn[0] == '.' || !isJournalExt(fn)) continue;
        if (fn.substr(0, 10) == date) { found = true; break; }
    }
    closedir(dir);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return found;
}

int JournalStorage::countToday() {
    if (!mounted_) return 0;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    time_t now; time(&now);
    struct tm *tm = localtime(&now);
    char today[16];
    strftime(today, sizeof(today), "%Y-%m-%d", tm);

    int count = 0;
    DIR *dir = opendir(basePath().c_str());
    if (!dir) { if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex); return 0; }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (fn[0] == '.' || !isJournalExt(fn)) continue;
        if (fn.substr(0, 10) == today) count++;
    }
    closedir(dir);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return count;
}

int JournalStorage::getStreak() {
    if (!mounted_) return 0;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    // Build date set from directory scan (no file content reads)
    std::unordered_set<std::string> dates;
    DIR *dir = opendir(basePath().c_str());
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_type != DT_REG) continue;
            std::string fn = de->d_name;
            if (fn[0] == '.' || !isJournalExt(fn)) continue;
            dates.insert(fn.substr(0, 10));
        }
        closedir(dir);
    }

    int streak = 0;
    time_t now; time(&now);
    for (int i = 0; i < 365; i++) {
        time_t t = now - i * 86400;
        struct tm *tm2 = localtime(&t);
        char date[16];
        strftime(date, sizeof(date), "%Y-%m-%d", tm2);
        if (dates.count(date))
            streak++;
        else
            break;
    }
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return streak;
}

int JournalStorage::totalEntries() {
    if (!mounted_) return 0;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    int count = 0;
    DIR *dir = opendir(basePath().c_str());
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_type != DT_REG) continue;
            std::string fn = de->d_name;
            if (fn[0] == '.' || !isJournalExt(fn)) continue;
            count++;
        }
        closedir(dir);
    }
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return count;
}
