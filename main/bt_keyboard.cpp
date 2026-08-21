#include "bt_keyboard.h"
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_hidh.h>
#include <esp_hid_common.h>

static const char *TAG = "BtKeybrd";

#define HID_REPORT_LEN  8
#define MAX_KEYS        6
#define SCAN_DURATION   5

// Special key codes returned for non-ASCII keys
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_IME_TOGGLE 0x84
#define KEY_CTRL_ENTER 0x85
#define KEY_SHIFT_UP    0x86
#define KEY_SHIFT_DOWN  0x87
#define KEY_SHIFT_LEFT  0x88
#define KEY_SHIFT_RIGHT 0x89
#define KEY_CTRL_I      0x8A
#define KEY_FULLWIDTH_TOGGLE 0x8B
#define KEY_TRAD_TOGGLE 0x8C
#define KEY_LSHIFT_TAP 0x8D
#define KEY_HOME       0x8E
#define KEY_END        0x8F
#define KEY_PAGE_UP    0xA0
#define KEY_PAGE_DOWN  0xA1
#define KEY_SEARCH     0xA2
#define KEY_HELP       0xA3
// Ctrl+0-9 → 快捷编辑文件切换 (0x90-0x99)
#define KEY_FILE_BASE 0x90

// HID Usage ID → ASCII
static const uint8_t s_asc_low[] = {
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    0x0a,0x1b,0x08,0x09,0x20,
    '-','=','[',']','\\',
    '#',';','\'','`',',','.','/',
};
static const uint8_t s_asc_shift[] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    0x0a,0x1b,0x08,0x09,0x20,
    '_','+','{','}','|',
    '~',':','"','~','<','>','?',
};

static uint8_t hid_to_ascii(uint8_t kc, uint8_t mod) {
    if (kc == 82) return (mod & 0x22) ? KEY_SHIFT_UP : KEY_UP;
    if (kc == 81) return (mod & 0x22) ? KEY_SHIFT_DOWN : KEY_DOWN;
    if (kc == 80) return (mod & 0x22) ? KEY_SHIFT_LEFT : KEY_LEFT;
    if (kc == 79) return (mod & 0x22) ? KEY_SHIFT_RIGHT : KEY_RIGHT;
    if (kc == 74) return KEY_HOME;       // Home
    if (kc == 75) return KEY_PAGE_UP;    // PageUp
    if (kc == 77) return KEY_END;        // End
    if (kc == 78) return KEY_PAGE_DOWN;  // PageDown
    if (kc < 4 || kc > 103) return 0;
    uint8_t i = kc - 4;
    if (i >= sizeof(s_asc_low)) return 0;
    bool shift = (mod & 0x22) != 0;
    if (i <= 25) return shift ? ('A' + i) : ('a' + i);
    return shift ? s_asc_shift[i] : s_asc_low[i];
}

static BtKeyboard *s_self = nullptr;
BtKeyboard g_bt;
static QueueHandle_t s_queue = nullptr;
static esp_hidh_dev_t *s_dev = nullptr;

static bool s_connected = false;
static bool s_init_done = false;   // set once esp_hidh init completes
static bool s_scanning = false;
static bool s_connecting = false;  // 新增：标记正在连接中
static bool s_shift_tap_armed = false;  // 左Shift 单击检测武装标记
static int s_kb_battery = -1;           // 键盘电池电量 %，-1=未知/未连接
static uint8_t s_last_keys[MAX_KEYS] = {0};
static uint8_t s_last_mod = 0;
static int64_t s_key_press_time[MAX_KEYS] = {0};
static int64_t s_last_repeat_time[MAX_KEYS] = {0};
static esp_ble_addr_type_t s_paired_addr_type = BLE_ADDR_TYPE_RANDOM;

// 连接在后台任务里执行: esp_hidh_dev_open 是同步阻塞的(连接失败要等链路层
// 超时 ~30s),不能放在主循环里,否则 UI 会卡死。s_connect_task 非空表示任务存活。
static TaskHandle_t s_connect_task = nullptr;
static struct {
    uint8_t bda[ESP_BD_ADDR_LEN];
    esp_ble_addr_type_t addr_type;
} s_connect_req;

// Device list collected during scan
static BtDeviceInfo s_found_devices[MAX_BT_DEVICES];
static int s_found_count = 0;
static SemaphoreHandle_t s_devices_mutex = nullptr;

