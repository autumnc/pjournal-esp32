#include "bt_keyboard.h"
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/idf_additions.h>  // xTaskCreateWithCaps: bt_conn 栈放 PSRAM
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include "io_probe.h"
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
#define KEY_REDO       0xA4
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
static int64_t s_connect_started_us = 0;  // 本次连接尝试的起始时刻(仅在 s_connecting 为真时有意义)
static bool s_deiniting = false;   // deinit 进行中:阻止重连逻辑再发起新连接尝试
static bool s_shift_tap_armed = false;  // 左Shift 单击检测武装标记
static int s_kb_battery = -1;           // 键盘电池电量 %，-1=未知/未连接
static uint8_t s_last_keys[MAX_KEYS] = {0};
static uint8_t s_last_mod = 0;
static int64_t s_key_press_time[MAX_KEYS] = {0};
static int64_t s_last_repeat_time[MAX_KEYS] = {0};
static esp_ble_addr_type_t s_paired_addr_type = BLE_ADDR_TYPE_RANDOM;

// 连接在后台任务里执行: esp_hidh_dev_open 是同步阻塞的(连接失败要等链路层
// 超时 ~30s),不能放在主循环里,否则 UI 会卡死。
//
// 这个任务**开机建一次, 之后永不删除**, 由 s_connect_signal 唤醒。原先的写法是
// 每次连接都 xTaskCreate(connect_task, "bt_conn", 4096, ...) 现建一个, 而这里的
// 4096 是"字"不是字节: IDF 在 xTaskCreatePinnedToCore 里乘 sizeof(StackType_t)
// (freertos_tasks_c_additions.h), 也就是一次性要 16KB **连续内部** RAM —— 动态创建的
// 任务, TCB 与栈都只能来自内部 RAM(heap_idf.c: pvPortMalloc 带 MALLOC_CAP_INTERNAL
// |MALLOC_CAP_8BIT, 不看 SPIRAM_USE_MALLOC)。开机时内部 RAM 最大连续块还有 86KB,
// 第一连接建得起来; 跑一阵子之后输入法索引把内部 RAM 吃掉, 最大连续块只剩 3584
// 字节, 于是断线之后每次重连都死在这一句上。实测日志: 键盘 461.9s 断线后, 32 分钟里
// "Auto-connecting..." / "BT auto-reconnect retry" 各 919 条, 后面一条 "Connecting
// to" 都没有 —— 正是"刚开机连得上, 挂久了就再也连不上"。
// 常驻任务的栈放 PSRAM(8MB 空闲), 内部 RAM 只留几百字节 TCB, 这条失败路径就不存在了。
static TaskHandle_t s_connect_task = nullptr;
static SemaphoreHandle_t s_connect_signal = nullptr;
static bool s_connect_worker_ready = false;
static const uint32_t CONNECT_TASK_STACK_WORDS = 4096;  // 同原值(=16KB), 只是改落在 PSRAM

// 单次连接尝试的最长寿命。正常路径不用它兜底: 失败时 esp_hidh_dev_open 会在
// ~30s 后返回 NULL 并自行复位 s_connecting。取 60s 是给"卡死"留的出口 —— 请求发出
// 后 worker 迟迟没有复位 s_connecting(Bluedroid 内部命令阻塞、OPEN 事件丢失)会让
// 主循环的 !isConnecting() 门永远打不开, 键盘再也不会自动重连(日志表现为只剩每 30s
// 一次的配对列表重载)。超时后主动放弃这次尝试, 让重试继续。
static const int64_t CONNECT_ATTEMPT_TIMEOUT_US = 60 * 1000000LL;
static struct {
    uint8_t bda[ESP_BD_ADDR_LEN];
    esp_ble_addr_type_t addr_type;
} s_connect_req;
// 用户在"正在连接"时点了断开: 这次尝试已经作废, OPEN 事件到了也要立刻关掉,
// 否则键盘会在用户按过断开之后自己连上(requestConnect 会清掉它, 只影响当次尝试)。
static bool s_cancel_connect = false;

