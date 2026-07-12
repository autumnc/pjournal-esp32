#include "wifi_manager.h"
#include <cstring>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>

static const char *TAG = "WiFi";
WifiManager g_wifi;

static bool s_connected = false;
static bool s_auto_reconnect = true;
static EventGroupHandle_t s_wifi_event = NULL;
const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_auto_reconnect) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        if (s_wifi_event) xEventGroupSetBits(s_wifi_event, WIFI_CONNECTED_BIT);
    }
}

bool WifiManager::begin() {
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
    esp_wifi_start();

    s_wifi_event = xEventGroupCreate();
    _inited = true;
    return true;
}

bool WifiManager::connect(const char *ssid, const char *password) {
    if (!ssid || !*ssid) return false;
    s_auto_reconnect = true;

    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
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
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    s_connected = false;
}

std::string WifiManager::getIp() {
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        return buf;
    }
    return "";
}
