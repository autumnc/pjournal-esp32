#include "webdav_client.h"
#include "journal_storage.h"
#include <cstring>
#include <map>
#include <set>
#include <esp_log.h>
#include <esp_http_client.h>
#include <mbedtls/base64.h>

static const char *TAG = "WebDAV";
WebDavClient g_webdav;

// Simple XML parser for PROPFIND multi-status responses
// Finds all <d:response> elements and extracts href, getlastmodified
struct PropfindEntry {
    std::string href;
    std::string lastModified;
    bool isCollection = false;
};

static std::vector<PropfindEntry> parsePropfindResponse(const std::string &xml) {
    std::vector<PropfindEntry> entries;
    // Find each <d:response> block
    const char *p = xml.c_str();
    while (true) {
        const char *respStart = strstr(p, "<d:response>");
        if (!respStart) {
            respStart = strstr(p, "<response>");
            if (!respStart) break;
        }
        const char *respEnd = strstr(respStart, "</d:response>");
        if (!respEnd) {
            respEnd = strstr(respStart, "</response>");
            if (!respEnd) break;
        }
        std::string block(respStart, respEnd - respStart + 14);

        PropfindEntry entry;

        // href
        auto hrefTag = block.find("<d:href>");
        if (hrefTag == std::string::npos) hrefTag = block.find("<href>");
        if (hrefTag != std::string::npos) {
            auto hrefStart = block.find('>', hrefTag) + 1;
            auto hrefEnd = block.find("</", hrefStart);
            if (hrefEnd != std::string::npos) {
                entry.href = block.substr(hrefStart, hrefEnd - hrefStart);
            }
        }

        // collection check
        if (block.find("<d:collection") != std::string::npos ||
            block.find("<collection") != std::string::npos) {
            entry.isCollection = true;
        }

        // getlastmodified
        auto lmTag = block.find("<d:getlastmodified>");
        if (lmTag == std::string::npos) lmTag = block.find("<getlastmodified>");
        if (lmTag != std::string::npos) {
            auto lmStart = block.find('>', lmTag) + 1;
            auto lmEnd = block.find("</", lmStart);
            if (lmEnd != std::string::npos) {
                entry.lastModified = block.substr(lmStart, lmEnd - lmStart);
            }
        } else {
            // Try <d:getlastmodified xmlns="DAV:">
            lmTag = block.find("getlastmodified");
            if (lmTag != std::string::npos) {
                auto lmStart = block.find('>', lmTag) + 1;
                auto lmEnd = block.find("</", lmStart);
                if (lmEnd != std::string::npos) {
                    entry.lastModified = block.substr(lmStart, lmEnd - lmStart);
                }
            }
        }

        entries.push_back(entry);
        p = respEnd + 1;
    }
    return entries;
}

WebDavClient::WebDavClient() {}
WebDavClient::~WebDavClient() {}

void WebDavClient::configure(const std::string &url, const std::string &username, const std::string &password) {
    baseUrl_ = url;
    username_ = username;
    password_ = password;
    if (!baseUrl_.empty() && baseUrl_.back() != '/') baseUrl_ += '/';
}

bool WebDavClient::isConfigured() const {
    return !baseUrl_.empty() && !username_.empty() && !password_.empty();
}

std::string WebDavClient::buildUrl(const std::string &path) const {
    if (path.empty() || path == "/") return baseUrl_;
    std::string url = baseUrl_;
    // Remove leading slash from path if base already ends with one
    if (!path.empty() && path[0] == '/') url += path.substr(1);
    else url += path;
    return url;
}

std::string WebDavClient::authHeader() const {
    // Basic auth: base64(username:password)
    std::string raw = username_ + ":" + password_;
    size_t outLen = 0;
    mbedtls_base64_encode(nullptr, 0, &outLen, (const uint8_t *)raw.data(), raw.size());
    std::string b64(outLen, '\0');
    mbedtls_base64_encode((uint8_t *)&b64[0], outLen, &outLen,
                          (const uint8_t *)raw.data(), raw.size());
    // Remove null terminator
    if (!b64.empty() && b64.back() == '\0') b64.pop_back();
    return "Basic " + b64;
}

// HTTP request helper: returns response body
static std::string httpRequest(const std::string &url, const std::string &method,
                                const std::string &auth, const std::string &body = "",
                                const std::string &contentType = "",
                                const std::string &extraHeader = "",
                                int *outStatusCode = nullptr) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = method == "GET" ? HTTP_METHOD_GET :
                 method == "PUT" ? HTTP_METHOD_PUT :
                 method == "DELETE" ? HTTP_METHOD_DELETE :
                 method == "MKCOL" ? HTTP_METHOD_MKCOL :
                 method == "HEAD" ? HTTP_METHOD_HEAD :
                 HTTP_METHOD_PROPFIND;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return "";

    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");
    if (!auth.empty()) esp_http_client_set_header(client, "Authorization", auth.c_str());
    if (!contentType.empty()) esp_http_client_set_header(client, "Content-Type", contentType.c_str());
    if (!extraHeader.empty()) esp_http_client_set_header(client, "Depth", extraHeader.c_str());

    if (!body.empty() && method != "GET" && method != "HEAD") {
        esp_http_client_set_post_field(client, body.c_str(), (int)body.size());
    }

    std::string response;
    int status = 0;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        // Read response body
        char buf[512];
        int len;
        while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
            buf[len] = 0;
            response += buf;
        }
    } else {
        ESP_LOGW(TAG, "HTTP %s %s failed: %d", method.c_str(), url.c_str(), err);
        status = -1;
    }

    esp_http_client_cleanup(client);
    if (outStatusCode) *outStatusCode = status;
    return response;
}