// 扫描同样跑常驻 worker(理由见上面 bt_conn 那段): 原来每次扫描都
// xTaskCreate(scan_task, "bt_scan", 4096, ...) —— 又是 16KB 连续内部 RAM, 而且
// 返回值没检查、任务自己 vTaskDelete(NULL) 收尾。扫描是用户在蓝牙面板上手动点的,
// 偏偏常在内部 RAM 已经被用得差不多的时候点(tab 切来切去、输入法索引早装好了),
// 于是"点了扫描没反应, 一条日志都没有"。改成常驻 + PSRAM 栈, 扫描期间零堆分配。
static TaskHandle_t s_scan_task = nullptr;
static SemaphoreHandle_t s_scan_signal = nullptr;
static bool s_scan_worker_ready = false;
static const uint32_t SCAN_TASK_STACK_WORDS = 4096;  // 同原值(=16KB), 落在 PSRAM
// 每次 scanDevices() 请求 +1。worker 用它区分"这次扫描还是我那次": 连接请求中途把
// 扫描停了、用户紧接着又点了一次扫描时, 旧的一次收尾不能把新请求的 s_scanning 清掉。
static uint32_t s_scan_gen = 0;

// esp_hidh 内部符号(未在 esp_hidh.h 里公开, 但 bluedroid 构建下就是它):
// 用来在 deinit 时确认设备真的从列表里摘掉了 —— esp_hidh_deinit() 见到非空列表会
// 直接失败且什么都不释放, 那会让唤醒后的 esp_hidh_init 报 "Already initialized",
// BT 一直瘫到重启。哪天 IDF 把它藏起来了, 这里会是链接错误(而不是运行时静默出错)。
extern "C" esp_hidh_dev_t *esp_hidh_dev_get_by_bda(esp_bd_addr_t bda);

// 等某个 BDA 对应的设备从 esp_hidh 列表里消失(close 是异步的, 事件先到、释放后到)。
static void wait_hidh_dev_gone(const uint8_t *bda, uint32_t timeout_ms) {
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_hidh_dev_get_by_bda((uint8_t *)bda) != nullptr) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "device still in esp_hidh list after %u ms", (unsigned)timeout_ms);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

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
            if (s_cancel_connect) {
                // 用户在连接过程中点了断开: 当场关掉, 别让键盘"自己连上"
                s_cancel_connect = false;
                ESP_LOGI(TAG, "Connect attempt cancelled by user, closing device");
                esp_hidh_dev_close(param->open.dev);
                break;
            }
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
        // 只认当前设备的 close。迟到的旧设备 close 很常见: 关掉 A 立刻连 B 时,
        // A 的 close 事件会在 B 的 OPEN 之后才到, 拿它去抹状态就会把刚连上的 B 一起
        // 抹成"未连接"——主循环随即按掉线处理, 下一轮重连又去 close B, 于是键盘
        // 连上又掉、连上又掉。dev 指针在事件里不会提前失效: esp_hidh 先回调我们,
        // 再在 postprocess 里释放, 而且释放前不可能被新设备复用。
        if (param->close.dev != s_dev) {
            ESP_LOGI(TAG, "Ignoring close event for a device that is no longer current");
            break;
        }
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
                if (ctrl && shift && kc == 29) {
                    // Ctrl+Shift+Z → redo
                    uint8_t r = KEY_REDO;
                    xQueueSendToBack(s_queue, &r, 0);
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

// 常驻扫描 worker: 等 s_scan_signal, 一次扫描走完回到等待。永不返回(见 s_scan_task 的说明)。
static void scan_task(void *arg) {
    for (;;) {
        if (xSemaphoreTake(s_scan_signal, portMAX_DELAY) != pdTRUE) continue;
        if (s_deiniting) { s_scanning = false; continue; }
        const uint32_t gen = s_scan_gen;
        // 扫描请求在 3 秒预热期间可能被取消(拆栈/用户又点了一次), 那就别再开射频
        if (!s_scanning) continue;

        // 请求方(scanDevices)已经清空了列表, 这里只需要开始
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!s_scanning || s_scan_gen != gen) continue;  // 被取消, 或又来了新请求
        ESP_LOGI(TAG, "Scanning for BLE devices...");

        esp_err_t ret = esp_ble_gap_set_ext_scan_params(&s_ext_scan_params);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "set_ext_scan_params failed: %d", ret);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_scanning || s_scan_gen != gen) continue;
        ret = esp_ble_gap_start_ext_scan(SCAN_DURATION * 100, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "start_ext_scan failed: %d", ret);
        } else {
            ESP_LOGI(TAG, "Extended scan started for %d seconds", SCAN_DURATION);
        }

        // 等扫描窗口走完。连接请求会把 s_scanning 置 false(它要停扫描才能连),
        // 那就提前收尾, 别在这里空等 —— 但收尾只清 s_scanning: 这里绝不能再去动
        // 连接状态。原来这行是 setConnected(false), 而连接正是在扫描期间发起的,
        // 于是一次成功的连接会在 8 秒后被这里"宣布掉线", 主循环随即重连并 close 掉
        // 刚连上的键盘。
        for (int waited = 0; waited < (SCAN_DURATION + 3) * 1000; waited += 200) {
            if (!s_scanning || s_scan_gen != gen) break;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (s_scanning && s_scan_gen == gen) {
            ESP_LOGI(TAG, "Scan complete, found %d devices", s_found_count);
            s_scanning = false;
        }
    }
}

