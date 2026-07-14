#include "flomo_client.h"
#include "settings_manager.h"
#include "json_utils.h"
#include <cstring>
#include <cstdio>
#include <ctime>
#include <vector>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <mbedtls/md5.h>

static const char *TAG = "Flomo";
FlomoClient g_flomo;

// Apply inline formatting: **bold**, __underline__, ==highlight==
static std::string applyInlineFormats(const std::string &text) {
    std::string result = text;

    // **bold** -> <strong>bold</strong>
    size_t pos = 0;
    while ((pos = result.find("**", pos)) != std::string::npos) {
        size_t end = result.find("**", pos + 2);
        if (end != std::string::npos) {
            std::string bold = result.substr(pos + 2, end - pos - 2);
            result.replace(pos, end - pos + 2, "<strong>" + bold + "</strong>");
            pos += 8 + bold.length(); // skip past </strong>
        } else {
            break;
        }
    }

    // __underline__ -> <u>underline</u>
    pos = 0;
    while ((pos = result.find("__", pos)) != std::string::npos) {
        size_t end = result.find("__", pos + 2);
        if (end != std::string::npos) {
            std::string underline = result.substr(pos + 2, end - pos - 2);
            result.replace(pos, end - pos + 2, "<u>" + underline + "</u>");
            pos += 7 + underline.length(); // skip past </u>
        } else {
            break;
        }
    }

    // ==highlight== -> <mark>highlight</mark>
    pos = 0;
    while ((pos = result.find("==", pos)) != std::string::npos) {
        size_t end = result.find("==", pos + 2);
        if (end != std::string::npos) {
            std::string highlight = result.substr(pos + 2, end - pos - 2);
            result.replace(pos, end - pos + 2, "<mark>" + highlight + "</mark>");
            pos += 12 + highlight.length(); // skip past </mark>
        } else {
            break;
        }
    }

    return result;
}

// Convert plain text to HTML format for Flomo
// Supports: **bold**, __underline__, ==highlight==, - list items
static std::string textToHtml(const std::string &text) {
    if (text.empty()) return "";

    // If already starts with '<', assume it's already HTML
    if (text[0] == '<') return text;

    std::string result;
    std::vector<std::string> lines;

    // Split text into lines
    size_t start = 0;
    for (size_t i = 0; i <= text.length(); i++) {
        if (i == text.length() || text[i] == '\n') {
            if (i > start) {
                lines.push_back(text.substr(start, i - start));
            } else {
                lines.push_back("");
            }
            start = i + 1;
        }
    }

    bool inList = false;
    for (const auto &line : lines) {
        // Trim leading/trailing spaces for processing
        size_t trimStart = line.find_first_not_of(" \t");
        size_t trimEnd = line.find_last_not_of(" \t");
        std::string trimmed = (trimStart == std::string::npos) ? "" :
            line.substr(trimStart, trimEnd == std::string::npos ? std::string::npos : trimEnd - trimStart + 1);

        // Check for list items (- or * followed by space)
        bool isListItem = (trimmed.length() >= 2 && trimmed[0] == '-' && trimmed[1] == ' ') ||
                         (trimmed.length() >= 2 && trimmed[0] == '*' && trimmed[1] == ' ');

        if (isListItem) {
            if (!inList) {
                result += "<ul>";
                inList = true;
            }
            std::string content = trimmed.substr(2);
            content = applyInlineFormats(content);
            result += "<li>" + content + "</li>";
        } else {
            if (inList) {
                result += "</ul>";
                inList = false;
            }
            if (trimmed.empty()) {
                result += "<p><br></p>";
            } else {
                std::string content = applyInlineFormats(trimmed);
                result += "<p>" + content + "</p>";
            }
        }
    }

    if (inList) {
        result += "</ul>";
    }

    return result;
}

// Flomo constants
#define FLOMO_API_BASE    "https://flomoapp.com/api/v1"
#define FLOMO_API_KEY     "flomo_web"
#define FLOMO_APP_VERSION "4.0"
#define FLOMO_PLATFORM    "web"
#define FLOMO_SIGN_SECRET "dbbc3dd73364b4084c3a69346e0ce2b2"

FlomoClient::FlomoClient() {}

void FlomoClient::configure(const std::string &email, const std::string &password) {
    email_ = email;
    password_ = password;
}

bool FlomoClient::isConfigured() const {
    return !email_.empty() && !password_.empty();
}