bool WebDavClient::ensureDirectory(const std::string &path) {
    if (!isConfigured()) return false;
    std::string url = buildUrl(path);
    std::string auth = authHeader();
    int status = 0;
    httpRequest(url, "MKCOL", auth, "", "", "", &status);
    // 201=Created, 405=Already exists, 200/301/302=OK
    return (status == 201 || status == 200 || status == 405 || status == 301 || status == 302);
}

bool WebDavClient::upload(const std::string &remotePath, const std::string &content) {
    if (!isConfigured()) return false;
    std::string url = buildUrl(remotePath);
    std::string auth = authHeader();
    int status = 0;
    httpRequest(url, "PUT", auth, content, "text/plain; charset=utf-8", "", &status);
    return (status == 200 || status == 201 || status == 204);
}

std::string WebDavClient::download(const std::string &remotePath) {
    if (!isConfigured()) return "";
    std::string url = buildUrl(remotePath);
    std::string auth = authHeader();
    int status = 0;
    auto body = httpRequest(url, "GET", auth, "", "", "", &status);
    if (status == 200 || status == 203) return body;
    return "";
}

std::vector<std::pair<std::string, std::string>> WebDavClient::listFiles(const std::string &dirPath) {
    std::vector<std::pair<std::string, std::string>> result;
    if (!isConfigured()) return result;

    std::string url = buildUrl(dirPath);
    std::string auth = authHeader();

    const char *propfindBody =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\">"
        "<d:prop><d:getlastmodified/><d:resourcetype/></d:prop>"
        "</d:propfind>";

    int status = 0;
    auto body = httpRequest(url, "PROPFIND", auth, propfindBody,
                            "application/xml; charset=utf-8", "1", &status);

    if (status != 207 && status != 200) return result;

    auto entries = parsePropfindResponse(body);
    for (auto &e : entries) {
        if (e.isCollection) continue;
        // Extract filename from href
        auto slash = e.href.rfind('/');
        std::string fname;
        if (slash != std::string::npos) fname = e.href.substr(slash + 1);
        else fname = e.href;

        // URL decode the filename
        std::string decoded;
        for (size_t i = 0; i < fname.size(); i++) {
            if (fname[i] == '%' && i + 2 < fname.size()) {
                char hex[3] = {fname[i+1], fname[i+2], 0};
                decoded += (char)strtol(hex, nullptr, 16);
                i += 2;
            } else {
                decoded += fname[i];
            }
        }

        if (decoded.empty() || decoded.size() < 10) continue; // skip dir entries
        result.push_back({decoded, e.lastModified});
    }
    return result;
}

std::string WebDavClient::headFile(const std::string &remotePath) {
    if (!isConfigured()) return "";
    std::string url = buildUrl(remotePath);
    std::string auth = authHeader();
    int status = 0;
    httpRequest(url, "HEAD", auth, "", "", "", &status);
    if (status == 200 || status == 203) {
        // We could get Last-Modified from the headers,
        // but for simplicity return "exists" for now
        return "exists";
    }
    return "";
}

bool WebDavClient::remove(const std::string &remotePath) {
    if (!isConfigured()) return false;
    std::string url = buildUrl(remotePath);
    std::string auth = authHeader();
    int status = 0;
    httpRequest(url, "DELETE", auth, "", "", "", &status);
    return (status == 200 || status == 204 || status == 404);
}

SyncResult WebDavClient::sync(const std::string &localDir) {
    if (!isConfigured()) {
        return {false, "请先在设置中配置 WebDAV"};
    }

    // Ensure remote journal directory exists
    std::string remoteDir = "journal/";
    if (!ensureDirectory(remoteDir)) {
        if (!ensureDirectory("")) {
            return {false, "无法创建远程目录"};
        }
    }

    // Get local files
    auto localEntries = g_journal.listEntries();
    std::map<std::string, std::string> localFiles;
    for (auto &e : localEntries) {
        localFiles[e.filename] = ""; // We don't track local mtime easily on SPIFFS
    }

    // Get remote files
    auto remoteFiles = listFiles(remoteDir);
    std::map<std::string, std::string> remoteMap;
    for (auto &rf : remoteFiles) {
        remoteMap[rf.first] = rf.second; // mtime string
    }

    int uploaded = 0, downloaded = 0, skipped = 0, failed = 0;

    // Union of all filenames
    std::set<std::string> allFiles;
    for (auto &lf : localFiles) allFiles.insert(lf.first);
    for (auto &rf : remoteMap) allFiles.insert(rf.first);

    for (auto &fname : allFiles) {
        bool localExists = localFiles.find(fname) != localFiles.end();
        bool remoteExists = remoteMap.find(fname) != remoteMap.end();

        if (localExists && !remoteExists) {
            // Upload new local file
            std::string content = g_journal.readEntry(fname);
            if (!content.empty()) {
                if (upload(remoteDir + fname, content)) uploaded++;
                else failed++;
            }
        } else if (!localExists && remoteExists) {
            // Download new remote file
            std::string content = download(remoteDir + fname);
            if (!content.empty()) {
                // Save via journal storage: create the file directly
                // Use existing save mechanism
                g_journal.saveEntryRaw(fname, content);
                downloaded++;
            } else failed++;
        } else if (localExists && remoteExists) {
            // Both exist: skip for simplicity (could compare mtime)
            skipped++;
        }
    }

    char msg[128];
    if (uploaded > 0 || downloaded > 0 || skipped > 0 || failed > 0) {
        snprintf(msg, sizeof(msg), "上传%d 下载%d 跳过%d 失败%d",
                 uploaded, downloaded, skipped, failed);
    } else {
        snprintf(msg, sizeof(msg), "无需同步");
    }

    if (failed > 0 && uploaded + downloaded == 0)
        return {false, msg};
    return {true, msg};
}
