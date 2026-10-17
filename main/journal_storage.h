#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <unordered_set>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_err.h>
#include <sdmmc_cmd.h>

struct JournalEntry {
    std::string filename;   // YYYY-MM-DD_HHMMSS.txt
    std::string date;       // YYYY-MM-DD
    std::string title;      // prompt or "自由写作"
    std::string preview;    // first line of body
    std::string full_text;  // full file content
};

struct JournalHistoryVersion {
    std::string filename;   // history snapshot filename
    time_t mtime = 0;
    size_t size = 0;
};

class JournalStorage {
public:
    bool begin();
    // 卸载应用侧 FATFS。IDF 6.1 必须传挂载时返回的原始 card 指针(传 nullptr
    // 直接返回 ESP_ERR_INVALID_ARG), 所以这里保存了 card_。失败时保持 mounted_ 不变。
    esp_err_t deinit();

    // Save a new entry
    bool saveEntry(const std::string &text);

    // Save entry with explicit filename (for sync downloads)
    bool saveEntryRaw(const std::string &filename, const std::string &content, bool createHistory = true);

    // Write a recovery draft outside the normal journal index.
    bool saveRecoveryDraft(const std::string &content, const std::string &meta);
    bool loadRecoveryDraft(std::string &content, std::string &meta);
    void clearRecoveryDraft();

    std::vector<JournalHistoryVersion> listHistoryVersions(const std::string &filename);
    std::string readHistoryVersion(const std::string &filename, const std::string &historyFilename);
    bool restoreHistoryVersion(const std::string &filename, const std::string &historyFilename);
    bool deleteHistoryVersion(const std::string &filename, const std::string &historyFilename);

    // List all entries, newest first
    std::vector<JournalEntry> listEntries();

    // List filenames with modification times (for sync)
    std::vector<std::pair<std::string, time_t>> listFileMtimes();

    // Read entry content by filename
    std::string readEntry(const std::string &filename);

    // Delete entry by filename
    bool deleteEntry(const std::string &filename);

    // Check if date has entry
    bool hasEntry(const std::string &date);

    // Count entries for today
    int countToday();

    // Calculate streak
    int getStreak();

    // Total entries
    int totalEntries();

    // SD card status
    bool isMounted() const { return mounted_; }

    // Get SD card mutex for thread-safe access from external callers
    static SemaphoreHandle_t sdMutex();

private:
    std::string basePath();
    void ensureDir();

    // In-memory index of journal filenames (newest first) + distinct dates.
    // Avoids repeated full-directory scans during boot and main screen draw.
    void scanIndex();                  // rebuild both index structures
    void ensureIndex();                // lazy rebuild if invalid
    void indexAddFile(const std::string &fn);
    void indexRemoveFile(const std::string &fn);
    std::vector<std::string> m_fileIndex;      // newest first
    std::unordered_set<std::string> m_dateSet; // distinct YYYY-MM-DD dates
    bool m_indexValid = false;

    bool mounted_ = false;
    sdmmc_card_t *card_ = nullptr;  // esp_vfs_fat_sdcard_unmount() 需要原始指针
};

extern JournalStorage g_journal;
