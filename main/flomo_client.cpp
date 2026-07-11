#include "flomo_client.h"
#include "settings_manager.h"
#include <cstring>
#include <cstdio>
#include <ctime>
#include <esp_log.h>
#include <esp_http_client.h>
#include <mbedtls/md5.h>

static const char *TAG = "Flomo";
FlomoClient g_flomo;

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
        email_.c_str(), password_.c_str(), ts,
        FLOMO_API_KEY, FLOMO_APP_VERSION, FLOMO_PLATFORM, sign.c_str());

    esp_http_client_config_t cfg = {};
    cfg.url = FLOMO_API_BASE "/user/login_by_email";
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return "";

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    std::string response;
    std::string token;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[512];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                response += buf;
            }
            // Parse JSON for access_token (simple search)
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
    } else {
        ESP_LOGW(TAG, "Login failed: %d", err);
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

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"timestamp\":\"%s\",\"api_key\":\"%s\",\"app_version\":\"%s\","
        "\"platform\":\"%s\",\"webp\":\"1\",\"content\":\"%s\","
        "\"source\":\"web\",\"tz\":\"8:0\",\"sign\":\"%s\"}",
        ts, FLOMO_API_KEY, FLOMO_APP_VERSION, FLOMO_PLATFORM,
        content.c_str(), sign.c_str());

    esp_http_client_config_t cfg = {};
    cfg.url = FLOMO_API_BASE "/memo";
    cfg.method = HTTP_METHOD_PUT;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");
    std::string bearer = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", bearer.c_str());
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[256];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                // Parse code field from JSON response
                auto codePos = std::string(buf).find("\"code\":0");
                if (codePos != std::string::npos) {
                    ok = true;
                    break;
                }
            }
        }
    }

    esp_http_client_cleanup(client);
    return ok;
}

FlomoResult FlomoClient::send(const std::string &text) {
    if (!isConfigured()) {
        return {false, "请先在设置中配置Flomo账号"};
    }

    // Format content with HTML tag and hashtag
    std::string content = "<p>" + text + "\n\n#日记</p>";

    // Try cached token first
    std::string token = getCachedToken();
    if (!token.empty()) {
        if (createMemo(token, content)) {
            return {true, "已发送到Flomo ✓"};
        }
    }

    // Re-login
    token = login();
    if (!token.empty()) {
        setCachedToken(token);
        if (createMemo(token, content)) {
            return {true, "已发送到Flomo ✓"};
        }
        return {false, "发送到Flomo失败"};
    }

    // Clear invalid token
    setCachedToken("");
    return {false, "Flomo登录失败，请检查账号密码"};
}

std::string FlomoClient::getCachedToken() {
    return g_settings.flomoToken();
}

void FlomoClient::setCachedToken(const std::string &token) {
    g_settings.setFlomoToken(token);
}