// Paired device list (persisted to /sdcard/settings/bt_paired, most recent first)
static BtPairedDevice s_paired[MAX_BT_DEVICES];
static int s_paired_count = 0;

// BLE scan params (extended)
static esp_ble_ext_scan_params_t s_ext_scan_params = {};

// Find device index by BDA, or -1 if not found
static int find_device(esp_bd_addr_t bda) {
    for (int i = 0; i < s_found_count; i++) {
        if (memcmp(s_found_devices[i].bda, bda, ESP_BD_ADDR_LEN) == 0)
            return i;
    }
    return -1;
}

// Manually parse AD data for device name (fallback)
static uint8_t* find_name_in_ad(uint8_t *data, uint8_t len, uint8_t *out_len) {
    *out_len = 0;
    uint8_t pos = 0;
    while (pos < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0) break;
        if (pos + field_len >= len) break;
        uint8_t type = data[pos + 1];
        if (type == ESP_BLE_AD_TYPE_NAME_CMPL || type == ESP_BLE_AD_TYPE_NAME_SHORT) {
            *out_len = field_len - 1;
            return &data[pos + 2];
        }
        pos += field_len + 1;
    }
    return nullptr;
}

// Add or update a device entry (caller must hold mutex)
static int add_or_update_device(esp_bd_addr_t bda, esp_ble_addr_type_t addr_type,
                                 uint8_t *name, uint8_t name_len, int rssi) {
    int idx = find_device(bda);
    if (idx >= 0) {
        // Update existing — only update name if we didn't have one before
        if (!s_found_devices[idx].name[0] && name && name_len > 0) {
            uint8_t copy_len = (name_len > 31) ? 31 : name_len;
            memcpy(s_found_devices[idx].name, name, copy_len);
            s_found_devices[idx].name[copy_len] = '\0';
            ESP_LOGI(TAG, "  -> [%d] updated name: %s", idx, s_found_devices[idx].name);
        }
        s_found_devices[idx].rssi = rssi;
        return idx;
    }
    if (s_found_count >= MAX_BT_DEVICES) return -1;
    idx = s_found_count;
    memcpy(s_found_devices[idx].bda, bda, ESP_BD_ADDR_LEN);
    s_found_devices[idx].addr_type = addr_type;
    s_found_devices[idx].rssi = rssi;
    if (name && name_len > 0) {
        uint8_t copy_len = (name_len > 31) ? 31 : name_len;
        memcpy(s_found_devices[idx].name, name, copy_len);
        s_found_devices[idx].name[copy_len] = '\0';
    } else {
        snprintf(s_found_devices[idx].name, sizeof(s_found_devices[idx].name),
                 "BLE-%02x%02x%02x", bda[3], bda[4], bda[5]);
    }
    s_found_count++;
    ESP_LOGI(TAG, "  -> [%d] %s (rssi=%d)", idx, s_found_devices[idx].name, rssi);
    return idx;
}

