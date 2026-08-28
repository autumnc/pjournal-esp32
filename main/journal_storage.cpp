#include "journal_storage.h"
#include "safe_file.h"
#include "settings_manager.h"
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
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

static std::string stemOf(const std::string &fn) {
    size_t dot = fn.rfind('.');
    return dot == std::string::npos ? fn : fn.substr(0, dot);
}

static void cleanupOldHistory(const std::string &dir, int keep) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (fn.empty() || fn[0] == '.') continue;
        files.push_back(fn);
    }
    closedir(d);
    if ((int)files.size() <= keep) return;
    std::sort(files.begin(), files.end());
    for (int i = 0; i < (int)files.size() - keep; i++) {
        remove((dir + "/" + files[i]).c_str());
    }
}

static bool saveJournalHistoryVersion(const std::string &base, const std::string &filename,
                                      const std::string &oldContent) {
    std::string dir = base + "/.history/" + stemOf(filename);
    if (!ensureDirPath(dir)) return false;
    time_t now;
    time(&now);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d_%H%M%S", tm);
    std::string path = dir + "/" + std::string(ts) + ".txt";
    for (int i = 1; fileExists(path) && i < 100; i++) {
        path = dir + "/" + std::string(ts) + "_" + std::to_string(i) + ".txt";
    }
    bool ok = safeWriteFile(path, oldContent);
    if (ok) cleanupOldHistory(dir, 10);
    return ok;
}

static std::string historyDirFor(const std::string &base, const std::string &filename) {
    return base + "/.history/" + stemOf(filename);
}

static bool isSafeHistoryFilename(const std::string &fn) {
    if (fn.empty() || fn.find('/') != std::string::npos || fn.find("..") != std::string::npos) return false;
    return isJournalExt(fn);
}

// SD card mutex (recursive to handle nested public method calls)
static SemaphoreHandle_t s_sd_mutex = nullptr;

SemaphoreHandle_t JournalStorage::sdMutex() { return s_sd_mutex; }

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

void JournalStorage::scanIndex() {
    m_fileIndex.clear();
    m_dateSet.clear();
    DIR *dir = opendir(basePath().c_str());
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_type != DT_REG) continue;
            std::string fn = de->d_name;
            if (fn.size() > 4 && fn.substr(fn.size() - 4) == ".bak") {
                std::string orig = fn.substr(0, fn.size() - 4);
                std::string origPath = basePath() + "/" + orig;
                if (isJournalExt(orig) && !fileExists(origPath)) {
                    rename((basePath() + "/" + fn).c_str(), origPath.c_str());
                    fn = orig;
                }
            }
            if (fn[0] == '.' || !isJournalExt(fn)) continue;
            m_fileIndex.push_back(fn);
            m_dateSet.insert(fn.substr(0, 10));
        }
        closedir(dir);
    }
    // Newest first
    std::sort(m_fileIndex.begin(), m_fileIndex.end(), std::greater<std::string>());
    m_indexValid = true;
}

void JournalStorage::ensureIndex() {
    if (!m_indexValid) scanIndex();
}

void JournalStorage::indexAddFile(const std::string &fn) {
    // Overwrite case (editor save, sync download of existing file) — already indexed
    for (const auto &f : m_fileIndex) {
        if (f == fn) return;
    }
    auto it = std::lower_bound(m_fileIndex.begin(), m_fileIndex.end(), fn, std::greater<std::string>());
    m_fileIndex.insert(it, fn);
    m_dateSet.insert(fn.substr(0, 10));
}

void JournalStorage::indexRemoveFile(const std::string &fn) {
    auto &v = m_fileIndex;
    v.erase(std::remove(v.begin(), v.end(), fn), v.end());
    std::string date = fn.substr(0, 10);
    for (const auto &f : v) {
        if (f.substr(0, 10) == date) return;  // date still has other files
    }
    m_dateSet.erase(date);
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

    if (!safeWriteFile(basePath() + "/" + fname, text)) {
        if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
        return false;
    }
    ESP_LOGI(TAG, "Saved: %s", fname.c_str());
    if (m_indexValid) indexAddFile(fname);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return true;
}

bool JournalStorage::saveEntryRaw(const std::string &filename, const std::string &content, bool createHistory) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureDir();
    std::string path = basePath() + "/" + filename;
    if (createHistory && g_settings.versionHistory() && filename.rfind("__", 0) != 0 && fileExists(path)) {
        std::string old = readWholeFile(path);
        if (old != content) saveJournalHistoryVersion(basePath(), filename, old);
    }
    if (!safeWriteFile(path, content)) {
        if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
        return false;
    }
    if (m_indexValid) indexAddFile(filename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return true;
}

bool JournalStorage::saveRecoveryDraft(const std::string &content, const std::string &meta) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string dir = basePath() + "/.recovery";
    bool ok = ensureDirPath(dir) &&
              safeWriteFile(dir + "/editor.tmp", content) &&
              safeWriteFile(dir + "/editor.meta", meta);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

