#include "wifi_manager.h"
#include <cstring>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *TAG = "WiFi";
WifiManager g_wifi;

// 现在有后台任务也会用 WiFi（Flomo 同步互 Esc 退出后 worker 还在跑），主任务
// 可能同时在设置里测网、开 Web 文件管理或做 WebDAV —— 两条任务同时 begin()/
// connect() 会把 esp_netif/事件处理器初始化两遍。所有改 WiFi 状态的入口串起来。
static SemaphoreHandle_t s_wifiMux = xSemaphoreCreateMutex();
struct WifiLock {
    WifiLock()  { if (s_wifiMux) xSemaphoreTake(s_wifiMux, portMAX_DELAY); }
    ~WifiLock() { if (s_wifiMux) xSemaphoreGive(s_wifiMux); }
};

static bool s_connected = false;
static bool s_auto_reconnect = false;
static EventGroupHandle_t s_wifi_event = NULL;
const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // STA_START also fires during connect()'s esp_wifi_start(), but the new
        // config isn't applied yet — auto-connecting here would use the stale
        // flash config and block the subsequent set_config. Only reconnect after
        // a real disconnect (s_auto_reconnect is turned on post-set_config).
        if (s_auto_reconnect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool auto_reconnect = s_auto_reconnect;  // Read before clearing state
        s_connected = false;
        if (auto_reconnect) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        if (s_wifi_event) xEventGroupSetBits(s_wifi_event, WIFI_CONNECTED_BIT);
    }
}

bool WifiManager::begin() {
    WifiLock lock;
    if (_inited) return true;
    esp_netif_init();
    // 事件循环可能已创建，忽略已存在错误
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Event loop create failed: %d", ret);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() == ESP_OK) _started = true;

    s_wifi_event = xEventGroupCreate();
    _inited = true;
    return true;
}

bool WifiManager::connect(const char *ssid, const char *password) {
    WifiLock lock;
    if (!ssid || !*ssid) return false;
    // 先抑制自动重连：esp_wifi_start() 会触发 STA_START，若此处 auto_reconnect
    // 已为 true，处理器会用旧配置抢先连接，导致下方的 set_config 报
    // "sta is connecting, cannot set config"，新 SSID 从未生效
    s_auto_reconnect = false;

    // Radio may have been stopped by a previous disconnect() — bring it back up
    if (!_started) {
        if (esp_wifi_start() != ESP_OK) return false;
        _started = true;
    }

    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    // 新配置已生效，之后掉线才允许自动重连
    s_auto_reconnect = true;
    esp_wifi_connect();

    // Wait for connection (10s timeout)
    if (s_wifi_event) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event, WIFI_CONNECTED_BIT,
                                                pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
        return (bits & WIFI_CONNECTED_BIT) != 0;
    }
    return false;
}

bool WifiManager::isConnected() { return s_connected; }

void WifiManager::disconnect() {
    WifiLock lock;
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    s_connected = false;
    // Fully power down the radio; WiFi is only needed on demand
    if (esp_wifi_stop() == ESP_OK) _started = false;
}

std::string WifiManager::getIp() {
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        return buf;
    }
    return "";
}