static void hidh_cb(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    auto event = (esp_hidh_event_t)id;
    auto *param = (esp_hidh_event_data_t *)event_data;
    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        s_connecting = false;  // 连接完成
        if (param->open.status == ESP_OK) {
            s_dev = param->open.dev;
            s_connected = true;
            if (s_self) {
                s_self->setConnected(true);
            }
            ESP_LOGI(TAG, "Keyboard connected: %s",
                     esp_hidh_dev_name_get(param->open.dev) ?: "?");
        } else {
            ESP_LOGE(TAG, "Keyboard HID open failed: %d", param->open.status);
        }
        // Save device info whenever we have a valid device handle,
        // so BLE-paired devices are persisted for auto-reconnect
        if (param->open.dev) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            if (bda && s_self) {
                const char *dev_name = esp_hidh_dev_name_get(param->open.dev);
                s_self->savePairedDevice(bda, s_paired_addr_type,
                                         dev_name ? dev_name : "?");
            }
        }
        break;
    case ESP_HIDH_CLOSE_EVENT:
        s_dev = nullptr;
        s_connected = false;
        s_connecting = false;  // 连接断开
        s_kb_battery = -1;     // 键盘电量失效
        memset(s_last_keys, 0, MAX_KEYS);
        memset(s_key_press_time, 0, MAX_KEYS * sizeof(int64_t));
        memset(s_last_repeat_time, 0, MAX_KEYS * sizeof(int64_t));
        s_shift_tap_armed = false;
        if (s_self) s_self->setConnected(false);
        ESP_LOGI(TAG, "Keyboard disconnected (rsn=0x%x)", param->close.reason);
        break;
    case ESP_HIDH_INPUT_EVENT: {
        if (!s_queue) break;
        uint8_t *data = param->input.data;
        size_t len = param->input.length;
        if (!data || len < 2) break;
        uint8_t mod = data[0];
        // 左Shift 单击检测: 按下时武装, 期间按下任何键则解除, 松开时若仍武装则触发
        bool lshiftNow = (mod & 0x02) != 0;
        bool lshiftWas = (s_last_mod & 0x02) != 0;
        if (lshiftNow && !lshiftWas) s_shift_tap_armed = true;
        const uint8_t *keys = (len >= HID_REPORT_LEN) ? (data + 2) : (data + 1);
        int nkeys = (len >= HID_REPORT_LEN) ? 6 : ((int)len - 1);
        if (nkeys > MAX_KEYS) nkeys = MAX_KEYS;

        // Track which keys are currently pressed for repeat logic
        bool current_pressed[MAX_KEYS] = {false};

        for (int i = 0; i < nkeys; i++) {
            uint8_t kc = keys[i];
            if (kc == 0) continue;

            // Check if this key was already pressed
            bool old = false;
            int slot = -1;
            for (int j = 0; j < MAX_KEYS; j++) {
                if (s_last_keys[j] == kc) {
                    old = true;
                    slot = j;
                    break;
                }
            }

            // Mark as currently pressed
            if (slot >= 0) current_pressed[slot] = true;

            // If new key press, record time and send event
            if (!old) {
                s_shift_tap_armed = false;  // 与Shift组合使用的按键按下, 取消单击
                // Find empty slot for this new key
                for (int j = 0; j < MAX_KEYS; j++) {
                    if (s_last_keys[j] == 0) {
                        s_last_keys[j] = kc;
                        s_key_press_time[j] = esp_timer_get_time();
                        current_pressed[j] = true;
                        break;
                    }
                }

                // Ctrl modifier handling
                bool ctrl = (mod & 0x11) != 0;
                bool shift = (mod & 0x22) != 0;
                if (ctrl && shift && kc == 9) {
                    // Ctrl+Shift+F → simplified/traditional toggle (before generic Ctrl+letter)
                    uint8_t tt = KEY_TRAD_TOGGLE;
                    xQueueSendToBack(s_queue, &tt, 0);
                    continue;
                }
                if (ctrl && kc == 12) {
                    // Ctrl+I → inspiration panel (must check before generic Ctrl+letter)
                    uint8_t ci = KEY_CTRL_I;
                    xQueueSendToBack(s_queue, &ci, 0);
                    continue;
                }
                if (ctrl && kc == 44) {
                    // Ctrl+Space → IME toggle
                    uint8_t toggle = KEY_IME_TOGGLE;
                    xQueueSendToBack(s_queue, &toggle, 0);
                    continue;
                }
                if (shift && kc == 44) {
                    // Shift+Space → fullwidth toggle
                    uint8_t fwt = KEY_FULLWIDTH_TOGGLE;
                    xQueueSendToBack(s_queue, &fwt, 0);
                    continue;
                }
                if (ctrl && kc == 40) {
                    // Ctrl+Enter → special key
                    uint8_t ce = KEY_CTRL_ENTER;
                    xQueueSendToBack(s_queue, &ce, 0);
                    continue;
                }
                if (ctrl && shift && kc == 56) {
                    // Ctrl+? (Shift+/) → 快捷键帮助对话框
                    uint8_t h = KEY_HELP;
                    xQueueSendToBack(s_queue, &h, 0);
                    continue;
                }
                if (ctrl && kc == 56) {
                    // Ctrl+/ → 搜索/替换对话框 (HID usage 56 = '/')
                    uint8_t s = KEY_SEARCH;
                    xQueueSendToBack(s_queue, &s, 0);
                    continue;
                }
                if (ctrl && kc >= 4 && kc <= 29) {
                    // Ctrl+letter → control character (0x01-0x1A)
                    uint8_t cc = kc - 3;
                    xQueueSendToBack(s_queue, &cc, 0);
                    continue;
                }
                if (ctrl && kc >= 30 && kc <= 39) {
                    // Ctrl+0-9 → 快捷编辑文件切换. HID usage: '1'=30 ... '9'=38, '0'=39
                    int fileIdx = (kc == 39) ? 0 : (kc - 29);
                    uint8_t fk = KEY_FILE_BASE + fileIdx;
                    xQueueSendToBack(s_queue, &fk, 0);
                    continue;
                }

                uint8_t ascii = hid_to_ascii(kc, mod);
                if (ascii) xQueueSendToBack(s_queue, &ascii, 0);
            }
        }

        // Clear keys that were released
        for (int i = 0; i < MAX_KEYS; i++) {
            if (s_last_keys[i] != 0 && !current_pressed[i]) {
                s_last_keys[i] = 0;
                s_key_press_time[i] = 0;
                s_last_repeat_time[i] = 0;
            }
        }

        if (!lshiftNow && lshiftWas) {
            if (s_shift_tap_armed) {
                uint8_t ev = KEY_LSHIFT_TAP;
                xQueueSendToBack(s_queue, &ev, 0);
            }
            s_shift_tap_armed = false;
        }

        s_last_mod = mod;
        break;
    }
    case ESP_HIDH_BATTERY_EVENT:
        s_kb_battery = param->battery.level;
        ESP_LOGI(TAG, "Keyboard battery: %d%%", s_kb_battery);
        break;
    default:
        break;
    }
}