std::string FlomoClient::generateSign(const std::string &sortedParams) {
    std::string raw = sortedParams + FLOMO_SIGN_SECRET;
    unsigned char md5[16];
    mbedtls_md5((const unsigned char *)raw.c_str(), raw.size(), md5);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", md5[i]);
    return std::string(hex, 32);
}

std::string FlomoClient::login() {
    if (!isConfigured()) return "";

    time_t now;
    time(&now);
    char ts[16];
    snprintf(ts, sizeof(ts), "%lld", (long long)now);

    // Build params for signing
    std::string params = "api_key=" + std::string(FLOMO_API_KEY)
        + "&app_version=" + std::string(FLOMO_APP_VERSION)
        + "&email=" + email_
        + "&password=" + password_
        + "&platform=" + std::string(FLOMO_PLATFORM)
        + "&timestamp=" + ts
        + "&webp=1";
    std::string sign = generateSign(params);

    // Build JSON body
    char body[512];
    snprintf(body, sizeof(body),
        "{\"email\":\"%s\",\"password\":\"%s\",\"wechat_union_id\":\"\","
        "\"wechat_oa_open_id\":\"\",\"timestamp\":\"%s\","
        "\"api_key\":\"%s\",\"app_version\":\"%s\","
        "\"platform\":\"%s\",\"webp\":\"1\",\"sign\":\"%s\"}",
        json_escape(email_).c_str(), json_escape(password_).c_str(), ts,
        FLOMO_API_KEY, FLOMO_APP_VERSION, FLOMO_PLATFORM, sign.c_str());

    esp_http_client_config_t cfg = {};
    cfg.url = FLOMO_API_BASE "/user/login_by_email";
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return "";

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");

    std::string response;
    std::string token;
    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err == ESP_OK) {
        esp_http_client_write(client, body, (int)strlen(body));
        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[512];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                response += buf;
            }
            // Parse JSON for access_token (simple search)
            // Response format: {"code":0,"data":{"access_token":"xxx"}}
            // First check code == 0, then look for access_token in data
            auto dataPos = response.find("\"data\"");
            if (dataPos != std::string::npos) {
                auto tokPos = response.find("\"access_token\"", dataPos);
                if (tokPos != std::string::npos) {
                    auto valStart = response.find('"', tokPos + 14);
                    if (valStart != std::string::npos) {
                        valStart++;
                        auto valEnd = response.find('"', valStart);
                        if (valEnd != std::string::npos) {
                            token = response.substr(valStart, valEnd - valStart);
                        }
                    }
                }
            }
            // Fallback: try top-level access_token (older API format)
            if (token.empty()) {
                auto tokPos = response.find("\"access_token\"");
                if (tokPos != std::string::npos) {
                    auto valStart = response.find('"', tokPos + 14);
                    if (valStart != std::string::npos) {
                        valStart++;
                        auto valEnd = response.find('"', valStart);
                        if (valEnd != std::string::npos) {
                            token = response.substr(valStart, valEnd - valStart);
                        }
                    }
                }
            }
            if (token.empty()) {
                ESP_LOGW(TAG, "Login 200 but no token in response: %.*s", (int)response.size(), response.c_str());
            }
        } else {
            ESP_LOGW(TAG, "Login returned HTTP %d (content-length: %d)", status, content_length);
            // Read response body for diagnostics
            char buf[256];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                response += buf;
            }
            if (!response.empty())
                ESP_LOGW(TAG, "Login response: %.*s", (int)response.size(), response.c_str());
        }
    } else {
        ESP_LOGW(TAG, "Login open failed: %d", err);
    }

    esp_http_client_cleanup(client);
    return token;
}

