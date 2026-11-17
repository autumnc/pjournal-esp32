#include "weather_client.h"

#include "json_parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "Weather";
static const char *WEATHER_API_URL = "https://uapis.cn/api/v1/misc/weather?city=";

static std::string urlEncode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// 天气文本会直接插进编辑器的一行里：去掉换行/制表符，并限制长度，
// 防止异常响应破坏正文的行结构或把超长文本灌进去。
static std::string sanitizeWeatherText(const std::string &in) {
    std::string out;
    out.reserve(std::min<size_t>(in.size(), 96));
    for (size_t i = 0; i < in.size();) {
        unsigned char c = (unsigned char)in[i];
        size_t len = (c < 0x80) ? 1
                   : ((c >> 5) == 0x6) ? 2
                   : ((c >> 4) == 0xE) ? 3
                   : ((c >> 3) == 0x1E) ? 4 : 1;
        if (i + len > in.size()) len = 1;   // 截断的 UTF-8 序列按单字节处理
        if (out.size() + len > 96) break;
        if (len == 1 && (c == '\r' || c == '\n' || c == '\t')) {
            out += ' ';
        } else {
            out.append(in, i, len);
        }
        i += len;
    }
    // 上面的多字节分支可能原样带进畸形序列里的控制字符，再兜底清一遍。
    for (char &ch : out) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    size_t b = 0, e = out.size();
    while (b < e && isspace((unsigned char)out[b])) b++;
    while (e > b && isspace((unsigned char)out[e - 1])) e--;
    return out.substr(b, e - b);
}

WeatherResult weatherFetchCurrent(const std::string &city) {
    WeatherResult result;
    size_t begin = 0, end = city.size();
    while (begin < end && isspace((unsigned char)city[begin])) begin++;
    while (end > begin && isspace((unsigned char)city[end - 1])) end--;
    if (begin == end) {
        result.error = "请先设置天气地区";
        return result;
    }
    std::string queryCity = city.substr(begin, end - begin);

    std::string url = std::string(WEATHER_API_URL) + urlEncode(queryCity);

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        result.error = "网络初始化失败";
        return result;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "pjournal-esp32/1.0");

    std::string response;
    int status = 0;
    bool timedOut = false;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char buf[256];
            int64_t deadline = esp_timer_get_time() + 8000000;
            while (true) {
                if (esp_timer_get_time() > deadline) {
                    timedOut = true;
                    break;
                }
                int len = esp_http_client_read(client, buf, sizeof(buf) - 1);
                if (len > 0) {
                    buf[len] = 0;
                    response += buf;
                    if (response.size() > 4096) break;  // 正常响应仅几百字节
                } else if (len == -ESP_ERR_HTTP_EAGAIN) {
                    // 非阻塞返回时不要自旋占满 CPU，后台任务也要让出时间片。
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                } else {
                    break;
                }
            }
        }
    }
    esp_http_client_cleanup(client);

    if (timedOut) {
        result.error = "天气请求超时";
        return result;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP request failed: %d", err);
        result.error = "网络请求失败";
        return result;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "Weather API status %d", status);
        result.error = "天气服务不可用";
        return result;
    }

    JsonValue root = JsonValue::parse(response);
    if (!root.isObject() || !root["weather"].isString()) {
        ESP_LOGW(TAG, "Unexpected response: %.256s", response.c_str());
        result.error = "天气数据解析失败";
        return result;
    }

    std::string text = sanitizeWeatherText(root["weather"].asString());
    if (text.empty()) {
        result.error = "天气数据为空";
        return result;
    }

    char temp[24] = "";
    if (root["temperature"].isNumber()) {
        double t = root["temperature"].asNumber();
        if (std::isfinite(t)) {
            if (std::fabs(t - std::round(t)) < 0.05)
                snprintf(temp, sizeof(temp), "%d℃", (int)std::lround(t));
            else
                snprintf(temp, sizeof(temp), "%.1f℃", t);
        }
    }

    result.summary = text;
    if (temp[0]) {
        result.summary += ' ';
        result.summary += temp;
    }
    result.success = true;
    return result;
}