// Check if advertising data contains the HID service UUID (0x1812)
static bool has_hid_service(uint8_t *data, uint8_t len) {
    uint8_t pos = 0;
    while (pos + 1 < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0) break;
        if (pos + 1 + field_len > len) break;
        uint8_t type = data[pos + 1];
        if (type == ESP_BLE_AD_TYPE_16SRV_CMPL || type == ESP_BLE_AD_TYPE_16SRV_PART) {
            for (uint8_t i = 0; i + 1 < field_len - 1; i += 2) {
                if (pos + 2 + i + 1 < len &&
                    data[pos + 2 + i] == 0x12 && data[pos + 2 + i + 1] == 0x18)
                    return true;
            }
        }
        pos += field_len + 1;
    }
    return false;
}

static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            // Skip non-HID devices
            if (!has_hid_service(param->scan_rst.ble_adv,
                param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len))
                break;
            uint8_t name_len = 0;
            uint8_t *name = esp_ble_resolve_adv_data_by_type(
                param->scan_rst.ble_adv,
                param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len,
                ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
            if (!name || name_len == 0) {
                name = esp_ble_resolve_adv_data_by_type(
                    param->scan_rst.ble_adv,
                    param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len,
                    ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
            }
            if (!name || name_len == 0) {
                name = find_name_in_ad(param->scan_rst.ble_adv,
                    param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len, &name_len);
            }
            if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
            add_or_update_device(param->scan_rst.bda, param->scan_rst.ble_addr_type,
                                 name, name_len, param->scan_rst.rssi);
            if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);
        }
        break;
    case ESP_GAP_BLE_EXT_ADV_REPORT_EVT: {
        auto &rpt = param->ext_adv_report.params;
        // Skip non-HID devices
        if (!has_hid_service(rpt.adv_data, rpt.adv_data_len))
            break;
        uint8_t name_len = 0;
        uint8_t *name = esp_ble_resolve_adv_data_by_type(
            rpt.adv_data, rpt.adv_data_len,
            ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
        if (!name || name_len == 0) {
            name = esp_ble_resolve_adv_data_by_type(
                rpt.adv_data, rpt.adv_data_len,
                ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
        }
        // Manual fallback if API fails with extended data
        if (!name || name_len == 0) {
            name = find_name_in_ad(rpt.adv_data, rpt.adv_data_len, &name_len);
        }
        if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
        add_or_update_device(rpt.addr, (esp_ble_addr_type_t)rpt.addr_type,
                             name, name_len, rpt.rssi);
        if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);
        break;
    }
    case ESP_GAP_BLE_SCAN_TIMEOUT_EVT:
        ESP_LOGI(TAG, "Scan timeout");
        break;
    case ESP_GAP_BLE_NC_REQ_EVT:
        ESP_LOGI(TAG, "BLE NC_REQ passkey: %06" PRIu32, param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "BLE pairing code: %06" PRIu32, param->ble_security.key_notif.passkey);
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "BLE SEC_REQ - responding");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        ESP_LOGI(TAG, "BLE PASSKEY_REQ");
        break;
    case ESP_GAP_BLE_KEY_EVT:
        ESP_LOGI(TAG, "BLE KEY type = %d", param->ble_security.ble_key.key_type);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "BLE auth success");
        } else {
            ESP_LOGE(TAG, "BLE auth fail: 0x%x", param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    default:
        break;
    }
}