bool FlomoClient::createMemo(const std::string &token, const std::string &content) {
    if (token.empty()) return false;

    time_t now;
    time(&now);
    char ts[16];
    snprintf(ts, sizeof(ts), "%lld", (long long)now);

    std::string params = "api_key=" + std::string(FLOMO_API_KEY)
        + "&app_version=" + std::string(FLOMO_APP_VERSION)
        + "&content=" + content
        + "&platform=" + std::string(FLOMO_PLATFORM)
        + "&source=web"
        + "&timestamp=" + ts
        + "&tz=8:0"
        + "&webp=1";
    std::string sign = generateSign(params);

    char body[2048];
    std::string escapedContent = json_escape(content);
    int needed = snprintf(nullptr, 0,
        "{\"timestamp\":\"%s\",\"api_key\":\"%s\",\"app_version\":\"%s\","
        "\"platform\":\"%s\",\"webp\":\"1\",\"content\":\"%s\","
        "\"source\":\"web\",\"tz\":\"8:0\",\"sign\":\"%s\"}",
        ts, FLOMO_API_KEY, FLOMO_APP_VERSION, FLOMO_PLATFORM,
        escapedContent.c_str(), sign.c_str());
    if (needed >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "Content too long for Flomo (need %d bytes, have %d)", needed, (int)sizeof(body));
        return false;
    }
    snprintf(body, sizeof(body),
        "{\"timestamp\":\"%s\",\"api_key\":\"%s\",\"app_version\":\"%s\","
        "\"platform\":\"%s\",\"webp\":\"1\",\"content\":\"%s\","
        "\"source\":\"web\",\"tz\":\"8:0\",\"sign\":\"%s\"}",
        ts, FLOMO_API_KEY, FLOMO_APP_VERSION, FLOMO_PLATFORM,
        escapedContent.c_str(), sign.c_str());

    esp_http_client_config_t cfg = {};
    cfg.url = FLOMO_API_BASE "/memo";
    cfg.method = HTTP_METHOD_PUT;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");
    std::string bearer = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", bearer.c_str());

    bool ok = false;
    std::string response;
    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    if (err == ESP_OK) {
        esp_http_client_write(client, body, (int)strlen(body));
        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[256];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                response += buf;
            }
            // Parse code field from JSON response
            auto codePos = response.find("\"code\":0");
            if (codePos != std::string::npos) {
                ok = true;
            } else {
                ESP_LOGW(TAG, "Memo 200 but code not 0: %.*s", (int)response.size(), response.c_str());
            }
        } else {
            ESP_LOGW(TAG, "Memo returned HTTP %d (content-length: %d)", status, content_length);
            char buf[256];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                response += buf;
            }
            if (!response.empty())
                ESP_LOGW(TAG, "Memo response: %.*s", (int)response.size(), response.c_str());
        }
    } else {
        ESP_LOGW(TAG, "Memo open failed: %d", err);
    }

    esp_http_client_cleanup(client);
    return ok;
}

FlomoResult FlomoClient::send(const std::string &text) {
    if (!isConfigured()) {
        std::string email = g_settings.flomoEmail();
        std::string pass = g_settings.flomoPassword();
        if (!email.empty() && !pass.empty()) configure(email, pass);
    }
    if (!isConfigured()) {
        return {false, "请先在设置中配置Flomo账号"};
    }

    // Split text into chunks of <= 5000 chars (Flomo limit)
    const size_t MAX_CHUNK = 5000;
    std::vector<std::string> chunks;
    if (text.length() <= MAX_CHUNK) {
        chunks.push_back(text);
    } else {
        for (size_t i = 0; i < text.length(); i += MAX_CHUNK) {
            chunks.push_back(text.substr(i, MAX_CHUNK));
        }
    }

    int success = 0;
    int failed = 0;

    // Try cached token first
    std::string token = getCachedToken();
    if (!token.empty()) {
        for (auto &chunk : chunks) {
            std::string htmlContent = textToHtml(chunk);
            htmlContent += "\n\n<p>#日记</p>";
            if (createMemo(token, htmlContent)) {
                success++;
            } else {
                // Cached token failed, try re-login
                token = "";
                break;
            }
        }
        if (success == (int)chunks.size()) {
            return {true, chunks.size() == 1 ? "已发送到Flomo ✓" : ("已发送" + std::to_string(success) + "条到Flomo ✓")};
        }
    }

    // Re-login if needed
    if (token.empty()) {
        token = login();
        if (!token.empty()) {
            setCachedToken(token);
            for (auto &chunk : chunks) {
                std::string htmlContent = textToHtml(chunk);
                htmlContent += "\n\n<p>#日记</p>";
                if (createMemo(token, htmlContent)) {
                    success++;
                } else {
                    failed++;
                }
            }
        } else {
            setCachedToken("");
            return {false, "Flomo登录失败，请检查账号密码"};
        }
    }

    if (failed == 0 && success > 0) {
        return {true, chunks.size() == 1 ? "已发送到Flomo ✓" : ("已发送" + std::to_string(success) + "条到Flomo ✓")};
    } else if (success > 0) {
        return {false, "部分发送成功（" + std::to_string(success) + "/" + std::to_string(chunks.size()) + "）"};
    }
    return {false, "发送到Flomo失败"};
}

std::string FlomoClient::getCachedToken() {
    return g_settings.flomoToken();
}

void FlomoClient::setCachedToken(const std::string &token) {
    g_settings.setFlomoToken(token);
    ESP_LOGI(TAG, "Token saved to settings");
}
