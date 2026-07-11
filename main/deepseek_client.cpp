#include "deepseek_client.h"
#include "settings_manager.h"
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <esp_http_client.h>

static const char *TAG = "Deepseek";
DeepseekClient g_deepseek;

#define DEEPSEEK_API_URL "https://api.deepseek.com/chat/completions"

DeepseekResult DeepseekClient::generatePrompt(const std::string &userContext) {
    std::string apiKey = g_settings.deepseekKey();
    if (apiKey.empty()) {
        return {false, "请先在设置中配置Deepseek Key"};
    }

    // Build request body
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "{\"model\":\"deepseek-chat\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是一个日记写作助手。根据用户的背景信息，"
        "生成一个简短的日记写作提示（不超过42字），引导用户记录今天的生活。\"},"
        "{\"role\":\"user\",\"content\":\"我的背景：%s。请生成一个写作提示。\"}],"
        "\"max_tokens\":100,\"temperature\":0.7}",
        userContext.c_str());
    if (n >= (int)sizeof(body)) {
        ESP_LOGW(TAG, "Request body truncated (%d >= %d)", n, (int)sizeof(body));
    }

    esp_http_client_config_t cfg = {};
    cfg.url = DEEPSEEK_API_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 30000;
    cfg.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return {false, "HTTP客户端初始化失败"};

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");
    std::string auth = "Bearer " + apiKey;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    std::string response;
    DeepseekResult result = {false, "API请求失败"};

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
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
                        content += c;
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
            ESP_LOGW(TAG, "API returned status %d", status);
            result.content = "API返回错误";
        }
    } else {
        ESP_LOGW(TAG, "HTTP request failed: %d", err);
        result.content = "网络请求失败";
    }

    esp_http_client_cleanup(client);
    return result;
}