static void scan_task(void *arg) {
    if (s_scanning) { vTaskDelete(NULL); return; }
    s_scanning = true;

    // Clear previous results
    if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    s_found_count = 0;
    if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Scanning for BLE devices...");

    esp_err_t ret;
    ret = esp_ble_gap_set_ext_scan_params(&s_ext_scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_ext_scan_params failed: %d", ret);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ret = esp_ble_gap_start_ext_scan(SCAN_DURATION * 100, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start_ext_scan failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "Extended scan started for %d seconds", SCAN_DURATION);
    }

    vTaskDelay(pdMS_TO_TICKS((SCAN_DURATION + 3) * 1000));
    ESP_LOGI(TAG, "Scan complete, found %d devices", s_found_count);
    s_scanning = false;

    if (s_self) {
        s_self->setConnected(false);
    }
    vTaskDelete(NULL);
}

// 后台执行实际连接。esp_hidh_dev_open 同步阻塞: 成功时返回 dev,随后由异步的
// ESP_HIDH_OPEN_EVENT 复位 s_connecting;失败时返回 NULL 且不发任何事件,
// 必须在这里复位 s_connecting,否则重连逻辑会永远被 isConnecting() 卡住。
static void connect_task(void *arg) {
    if (s_scanning) {
        s_scanning = false;
        esp_ble_gap_stop_ext_scan();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    char hex[13];
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x",
             s_connect_req.bda[0], s_connect_req.bda[1], s_connect_req.bda[2],
             s_connect_req.bda[3], s_connect_req.bda[4], s_connect_req.bda[5]);
    ESP_LOGI(TAG, "Connecting to %s...", hex);

    esp_hidh_dev_t *dev = esp_hidh_dev_open(s_connect_req.bda, ESP_HID_TRANSPORT_BLE,
                                            s_connect_req.addr_type);
    if (dev == nullptr) {
        s_connecting = false;
        ESP_LOGE(TAG, "HID open failed for %s", hex);
    }
    s_connect_task = nullptr;
    vTaskDelete(NULL);
}

// 记录连接请求并启动后台连接任务。s_connected/s_connecting 门控避免重复发起,
// 连接中状态下后续请求直接跳过。
static void requestConnect(const uint8_t *bda, esp_ble_addr_type_t addr_type) {
    if (s_connected || s_connecting) {
        ESP_LOGI(TAG, "Already connected or connecting, skip connect request");
        return;
    }
    memcpy(s_connect_req.bda, bda, ESP_BD_ADDR_LEN);
    s_connect_req.addr_type = addr_type;
    s_connecting = true;
    s_connect_task = nullptr;
    xTaskCreate(connect_task, "bt_conn", 4096, NULL, 3, &s_connect_task);
}

BtKeyboard& BtKeyboard::getInstance() {
    static BtKeyboard inst;
    s_self = &inst;
    return inst;
}

esp_err_t BtKeyboard::init() {
    if (s_queue) return ESP_OK;

    s_queue = xQueueCreate(32, sizeof(uint8_t));
    if (!s_queue) return ESP_FAIL;

    s_devices_mutex = xSemaphoreCreateMutex();
    if (!s_devices_mutex) return ESP_FAIL;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) return ret;
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) return ret;

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret != ESP_OK) return ret;
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) return ret;

    esp_ble_gap_register_callback(ble_gap_cb);

    // Configure SMP/security parameters for HID keyboard pairing
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_IO;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1);

    esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);

    esp_hidh_config_t hid_cfg = {
        .callback = hidh_cb,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ret = esp_hidh_init(&hid_cfg);
    if (ret != ESP_OK) return ret;

    // Pre-configure extended scan params
    s_ext_scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    s_ext_scan_params.filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    s_ext_scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE;
    s_ext_scan_params.cfg_mask = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK;
    s_ext_scan_params.uncoded_cfg.scan_type = BLE_SCAN_TYPE_ACTIVE;
    s_ext_scan_params.uncoded_cfg.scan_interval = 0x50;
    s_ext_scan_params.uncoded_cfg.scan_window = 0x30;

    ESP_LOGI(TAG, "BT keyboard driver initialized");
    s_init_done = true;
    return ESP_OK;
}