bool JournalStorage::loadRecoveryDraft(std::string &content, std::string &meta) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string dir = basePath() + "/.recovery";
    content = readWholeFile(dir + "/editor.tmp");
    meta = readWholeFile(dir + "/editor.meta");
    bool ok = !content.empty() || !meta.empty();
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

void JournalStorage::clearRecoveryDraft() {
    if (!mounted_) return;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string dir = basePath() + "/.recovery";
    remove((dir + "/editor.tmp").c_str());
    remove((dir + "/editor.meta").c_str());
    remove((dir + "/editor.tmp.tmp").c_str());
    remove((dir + "/editor.meta.tmp").c_str());
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
}

std::vector<JournalHistoryVersion> JournalStorage::listHistoryVersions(const std::string &filename) {
    std::vector<JournalHistoryVersion> result;
    if (!mounted_) return result;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string dir = historyDirFor(basePath(), filename);
    DIR *d = opendir(dir.c_str());
    if (!d) {
        if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
        return result;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_type != DT_REG) continue;
        std::string fn = de->d_name;
        if (!isSafeHistoryFilename(fn)) continue;
        struct stat st;
        std::string path = dir + "/" + fn;
        if (stat(path.c_str(), &st) == 0) {
            JournalHistoryVersion v;
            v.filename = fn;
            v.mtime = st.st_mtime;
            v.size = (size_t)st.st_size;
            result.push_back(v);
        }
    }
    closedir(d);
    std::sort(result.begin(), result.end(), [](const JournalHistoryVersion &a, const JournalHistoryVersion &b) {
        if (a.filename != b.filename) return a.filename > b.filename;
        return a.mtime > b.mtime;
    });
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return result;
}

std::string JournalStorage::readHistoryVersion(const std::string &filename, const std::string &historyFilename) {
    if (!mounted_ || !isSafeHistoryFilename(historyFilename)) return "";
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    std::string content = readWholeFile(historyDirFor(basePath(), filename) + "/" + historyFilename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return content;
}

bool JournalStorage::restoreHistoryVersion(const std::string &filename, const std::string &historyFilename) {
    if (!mounted_ || !isSafeHistoryFilename(historyFilename)) return false;
    std::string content = readHistoryVersion(filename, historyFilename);
    if (content.empty()) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureDir();
    std::string path = basePath() + "/" + filename;
    if (fileExists(path)) {
        std::string old = readWholeFile(path);
        if (old != content) saveJournalHistoryVersion(basePath(), filename, old);
    }
    bool ok = safeWriteFile(path, content);
    if (ok && m_indexValid) indexAddFile(filename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

bool JournalStorage::deleteHistoryVersion(const std::string &filename, const std::string &historyFilename) {
    if (!mounted_ || !isSafeHistoryFilename(historyFilename)) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    bool ok = remove((historyDirFor(basePath(), filename) + "/" + historyFilename).c_str()) == 0;
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

std::vector<JournalEntry> JournalStorage::listEntries() {
    std::vector<JournalEntry> entries;
    if (!mounted_) return entries;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);

    // Rebuild index to pick up external changes; index is already newest-first
    scanIndex();

    for (const auto &fn : m_fileIndex) {
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
                        if (line == "自由写作" && e.title.empty()) e.title = "自由写作";
                        if (!line.empty() &&
                            line.find("日期:") != 0 &&
                            line.find("字数:") != 0 &&
                            line.find("提示词:") != 0 &&
                            line != "自由写作") {
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
        if (fn.size() >= 2 && fn[0] == '_' && fn[1] == '_') continue;  // skip temp files
        if (!isJournalExt(fn)) continue;

        struct stat st;
        std::string full = basePath() + "/" + fn;
        repairSafeWriteFile(full);
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
    std::string path = basePath() + "/" + filename;
    repairSafeWriteFile(path);
    FILE *f = fopen(path.c_str(), "r");
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
    if (ok && m_indexValid) indexRemoveFile(filename);
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return ok;
}

bool JournalStorage::hasEntry(const std::string &date) {
    if (!mounted_) return false;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureIndex();
    bool found = m_dateSet.count(date) > 0;
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return found;
}

int JournalStorage::countToday() {
    if (!mounted_) return 0;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureIndex();

    time_t now; time(&now);
    struct tm *tm = localtime(&now);
    char today[16];
    strftime(today, sizeof(today), "%Y-%m-%d", tm);

    int count = 0;
    for (const auto &fn : m_fileIndex) {
        if (fn.substr(0, 10) == today) count++;
    }
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return count;
}

int JournalStorage::getStreak() {
    if (!mounted_) return 0;
    if (s_sd_mutex) xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
    ensureIndex();

    int streak = 0;
    time_t now; time(&now);
    for (int i = 0; i < 365; i++) {
        time_t t = now - i * 86400;
        struct tm *tm2 = localtime(&t);
        char date[16];
        strftime(date, sizeof(date), "%Y-%m-%d", tm2);
        if (m_dateSet.count(date))
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
    ensureIndex();
    int count = (int)m_fileIndex.size();
    if (s_sd_mutex) xSemaphoreGiveRecursive(s_sd_mutex);
    return count;
}