// 常驻连接 worker: 等 s_connect_signal, 醒来后读 s_connect_req 里最新的一次请求。
// esp_hidh_dev_open 同步阻塞: 成功时返回 dev,随后由异步的 ESP_HIDH_OPEN_EVENT 复位
// s_connecting;失败时返回 NULL 且不发任何事件, 必须在这里复位 s_connecting,否则
// 重连逻辑会永远被 isConnecting() 卡住。这个循环永不返回, 由 init() 建一次。
static void connect_task(void *arg) {
    for (;;) {
        if (xSemaphoreTake(s_connect_signal, portMAX_DELAY) != pdTRUE) continue;

        // 正在拆栈(s_deiniting)或已经连上了: 这次唤醒作废 —— s_connect_req 里可能
        // 还留着上一次的请求, 且 requestConnect 期间可能已被断开/重连事件超车。
        if (s_deiniting || s_connected) {
            s_connecting = false;
            s_connect_started_us = 0;
            continue;
        }

        uint8_t bda[ESP_BD_ADDR_LEN];
        esp_ble_addr_type_t addr_type = s_connect_req.addr_type;
        memcpy(bda, s_connect_req.bda, ESP_BD_ADDR_LEN);
        // 本次尝试的标记: esp_hidh_dev_open 会阻塞几十秒, 期间 isConnecting() 的 60s
        // 兜底可能把 s_connecting 放开、主循环又发来一次新请求(时间戳随之刷新)。
        // 那种情况下这次尝试已经作废, 失败返回时不能再动 s_connecting, 否则会把
        // 新尝试的门也一起放开, 主循环跟着再发一次 —— 两次 open 撞在一起。
        const int64_t attempt_started_us = s_connect_started_us;

        if (s_scanning) {
            s_scanning = false;
            esp_ble_gap_stop_ext_scan();
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        char hex[13];
        snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        ESP_LOGI(TAG, "Connecting to %s...", hex);

        esp_hidh_dev_t *dev = esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE, addr_type);
        if (dev == nullptr) {
            // 进休眠拆栈会把飞行中的这次 open 打断, 那是预期行为, 不是失败 —— 键盘
            // 睡着的时候重试一直在跑, 所以**每次休眠**都会撞上, 用错误级别打出来会
            // 把真正的失败埋掉。区分方法: 被拆栈打断的那种没有链路超时那条
            // "OPEN failed: 0x85"(status 为 0x0), 而且此时 s_deiniting 必定为真。
            if (s_deiniting) {
                ESP_LOGI(TAG, "Connect attempt to %s aborted by BT teardown (light sleep)", hex);
            } else {
                ESP_LOGE(TAG, "HID open failed for %s", hex);
            }
            if (s_connect_started_us == attempt_started_us) {
                s_connecting = false;
                s_connect_started_us = 0;
            }
        }
    }
}