void BtKeyboard::deinit() {
    s_connecting = false;
    s_connect_task = nullptr;
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        // esp_hidh_deinit 要求所有设备已关闭,而 close 是异步的
        // (close 事件约 60-100ms 后到达)。不等它完成就 deinit 会返回错误,
        // 导致唤醒后 esp_hidh_init 无法重新初始化、键盘无法重连。
        vTaskDelay(pdMS_TO_TICKS(200));
        s_dev = nullptr;
    }
    esp_hidh_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
    if (s_devices_mutex) { vSemaphoreDelete(s_devices_mutex); s_devices_mutex = nullptr; }
    s_init_done = false;
}

void BtKeyboard::scanDevices() {
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        esp_hidh_dev_free(s_dev);
        s_dev = nullptr;
    }
    s_connected = false;
    connected_ = false;
    s_kb_battery = -1;  // 键盘电量失效
    memset(s_last_keys, 0, MAX_KEYS);
    // Clear device list
    if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    s_found_count = 0;
    if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);
    xTaskCreate(scan_task, "bt_scan", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "BT scan started for HID keyboards");
}

int BtKeyboard::deviceCount() {
    return s_found_count;
}

const BtDeviceInfo* BtKeyboard::getDevice(int idx) {
    if (idx < 0 || idx >= s_found_count) return nullptr;
    return &s_found_devices[idx];
}

void BtKeyboard::clearDevices() {
    if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    s_found_count = 0;
    if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);
}

bool BtKeyboard::isScanning() {
    return s_scanning;
}

bool BtKeyboard::isConnecting() const {
    return s_connecting;
}

esp_err_t BtKeyboard::connectDevice(int idx) {
    if (idx < 0 || idx >= s_found_count) return ESP_ERR_INVALID_ARG;
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        s_dev = nullptr;
    }
    s_connected = false;
    connected_ = false;
    s_kb_battery = -1;  // 键盘电量失效

    auto &d = s_found_devices[idx];
    ESP_LOGI(TAG, "Connecting to %s...", d.name);
    s_paired_addr_type = d.addr_type;

    // Save device info immediately so it's persisted even if HID channel
    // encounters issues after BLE pairing succeeds
    savePairedDevice(d.bda, d.addr_type, d.name);

    requestConnect(d.bda, d.addr_type);
    return ESP_OK;
}

void BtKeyboard::disconnect() {
    s_connecting = false;  // 取消连接中状态
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        s_dev = nullptr;
    }
    s_connected = false;
    connected_ = false;
    s_kb_battery = -1;  // 键盘电量失效
    memset(s_last_keys, 0, MAX_KEYS);
    if (s_queue) xQueueReset(s_queue);
}

int BtKeyboard::keyboardBatteryPct() {
    return s_kb_battery;
}

uint8_t BtKeyboard::readKey() {
    uint8_t c = 0;
    if (s_queue && xQueueReceive(s_queue, &c, 0) == pdTRUE) return c;
    return 0;
}

void BtKeyboard::flushKeys() {
    if (s_queue) xQueueReset(s_queue);
}

