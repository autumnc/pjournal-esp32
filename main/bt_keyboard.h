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

    // Persistent pairing: save/load device on SD card for auto-reconnect
    void savePairedDevice(const uint8_t *bda, esp_ble_addr_type_t addr_type, const char *name);
    bool loadPairedDevice(uint8_t *bda, esp_ble_addr_type_t &addr_type);
    void clearPairedDevice();
    esp_err_t connectBDA(const uint8_t *bda, esp_ble_addr_type_t addr_type);

    // Keyboard input
    uint8_t readKey();
    void flushKeys();
    void checkKeyRepeat();  // Check for key repeat events

    // Status
    bool isConnected() const;
    bool isScanning();
    bool isConnecting() const;  // 新增：检查是否正在连接
    void setConnected(bool c);

private:
    bool connected_ = false;
};

extern BtKeyboard g_bt;