// 记录连接请求并唤醒常驻 worker。s_connected/s_connecting/s_deiniting 门控避免重复发起,
// 连接中状态下后续请求直接跳过。这里不再有任何堆分配, 所以"请求发不出去"这条路径
// 不会再因为内部 RAM 碎片化而出现(见 connect_task 上方的说明)。
static void requestConnect(const uint8_t *bda, esp_ble_addr_type_t addr_type) {
    if (s_deiniting || s_connected || s_connecting) {
        ESP_LOGI(TAG, "Already connected or connecting, skip connect request");
        return;
    }
    memcpy(s_connect_req.bda, bda, ESP_BD_ADDR_LEN);
    s_connect_req.addr_type = addr_type;
    s_connect_started_us = esp_timer_get_time();
    s_connecting = true;
    s_cancel_connect = false;  // 新请求作废掉上一次的取消标记(它只对当次尝试有效)
    if (s_connect_signal == nullptr || xSemaphoreGive(s_connect_signal) != pdTRUE) {
        // 只有 worker 没建起来(或信号量已被占)才会走到这里。复位 s_connecting,
        // 别把重试门锁死; 下次 init() 还有机会把 worker 补上。
        s_connecting = false;
        s_connect_started_us = 0;
        ESP_LOGE(TAG, "bt_conn worker unavailable, giving up this attempt (internal largest=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        IO_PROBE_HEAP("bt_conn worker 不可用");
    }
}

BtKeyboard& BtKeyboard::getInstance() {
    static BtKeyboard inst;
    s_self = &inst;
    return inst;
}

esp_err_t BtKeyboard::init() {
    // 判据是 s_init_done, 不是 s_queue: 原来写的是 `if (s_queue) return ESP_OK`, 而
    // 下面任何一个失败分支都没有清理 s_queue —— 一旦 init 中途失败(比如内部 RAM 不够
    // 起 bluedroid), 下一次 init 会被这句直接骗过去返回"成功", 其实 BT 栈根本没起来,
    // 而且队列/互斥量已经泄漏。
    if (s_init_done) return ESP_OK;

    s_deiniting = false;
    s_cancel_connect = false;
    bool controller_inited = false;
    bool bluedroid_inited = false;
    bool hidh_inited = false;
    esp_err_t ret = ESP_FAIL;

    // 上一次失败留下的残渣(理论上不该有, 兜底)
    if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
    if (s_devices_mutex) { vSemaphoreDelete(s_devices_mutex); s_devices_mutex = nullptr; }

    s_queue = xQueueCreate(32, sizeof(uint8_t));
    if (!s_queue) return ESP_FAIL;

    s_devices_mutex = xSemaphoreCreateMutex();
    if (!s_devices_mutex) goto fail;

    // 这一整段套一层作用域: handle 类配置结构体(带默认成员初始化)都在这儿, C++
    // 不允许 `goto fail` 跳过它们的初始化 —— 包成块之后 goto 是"跳出块", 合法。
    {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) goto fail;
    controller_inited = true;
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) goto fail;

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret != ESP_OK) goto fail;
    bluedroid_inited = true;
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) goto fail;

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
    if (ret != ESP_OK) goto fail;
    hidh_inited = true;

    // Pre-configure extended scan params
    s_ext_scan_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    s_ext_scan_params.filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    s_ext_scan_params.scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE;
    s_ext_scan_params.cfg_mask = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK;
    s_ext_scan_params.uncoded_cfg.scan_type = BLE_SCAN_TYPE_ACTIVE;
    s_ext_scan_params.uncoded_cfg.scan_interval = 0x50;
    s_ext_scan_params.uncoded_cfg.scan_window = 0x30;

    // 常驻 worker 的信号量(连接/扫描)在 BT 栈起来之后建, 一次建好就不再删:
    // 它们跟 worker 一样跨休眠的生命周期。
    if (!s_connect_signal) s_connect_signal = xSemaphoreCreateBinary();
    if (!s_scan_signal) s_scan_signal = xSemaphoreCreateBinary();
    if (!s_connect_signal || !s_scan_signal) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    // 常驻 worker: 栈放 PSRAM(内部 RAM 只占几百字节 TCB), 只建一次 —— 休眠唤醒走的
    // 是 deinit/init, 而 worker 在 deinit 里不会被删(它们跨 BT 栈的生命周期), 所以
    // 这里用 *_worker_ready 守卫, 避免第二次 init 又建一个。建不起来也不致命:
    // 连接会打印 "bt_conn worker unavailable", 扫描会在 scanDevices 里被挡下并报错。
    if (!s_connect_worker_ready &&
        xTaskCreateWithCaps(connect_task, "bt_conn", CONNECT_TASK_STACK_WORDS, NULL, 3,
                            &s_connect_task, MALLOC_CAP_SPIRAM) == pdPASS) {
        s_connect_worker_ready = true;
    }
    if (!s_connect_worker_ready) {
        ESP_LOGE(TAG, "bt_conn worker create failed (internal largest=%u, psram largest=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        s_connect_task = nullptr;
    }
    if (!s_scan_worker_ready &&
        xTaskCreateWithCaps(scan_task, "bt_scan", SCAN_TASK_STACK_WORDS, NULL, 2,
                            &s_scan_task, MALLOC_CAP_SPIRAM) == pdPASS) {
        s_scan_worker_ready = true;
    }
    if (!s_scan_worker_ready) {
        ESP_LOGE(TAG, "bt_scan worker create failed (internal largest=%u, psram largest=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        s_scan_task = nullptr;
    }

    ESP_LOGI(TAG, "BT keyboard driver initialized");
    s_init_done = true;
    return ESP_OK;
    }  // 结束上面那段作用域

fail:
    // 反序拆掉已经建起来的部分。原来每个失败分支都是直接 return, 于是"半个 BT 栈"
    // 留在原地: 队列/互斥量泄漏, bluedroid/controller 也还开着 —— 下一次 init 要么被
    // s_queue 骗过去, 要么在 esp_bluedroid_init 上再报一次错, 状态越滚越乱。
    ESP_LOGE(TAG, "BT init failed (0x%x), unwinding", ret);
    if (hidh_inited) esp_hidh_deinit();
    if (bluedroid_inited) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
    }
    if (controller_inited) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
    }
    if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
    if (s_devices_mutex) { vSemaphoreDelete(s_devices_mutex); s_devices_mutex = nullptr; }
    s_init_done = false;
    s_deiniting = false;
    return ret;
}