void BtKeyboard::checkKeyRepeat() {
    if (!s_queue || !s_connected) return;

    int64_t now = esp_timer_get_time();
    int64_t delay_us = KEY_REPEAT_DELAY_MS * 1000;
    int64_t interval_us = KEY_REPEAT_INTERVAL_MS * 1000;

    // Check each key slot for repeat
    for (int i = 0; i < MAX_KEYS; i++) {
        uint8_t kc = s_last_keys[i];
        if (kc == 0) continue;

        int64_t press_time = s_key_press_time[i];
        if (press_time == 0) continue;

        int64_t elapsed = now - press_time;

        // Check if key held long enough for repeat
        if (elapsed >= delay_us) {
            // Initialize last repeat time on first check
            if (s_last_repeat_time[i] == 0) {
                s_last_repeat_time[i] = press_time + delay_us;
            }

            // Send one repeat if we're past the next repeat time
            if (now >= s_last_repeat_time[i] + interval_us) {
                // Ctrl modifier handling for repeat
                bool ctrl = (s_last_mod & 0x11) != 0;
                if (ctrl && (s_last_mod & 0x22) && kc == 9) {
                    // Ctrl+Shift+F → trad toggle; consume repeat
                } else if (ctrl && kc == 12) {
                    uint8_t ci = KEY_CTRL_I;
                    xQueueSendToBack(s_queue, &ci, 0);
                } else if (ctrl && kc == 44) {
                    uint8_t toggle = KEY_IME_TOGGLE;
                    xQueueSendToBack(s_queue, &toggle, 0);
                } else if (ctrl && kc == 40) {
                    uint8_t ce = KEY_CTRL_ENTER;
                    xQueueSendToBack(s_queue, &ce, 0);
                } else if (ctrl && (s_last_mod & 0x22) && kc == 56) {
                    // Ctrl+? → 帮助; 按住不自动重复,避免误开关
                } else if (ctrl && kc == 56) {
                    // Ctrl+/ → 搜索对话框; 按住不自动重复,避免误开/误关
                } else if ((s_last_mod & 0x22) && kc == 44) {
                    // Shift+Space → fullwidth toggle (repeat)
                    uint8_t fwt = KEY_FULLWIDTH_TOGGLE;
                    xQueueSendToBack(s_queue, &fwt, 0);
                } else if (ctrl && kc >= 4 && kc <= 29) {
                    uint8_t cc = kc - 3;
                    xQueueSendToBack(s_queue, &cc, 0);
                } else if (ctrl && kc >= 30 && kc <= 39) {
                    // Ctrl+0-9 repeat → 文件切换码(编辑器对同文件 no-op)
                    int fileIdx = (kc == 39) ? 0 : (kc - 29);
                    uint8_t fk = KEY_FILE_BASE + fileIdx;
                    xQueueSendToBack(s_queue, &fk, 0);
                } else {
                    uint8_t ascii = hid_to_ascii(kc, s_last_mod);
                    if (ascii) xQueueSendToBack(s_queue, &ascii, 0);
                }

                // Update last repeat time
                s_last_repeat_time[i] = now;
            }
        }
    }
}

void BtKeyboard::savePairedDevice(const uint8_t *bda, esp_ble_addr_type_t addr_type, const char *name) {
    // Upsert: move to front (most recently used first), drop oldest if full
    int idx = -1;
    for (int i = 0; i < s_paired_count; i++) {
        if (memcmp(s_paired[i].bda, bda, ESP_BD_ADDR_LEN) == 0) { idx = i; break; }
    }
    if (idx >= 0) {
        for (int i = idx; i < s_paired_count - 1; i++) s_paired[i] = s_paired[i + 1];
        s_paired_count--;
    }
    if (s_paired_count >= MAX_BT_DEVICES) s_paired_count = MAX_BT_DEVICES - 1;
    for (int i = s_paired_count; i > 0; i--) s_paired[i] = s_paired[i - 1];
    s_paired_count++;
    memcpy(s_paired[0].bda, bda, ESP_BD_ADDR_LEN);
    s_paired[0].addr_type = addr_type;
    if (name && name[0]) {
        snprintf(s_paired[0].name, sizeof(s_paired[0].name), "%.31s", name);
    } else {
        snprintf(s_paired[0].name, sizeof(s_paired[0].name), "BLE-%02x%02x%02x", bda[3], bda[4], bda[5]);
    }
    mkdir("/sdcard/settings", 0777);
    FILE *f = fopen("/sdcard/settings/bt_paired", "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to save paired devices (no SD card?)");
        return;
    }
    for (int i = 0; i < s_paired_count; i++) {
        fprintf(f, "%02x%02x%02x%02x%02x%02x\n%d\n%s\n",
                s_paired[i].bda[0], s_paired[i].bda[1], s_paired[i].bda[2],
                s_paired[i].bda[3], s_paired[i].bda[4], s_paired[i].bda[5],
                (int)s_paired[i].addr_type, s_paired[i].name);
    }
    fclose(f);
    ESP_LOGI(TAG, "Saved %d paired device(s), first %s", s_paired_count, s_paired[0].name);
}

