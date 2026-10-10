#pragma once

#include <cstdint>
#include <cstring>
#include <esp_err.h>
#include <esp_bt_defs.h>
#include <esp_gap_ble_api.h>

#define MAX_BT_DEVICES 10

// Key repeat timing
#define KEY_REPEAT_DELAY_MS    500  // Initial delay before first repeat
#define KEY_REPEAT_INTERVAL_MS 50   // Interval between repeats

struct BtDeviceInfo {
    esp_bd_addr_t bda;
    esp_ble_addr_type_t addr_type;
    char name[32];
    int rssi;
};

struct BtPairedDevice {
    esp_bd_addr_t bda;
    esp_ble_addr_type_t addr_type;
    char name[32];
};

class BtKeyboard {
public:
    BtKeyboard() = default;
    static BtKeyboard& getInstance();

    esp_err_t init();
    void deinit();

    // Scan for BLE HID keyboards (non-blocking, starts background task)
    void scanDevices();

    // Access scan results (valid after scan completes)
    int deviceCount();
    const BtDeviceInfo* getDevice(int idx);
    void clearDevices();

    // Connect to a discovered device by index
    esp_err_t connectDevice(int idx);
    void disconnect();

    // Persistent pairing: save/load devices on SD card for auto-reconnect
    void savePairedDevice(const uint8_t *bda, esp_ble_addr_type_t addr_type, const char *name);
    void loadPairedDevices();
    bool removePairedDevice(const uint8_t *bda);
    int pairedDeviceCount();
    const BtPairedDevice* getPairedDevice(int idx);
    int connectedPairedIndex();  // paired-list index of connected device, -1 if none
    esp_err_t connectBDA(const uint8_t *bda, esp_ble_addr_type_t addr_type);

    // Keyboard input
    uint8_t readKey();  // 非阻塞取一个按键, 无键返回 0
    // 阻塞取一个按键: 有键立即返回 true, 否则最多等待 timeout_ms 后返回 false。
    // 主循环用它做空闲等待, 既保留原来的空转节拍, 又让按键立刻唤醒而不白等一个周期。
    bool waitKey(uint8_t &out, uint32_t timeout_ms);
    void flushKeys();
    void checkKeyRepeat();  // Check for key repeat events

    // Status
    bool isConnected() const;
    bool isInitialized() const;  // true after init() completes (async boot)
    bool isScanning();
    bool isConnecting() const;  // 新增：检查是否正在连接
    void setConnected(bool c);
    int keyboardBatteryPct();  // 键盘电池电量 %，-1=未知/未连接

private:
    bool connected_ = false;
};

extern BtKeyboard g_bt;
