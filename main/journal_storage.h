#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

struct JournalEntry {
    std::string filename;   // YYYY-MM-DD_HHMMSS.txt
    std::string date;       // YYYY-MM-DD
    std::string title;      // prompt or "自由写作"
    std::string preview;    // first line of body
    std::string full_text;  // full file content
};

class JournalStorage {
public:
    bool begin();
    void deinit();

    // Save a new entry
    bool saveEntry(const std::string &text);

    // Save entry with explicit filename (for sync downloads)
    bool saveEntryRaw(const std::string &filename, const std::string &content);

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

private:
    std::string basePath();
    void ensureDir();
    bool mounted_ = false;
};

extern JournalStorage g_journal;
