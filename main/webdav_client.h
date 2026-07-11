#pragma once

#include <string>
#include <vector>

struct SyncResult {
    bool success;
    std::string message;
};

class WebDavClient {
public:
    WebDavClient();
    ~WebDavClient();

    void configure(const std::string &url, const std::string &username, const std::string &password);
    bool isConfigured() const;

    // Ensure remote directory exists (MKCOL)
    bool ensureDirectory(const std::string &path);

    // Upload a file (PUT)
    bool upload(const std::string &remotePath, const std::string &content);

    // Download a file (GET), returns content or empty on failure
    std::string download(const std::string &remotePath);

    // List files with mtime (PROPFIND)
    // Returns {filename: iso_timestamp}
    std::vector<std::pair<std::string, std::string>> listFiles(const std::string &dirPath);

    // Check if file exists and get its mtime (HEAD)
    std::string headFile(const std::string &remotePath);

    // Delete a file (DELETE)
    bool remove(const std::string &remotePath);

    // Bidirectional sync with local journal entries
    SyncResult sync(const std::string &localDir);

private:
    std::string baseUrl_;
    std::string username_;
    std::string password_;

    std::string buildUrl(const std::string &path) const;
    std::string authHeader() const;
};

extern WebDavClient g_webdav;