void BtKeyboard::deinit() {
    s_deiniting = true;
    // 常驻 worker 不在这里删: 它在 BT 栈之上重入(唤醒后会重新 init), 删了就得
    // 在 init 里重建, 而重建要在内部 RAM 上分配 —— 正是这次要避开的路径。它的
    // 栈在 PSRAM, 空转只占一个信号量等待。丢掉的只是可能还挂着的那次请求信号:
    // 不清掉的话, 唤醒后(此时 s_deiniting 已复位)它会把陈旧的一次连接当成新请求。
    if (s_connect_signal) xSemaphoreTake(s_connect_signal, 0);
    if (s_scan_signal) xSemaphoreTake(s_scan_signal, 0);
    s_cancel_connect = false;
    // 扫描要用射频, 而且拆 bluedroid 时扫描任务还在跑会报一堆错 —— 先停掉它
    if (s_scanning) {
        s_scanning = false;
        esp_ble_gap_stop_ext_scan();
    }
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        // esp_hidh_deinit 要求设备列表为空, 否则直接返回 ESP_ERR_INVALID_STATE 且**什么
        // 都不释放** —— 事件循环和信号量留在原地, 唤醒后 esp_hidh_init 报
        // "Already initialized", BT 瘫到重启。而 close 是异步的, 所以这里按"设备真的
        // 从列表里消失了"来等(最多 1.5s), 不再像原来那样盲等 200ms 赌事件准时到。
        const uint8_t *bda = esp_hidh_dev_bda_get(s_dev);
        if (bda) {
            uint8_t closing_bda[ESP_BD_ADDR_LEN];
            memcpy(closing_bda, bda, sizeof(closing_bda));
            wait_hidh_dev_gone(closing_bda, 1500);
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    // 先关 bluedroid:若有连接尝试在飞行中(重试失败后立即重发,休眠时刻
    // 几乎必然撞上),它会在栈关闭过程中失败并自行从设备列表释放。等它退出后
    // 再 esp_hidh_deinit 才能走完清理;否则 esp_hidh_deinit 因列表非空提前返回,
    // 泄漏事件循环与信号量,唤醒后 esp_hidh_init 报 Already initialized 失败,
    // BT 瘫到下一次完整 deinit(表现为自动休眠唤醒后键盘永远连不上)。
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    int waited = 0;
    while (s_connecting && waited < 3000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    s_connecting = false;
    // 返回值必须看: 失败意味着 hidh 没拆干净, 下一次 init 会直接失败 —— 与其让
    // 它在几百行外以 "Already initialized" 的面目出现, 不如在这里说清楚。
    esp_err_t hidh_ret = esp_hidh_deinit();
    if (hidh_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_deinit failed: 0x%x — BT 可能要重启才能恢复", hidh_ret);
    }
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    // 兜底: 事件没来(或上面 deinit 失败)也不能把状态留成"已连接" —— 唤醒后主循环
    // 会因此以为键盘还在, 从此不再发起任何重连, 比掉线更难查。
    s_dev = nullptr;
    s_connected = false;
    s_connecting = false;
    s_connect_started_us = 0;
    if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
    if (s_devices_mutex) { vSemaphoreDelete(s_devices_mutex); s_devices_mutex = nullptr; }
    s_init_done = false;
}

void BtKeyboard::scanDevices() {
    if (s_deiniting || !s_init_done) return;
    if (s_scanning) {
        ESP_LOGI(TAG, "Scan already in progress, ignoring request");
        return;
    }
    if (s_dev) {
        // 扫描前先断开当前连接(要腾出射频)。不在这里置空 s_dev / 调
        // esp_hidh_dev_free: 前者依赖 close 事件来清(见 hidh_cb 的 CLOSE 分支),
        // 后者是个空实现(esp_hidh 自己会在 close 的 postprocess 里释放设备)。
        esp_hidh_dev_close(s_dev);
    }
    s_connected = false;
    connected_ = false;
    s_kb_battery = -1;  // 键盘电量失效
    memset(s_last_keys, 0, MAX_KEYS);
    // Clear device list
    if (s_devices_mutex) xSemaphoreTake(s_devices_mutex, portMAX_DELAY);
    s_found_count = 0;
    if (s_devices_mutex) xSemaphoreGive(s_devices_mutex);

    if (!s_scan_worker_ready || s_scan_signal == nullptr) {
        ESP_LOGE(TAG, "bt_scan worker unavailable, scan not started (internal largest=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        IO_PROBE_HEAP("bt_scan worker 不可用");
        return;
    }
    // 先把旗立好再叫 worker(worker 醒来第一件事就是看这面旗), UI 则靠 isScanning()
    // 判断扫描何时结束。换代次是为了让"上一次扫描的收尾"认得自己不是最新那次。
    s_scan_gen++;
    s_scanning = true;
    if (xSemaphoreGive(s_scan_signal) != pdTRUE) {
        s_scanning = false;
        ESP_LOGE(TAG, "scan signal give failed, scan not started");
        return;
    }
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
    if (!s_connecting) return false;
    // 自愈: 连接尝试有可能永远不结束(任务创建失败、Bluedroid 命令阻塞、OPEN 事件
    // 丢失)。超时即视为本次尝试失败, 主动放行 —— 否则主循环每次都被
    // !isConnecting() 挡回去, 键盘再也不会自动重连, 而日志上完全看不出原因。
    if (s_connect_started_us != 0 &&
        esp_timer_get_time() - s_connect_started_us > CONNECT_ATTEMPT_TIMEOUT_US) {
        ESP_LOGW(TAG, "Connect attempt stuck for %lld ms, abandoning it",
                 (long long)((esp_timer_get_time() - s_connect_started_us) / 1000));
        s_connecting = false;
        s_connect_started_us = 0;
        return false;
    }
    return true;
}

esp_err_t BtKeyboard::connectDevice(int idx) {
    if (idx < 0 || idx >= s_found_count) return ESP_ERR_INVALID_ARG;
    // 关掉旧连接但**不动 s_dev**: 它由 close 事件来清(见 hidh_cb 的 CLOSE 分支),
    // 这样"旧设备的 close"和"新设备已经连上"才分得开。原来在这里立刻置空, 于是旧
    // 设备的 close 事件一到就把新连接也当成断开处理了。
    if (s_dev) esp_hidh_dev_close(s_dev);
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
    // 正在连接时点断开: 记住这次尝试已经作废。光把 s_connecting 置 false 不够 ——
    // 连接还在飞, 它成功了照样会发 OPEN 事件, 键盘就"自己连上"了(见 OPEN 分支)。
    if (s_connecting) s_cancel_connect = true;
    s_connecting = false;
    // s_dev 交给 close 事件清(理由见 connectDevice)
    if (s_dev) esp_hidh_dev_close(s_dev);
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

bool BtKeyboard::waitKey(uint8_t &out, uint32_t timeout_ms) {
    if (!s_queue) return false;
    uint8_t c = 0;
    if (xQueueReceive(s_queue, &c, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    out = c;
    return true;
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
                } else if (ctrl && (s_last_mod & 0x22) && kc == 29) {
                    // Ctrl+Shift+Z → redo; no key repeat
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
        // 文件可能被掉电/拔卡写坏, 别把垃圾值当地址类型喂给 BLE 栈
        if (at < BLE_ADDR_TYPE_PUBLIC || at > BLE_ADDR_TYPE_RPA_RANDOM) at = BLE_ADDR_TYPE_PUBLIC;
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
    if (s_deiniting) return ESP_FAIL;   // 正在拆栈(进休眠), 别再发起连接

    // s_dev 交给 close 事件清(理由见 connectDevice)
    if (s_dev) esp_hidh_dev_close(s_dev);
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
