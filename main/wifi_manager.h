#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <esp_err.h>

// 扫描到的一个 AP。
struct WifiScanEntry {
    std::string ssid;
    int8_t rssi = 0;
    bool open = false;  // 开放网络(不需要密码)
};

class WifiManager {
public:
    bool begin();
    // openNetwork=true 时把最低加密门限放到 OPEN, 否则开放网络会被
    // WPA2 门限过滤掉(扫描列表里能选到却永远连不上)。
    bool connect(const char *ssid, const char *password, bool openNetwork = false);
    // 扫描附近 AP。阻塞(最长约 4 秒), 只在后台任务里调用。
    // 需要时临时打开射频; 若射频是本次才打开且最终未连接, 扫描后关掉。
    bool scanNetworks(std::vector<WifiScanEntry> &out);
    bool isConnected();
    void disconnect();
    std::string getIp();

private:
    bool _inited = false;
    bool _started = false;  // wifi radio started (station active)
};

extern WifiManager g_wifi;