void BtKeyboard::loadPairedDevices() {
    s_paired_count = 0;
    FILE *f = fopen("/sdcard/settings/bt_paired", "r");
    if (!f) return;
    char line[64];
    while (s_paired_count < MAX_BT_DEVICES) {
        if (!fgets(line, sizeof(line), f)) break;
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strlen(line) != 12) continue;  // skip invalid line
        char hex[13];
        strncpy(hex, line, 12); hex[12] = '\0';
        int at = 0;
        if (!fgets(line, sizeof(line), f)) break;
        at = atoi(line);
        char name[32] = "";
        if (fgets(line, sizeof(line), f)) {
            nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            snprintf(name, sizeof(name), "%.31s", line);
        }
        for (int i = 0; i < 6; i++) {
            unsigned int byte;
            sscanf(hex + i * 2, "%02x", &byte);
            s_paired[s_paired_count].bda[i] = (uint8_t)byte;
        }
        s_paired[s_paired_count].addr_type = (esp_ble_addr_type_t)at;
        if (!name[0]) {
            snprintf(name, sizeof(name), "BLE-%02x%02x%02x",
                     s_paired[s_paired_count].bda[3],
                     s_paired[s_paired_count].bda[4],
                     s_paired[s_paired_count].bda[5]);
        }
        snprintf(s_paired[s_paired_count].name, sizeof(s_paired[s_paired_count].name), "%s", name);
        s_paired_count++;
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d paired device(s)", s_paired_count);
}

bool BtKeyboard::removePairedDevice(const uint8_t *bda) {
    for (int i = 0; i < s_paired_count; i++) {
        if (memcmp(s_paired[i].bda, bda, ESP_BD_ADDR_LEN) == 0) {
            for (int j = i; j < s_paired_count - 1; j++) s_paired[j] = s_paired[j + 1];
            s_paired_count--;
            FILE *f = fopen("/sdcard/settings/bt_paired", "w");
            if (!f) { ESP_LOGE(TAG, "Failed to save paired devices"); return true; }
            for (int k = 0; k < s_paired_count; k++) {
                fprintf(f, "%02x%02x%02x%02x%02x%02x\n%d\n%s\n",
                        s_paired[k].bda[0], s_paired[k].bda[1], s_paired[k].bda[2],
                        s_paired[k].bda[3], s_paired[k].bda[4], s_paired[k].bda[5],
                        (int)s_paired[k].addr_type, s_paired[k].name);
            }
            fclose(f);
            ESP_LOGI(TAG, "Removed paired device, %d remaining", s_paired_count);
            return true;
        }
    }
    return false;
}

int BtKeyboard::pairedDeviceCount() {
    return s_paired_count;
}

const BtPairedDevice* BtKeyboard::getPairedDevice(int idx) {
    if (idx < 0 || idx >= s_paired_count) return nullptr;
    return &s_paired[idx];
}

int BtKeyboard::connectedPairedIndex() {
    if (!s_connected || !s_dev) return -1;
    const uint8_t *bda = esp_hidh_dev_bda_get(s_dev);
    if (!bda) return -1;
    for (int i = 0; i < s_paired_count; i++) {
        if (memcmp(s_paired[i].bda, bda, ESP_BD_ADDR_LEN) == 0) return i;
    }
    return -1;
}

bool BtKeyboard::isConnected() const {
    return s_connected;
}

bool BtKeyboard::isInitialized() const {
    return s_init_done;
}

void BtKeyboard::setConnected(bool c) {
    s_connected = c;
    connected_ = c;
}

esp_err_t BtKeyboard::connectBDA(const uint8_t *bda, esp_ble_addr_type_t addr_type) {
    if (!s_init_done) return ESP_FAIL;  // BLE stack not initialized yet

    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        s_dev = nullptr;
    }
    s_connected = false;
    connected_ = false;

    s_paired_addr_type = addr_type;
    char hex[13];
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    ESP_LOGI(TAG, "Auto-connecting to saved device %s...", hex);
    requestConnect(bda, addr_type);
    return ESP_OK;
}
