#include "deepseek_client.h"
#include "settings_manager.h"
#include "json_utils.h"
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

static const char *TAG = "Deepseek";
DeepseekClient g_deepseek;

#define DEEPSEEK_API_URL "https://api.deepseek.com/chat/completions"

DeepseekResult DeepseekClient::generatePrompt(const std::string &userContext) {
    std::string apiKey = g_settings.deepseekKey();
    if (apiKey.empty()) {
        return {false, "请先在设置中配置Deepseek Key"};
    }

    // Build request body with proper JSON escaping
    std::string escapedContext = json_escape(userContext);
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是一个日记写作助手。根据用户的背景和爱好，"
        "运用这些爱好领域内的专业知识、概念、理论和思维方式，生成一个富有洞见和启发性的"
        "日记写作提示（不超过56字），引导用户用该领域的视角观察和记录今天的生活。\"},"
        "{\"role\":\"user\",\"content\":\"我的背景：%s。请生成一个写作提示。\"}],"
        "\"max_tokens\":100,\"temperature\":0.9}",
        escapedContext.c_str());
    if (n >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "Request body truncated (%d >= %d), userContext too long", n, (int)sizeof(body));
        return {false, "背景信息过长"};
    }

    esp_http_client_config_t cfg = {};
    cfg.url = DEEPSEEK_API_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return {false, "HTTP客户端初始化失败"};

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");
    std::string auth = "Bearer " + apiKey;
    esp_http_client_set_header(client, "Authorization", auth.c_str());

    std::string response;
    DeepseekResult result = {false, "API请求失败"};

    esp_err_t err = esp_http_client_open(client, (int)strlen(body));
    ESP_LOGI(TAG, "Request body: %s", body);
    if (err == ESP_OK) {
        esp_http_client_write(client, body, (int)strlen(body));
        int content_length = esp_http_client_fetch_headers(client);
        (void)content_length;
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[512];
            int len;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                response += buf;
                if (response.size() > 4096) break;
            }

            // Parse JSON: find "content":"... in choices[0].message.content
            auto contentKey = response.find("\"content\":\"");
            if (contentKey != std::string::npos) {
                contentKey += 11; // skip past "content":"
                std::string content;
                bool escaped = false;
                for (auto i = contentKey; i < response.size(); i++) {
                    char c = response[i];
                    if (escaped) {
                        // Handle JSON escape sequences
                        switch (c) {
                            case '"': content += '"'; break;
                            case '\\': content += '\\'; break;
                            case '/': content += '/'; break;
                            case 'b': content += '\b'; break;
                            case 'f': content += '\f'; break;
                            case 'n': content += '\n'; break;
                            case 'r': content += '\r'; break;
                            case 't': content += '\t'; break;
                            case 'u': {
                                // Unicode escape \uXXXX - simplified: just skip 4 hex digits
                                // For now, append a placeholder (proper handling would need UTF-8 encoding)
                                if (i + 4 < response.size()) {
                                    i += 4; // Skip XXXX
                                    content += '?'; // Placeholder for unicode
                                }
                                break;
                            }
                            default:
                                content += c; // Unknown escape, keep as-is
                        }
                        escaped = false;
                    } else if (c == '\\') {
                        escaped = true;
                    } else if (c == '"') {
                        break;
                    } else {
                        content += c;
                    }
                }
                if (!content.empty()) {
                    result.success = true;
                    result.content = content;
                }
            }
        } else {
            // Read error response body for debugging
            char buf[512];
            int len;
            std::string errorResponse;
            while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
                buf[len] = 0;
                errorResponse += buf;
                if (errorResponse.size() > 1024) break;
            }
            ESP_LOGW(TAG, "API returned status %d, response: %s", status, errorResponse.c_str());
            result.content = "API返回错误";
        }
    } else {
        ESP_LOGW(TAG, "HTTP request failed: %d", err);
        result.content = "网络请求失败";
    }

    esp_http_client_cleanup(client);
    return result;
}
