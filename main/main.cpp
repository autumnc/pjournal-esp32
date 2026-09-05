#include "user_config.h"
#include "font_renderer.h"
#include "bt_keyboard.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "quick_edit.h"
#include "journal_storage.h"
#include "webdav_client.h"
#include "flomo_client.h"
#include "ime/IME.h"
#include "pjournal_app.h"
#include "screen_editor.h"
#include "screen_settings.h"
#include "screen_gtd.h"
#include "screen_outline.h"
#include "screen_bt_manage.h"
#include "screen_file_manager.h"
#include "screen_voice.h"
#include "screen_polish.h"
#include "screen_polish_prompt.h"
#include "voice_input.h"
#include "typing_click.h"
#include "u8g2_st7305.h"
#include "pcf85063.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <nvs_flash.h>
#include <esp_sntp.h>
#include <driver/gpio.h>
#include <sys/time.h>
#include <freertos/semphr.h>
#include <cstdio>

static const char *TAG = "Main";

// Display device (global, used by font_renderer.cpp)
static u8g2_st7305_t s_lcd_dev;
u8g2_t *g_u8g2 = nullptr;

static bool initDisplay() {
    ESP_LOGI(TAG, "Initializing display...");
    u8g2_st7305_config_t cfg = u8g2_st7305_default_config();
    cfg.mosi_io = RLCD_MOSI_PIN;
    cfg.sclk_io = RLCD_SCK_PIN;
    cfg.dc_io   = RLCD_DC_PIN;
    cfg.cs_io   = RLCD_CS_PIN;
    cfg.reset_io = RLCD_RST_PIN;
    cfg.rotation = U8G2_R1;
    cfg.tile_buf_height = U8G2_ST7305_TILE_BUF_FULL;

    esp_err_t ret = u8g2_st7305_init(&s_lcd_dev, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %d", ret);
        return false;
    }
    g_u8g2 = u8g2_st7305_get_u8g2(&s_lcd_dev);
    return true;
}

enum class AsyncUiState {
    Idle,
    Running,
    Done,
};

static volatile AsyncUiState s_webdavState = AsyncUiState::Idle;
static SyncResult s_webdavResult = {false, ""};
static int64_t s_webdavResultUntil = 0;

static volatile AsyncUiState s_flomoState = AsyncUiState::Idle;
static FlomoResult s_flomoResult = {false, ""};
static std::string s_flomoText;
static AppState s_flomoReturnTo = APP_EDITOR;
static int64_t s_flomoResultUntil = 0;
static SemaphoreHandle_t s_asyncResultMutex = nullptr;

static void ensureAsyncResultMutex() {
    if (!s_asyncResultMutex) s_asyncResultMutex = xSemaphoreCreateMutex();
}

static void lockAsyncResult() {
    ensureAsyncResultMutex();
    if (s_asyncResultMutex) xSemaphoreTake(s_asyncResultMutex, portMAX_DELAY);
}

static void unlockAsyncResult() {
    if (s_asyncResultMutex) xSemaphoreGive(s_asyncResultMutex);
}

static void drawCenteredBusy(const char *title, const char *line) {
    ui_clear();
    ui_draw_text_centered(100, title, false, true);
    ui_draw_text_centered(135, line);
    ui_commit();
}

static void webdavSyncTask(void *arg) {
    (void)arg;
    bool wifiWasConnected = g_wifi.isConnected();
    SyncResult result = {false, ""};

    std::string url = g_settings.webdavUrl();
    std::string user = g_settings.webdavUsername();
    std::string pass = g_settings.webdavPassword();
    if (url.empty() || user.empty()) {
        result = {false, "请先配置WebDAV"};
    } else if (!ensure_wifi_connected()) {
        result = {false, "WiFi连接失败"};
    } else {
        g_webdav.configure(url, user, pass);
        result = g_webdav.sync("/sdcard/pjournal");
    }

    restore_wifi_state(wifiWasConnected);
    lockAsyncResult();
    s_webdavResult = result;
    unlockAsyncResult();
    s_webdavState = AsyncUiState::Done;
    vTaskDelete(nullptr);
}

static void flomoSendTask(void *arg) {
    (void)arg;
    bool wifiWasConnected = g_wifi.isConnected();
    FlomoResult result = {false, ""};

    if (s_flomoText.empty()) {
        result = {false, "内容为空"};
    } else if (!ensure_wifi_connected()) {
        result = {false, "WiFi未连接"};
    } else {
        result = g_flomo.send(s_flomoText);
    }

    restore_wifi_state(wifiWasConnected);
    lockAsyncResult();
    s_flomoResult = result;
    unlockAsyncResult();
    s_flomoState = AsyncUiState::Done;
    vTaskDelete(nullptr);
}

// BLE stack init + auto-connect in a background task so the main UI
// renders immediately instead of waiting ~1s for the BT controller.
static void btInitTask(void *arg) {
    ESP_LOGI(TAG, "Starting Bluetooth...");
    if (g_bt.init() == ESP_OK) {
        g_bt.loadPairedDevices();
        if (g_bt.pairedDeviceCount() > 0) {
            ESP_LOGI(TAG, "Found %d saved keyboard(s), will auto-connect...",
                     g_bt.pairedDeviceCount());
            const BtPairedDevice *p = g_bt.getPairedDevice(0);
            g_bt.connectBDA(p->bda, p->addr_type);
        }
    } else {
        ESP_LOGE(TAG, "Bluetooth init failed");
    }
    vTaskDelete(NULL);
}

// ── Light sleep 空闲休眠 ────────────────────────────────────────────────
// 键盘/物理按键无输入 ≥10 分钟后进入 ESP light sleep(RAM 保留、BLE 射频关闭),
// 空闲电流从 ~2-5mA 降到 ~0.15mA。休眠期间 BLE 射频关闭,键盘无法唤醒,
// 只能按物理按键(GPIO18/GPIO0)唤醒;唤醒后后台重新 init BLE 并自动重连。
#define AUTO_SLEEP_TIMEOUT_US   (10 * 60 * 1000000LL)
#define AUTO_SLEEP_GRACE_US     (2 * 60 * 1000000LL)
#define SLEEP_WAKE_MASK         ((1ULL << PIN_USER_BTN) | (1ULL << PIN_BOOT))

// 最近一次用户输入(BLE 键或物理按键)的时间,0 表示启动后尚未记录
static int64_t s_last_activity_us = 0;
// BOOT 唤醒后的首次短按释放不应再次触发休眠(唤醒按键与休眠按键是同一个键)
static bool s_boot_wake_release_pending = false;

// 物理按键时间判定(不依赖主循环节拍,各界面循环速度不同)
#define BTN_DEBOUNCE_US       (30000)     // 30ms 防抖
#define BTN_LONG_PRESS_US     (1000000)   // 1s 判定长按
#define BTN_DOUBLE_WINDOW_US  (300000)    // 300ms 双击窗口

// 单击动作排队: 松开后等待双击窗口确认非双击再执行,避免双击第一下误发导航键
static struct { int key = 0; int64_t queued_us = 0; } s_pending_single;

static void enterLightSleep(void) {
    // 休眠提示画在底部状态栏位置、居中,不遮挡/清空上方画面——
    // 保留画面模式下,最后画面 + 底部提示在整个休眠期间持续显示(RLCD 零功耗)
    const char *hint = "休眠中 按键唤醒";
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, STATUS_Y, SCREEN_W);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, 0, STATUS_Y + 1, SCREEN_W, FONT_H + 3);
    u8g2_SetDrawColor(g_u8g2, 0);
    g_font.drawText((SCREEN_W - g_font.textWidth(hint)) / 2,
                    STATUS_Y + 1 + g_font.ascent(), hint, false);
    u8g2_SetDrawColor(g_u8g2, 1);
    ui_commit();

    // 休眠屏保策略:保留画面(开)时让显示相关 GPIO 在休眠期间保持原状态,
    // 防止 GPIO 隔离工作区把面板 RST 浮空导致复位清空 GRAM;白屏(关)则恢复隔离(默认)。
    const gpio_num_t display_pins[] = {RLCD_RST_PIN, RLCD_CS_PIN, RLCD_DC_PIN,
                                       RLCD_SCK_PIN, RLCD_MOSI_PIN};
    bool retain = g_settings.sleepScreen();
    for (size_t i = 0; i < sizeof(display_pins) / sizeof(display_pins[0]); i++) {
        if (retain) gpio_sleep_sel_dis(display_pins[i]);
        else        gpio_sleep_sel_en(display_pins[i]);
    }

    // 休眠前停掉打字音效的 I2S/ES8311/PA,避免休眠期 MCLK 耗电与 light-sleep 冲突
    typingClickRelease();

    // 完全关断 BLE 射频(若键盘已连接,deinit 会同时断开 HID 连接)
    g_bt.deinit();

    // 物理按键唤醒:两键都是 RTC GPIO、内部上拉、低电平有效,任意按下唤醒
    esp_err_t ext1_ret = esp_sleep_enable_ext1_wakeup(SLEEP_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    if (ext1_ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_ext1_wakeup failed: %d", ext1_ret);
    }

    ESP_LOGI(TAG, "Entering light sleep...");
    esp_err_t ret = esp_light_sleep_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_light_sleep_start failed: %d", ret);
    }
    ESP_LOGI(TAG, "Woke up, wakeup cause=%d", esp_sleep_get_wakeup_cause());
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 &&
        (esp_sleep_get_ext1_wakeup_status() & (1ULL << PIN_BOOT))) {
        s_boot_wake_release_pending = true;  // 被 BOOT 唤醒,忽略它本次释放以免立即再睡
    }

    // 白屏模式下休眠时面板被隔离复位,必须重新初始化;保留画面模式下面板未复位,
    // 但统一重新初始化 + 强制整屏重绘也无害,保证画面回到当前 UI
    u8g2_InitDisplay(g_u8g2);
    u8g2_SetPowerSave(g_u8g2, 0);
    ui_invalidate_snapshot();

    // 软件时钟在休眠期间冻结,从电池供电的 RTC 重同步,保证日记时间戳正确
    time_t t = g_rtc.getTime();
    if (t > 0) {
        struct timeval tv = {(time_t)t, 0};
        settimeofday(&tv, NULL);
    }

    // 后台重新 init BLE + 自动重连键盘(即使休眠失败也恢复 BT 栈)
    xTaskCreatePinnedToCore(btInitTask, "bt_init", 8192, NULL, 5, NULL, 1);
}

static void checkLightSleep(AppState state) {
    if (!g_settings.autoSleep()) return;
    if (g_wifi.isConnected()) return;  // 网络操作中不休眠
    if (state == APP_BT_MANAGE) return;  // 用户在蓝牙管理界面

    static int64_t s_last_wake_us = 0;
    int64_t now = esp_timer_get_time();

    // 启动后首次调用:以当前时刻作为空闲计时起点
    if (s_last_activity_us == 0) {
        s_last_activity_us = now;
        return;
    }

    // 唤醒后 2 分钟 grace,覆盖 BLE 重连窗口 + 用户操作
    if (s_last_wake_us != 0 && (now - s_last_wake_us) < AUTO_SLEEP_GRACE_US) return;

    // 键盘/按键空闲超过 10 分钟 → 进入休眠
    if (now - s_last_activity_us >= AUTO_SLEEP_TIMEOUT_US) {
        enterLightSleep();
        s_last_wake_us = esp_timer_get_time();
        s_last_activity_us = s_last_wake_us;  // 重置空闲计时基准,避免唤醒后立即再睡
    }
}

// ── Application Main Loop ──────────────────────────────────────────────

extern "C" void app_main() {
    ESP_LOGI(TAG, "pjournal-esp32 v" PJOURNAL_VERSION " starting...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed");
    }

    // Dynamic frequency scaling: CPU drops to 80MHz when idle (main loop delay),
    // ramps back to 240MHz while WiFi/BT are active (they hold PM locks).
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    if (esp_pm_configure(&pm_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed");
    }

    // Initialize buttons with pull-up for stable reading
    gpio_reset_pin(PIN_USER_BTN);
    gpio_set_direction(PIN_USER_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_USER_BTN, GPIO_PULLUP_ONLY);
    gpio_reset_pin(PIN_BOOT);
    gpio_set_direction(PIN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BOOT, GPIO_PULLUP_ONLY);

    // Initialize the UI before any fatal error screen can be drawn.
    g_font.begin();
    g_font.setSize(22);
    bool displayReady = initDisplay();
    if (!displayReady) {
        ESP_LOGE(TAG, "Display initialization failed! System halted.");
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // Initialize SD card (needed before settings on SD)
    if (!g_journal.begin()) {
        ESP_LOGE(TAG, "SD card initialization failed! System halted.");
        if (displayReady) {
            ui_clear();
            ui_draw_text_centered(100, "SD卡初始化失败");
            ui_draw_text_centered(135, "请检查SD卡");
            ui_commit();
        }
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "Journal entries: %d", g_journal.totalEntries());

    // Initialize settings (stored on SD card)
    g_settings.begin();

    // 工作模式: "journal"(个人日记) 或 "quick"(快捷编辑), 重启生效
    g_quickEdit = (g_settings.appMode() == "quick");
    if (g_quickEdit) quickEditInit();

    // Initialize RTC
    if (g_rtc.begin()) {
        ESP_LOGI(TAG, "PCF85063 RTC initialized");
    } else {
        ESP_LOGW(TAG, "PCF85063 RTC not available or invalid time");
    }

    // Initialize battery ADC
    battery_init();

    ui_clear();
    ui_draw_text_centered(100, g_quickEdit ? "快捷编辑" : "个人日记");
    char ver[32];
    snprintf(ver, sizeof(ver), "v" PJOURNAL_VERSION);
    ui_draw_text_centered(135, ver);
    ui_commit();

    // Initialize WiFi manager (但不自动连接)
    // WiFi 将在需要时按需连接（WebDAV同步、Flomo发送、Deepseek提示生成等）

    // Set timezone from settings (for local time display)
    {
        std::string tz = g_settings.timezone();
        if (tz.empty()) tz = "CST-8";
        setenv("TZ", tz.c_str(), 1);
        tzset();
    }

    // Time sync: prefer RTC if its time is recent (>= July 2026), otherwise NTP
    {
        time_t rtcTime = g_rtc.getTime();
        bool rtcRecent = (rtcTime >= 1782864000); // July 1, 2026 00:00:00 UTC

        if (rtcRecent) {
            struct timeval tv = {(time_t)rtcTime, 0};
            settimeofday(&tv, NULL);
            struct tm *tm = localtime(&rtcTime);
            char ts[32];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
            ESP_LOGI(TAG, "RTC time is recent, using directly: %s", ts);
        } else {
            ESP_LOGW(TAG, "RTC time (%lld) is before July 2026, attempting NTP sync...", (long long)rtcTime);
            std::string ssid = g_settings.wifiSsid();
            if (!ssid.empty()) {
                std::string ntp = g_settings.ntpServer();
                if (ntp.empty()) ntp = "pool.ntp.org";

                ui_draw_text_centered(165, "正在同步时间...");
                ui_commit();

                std::string pass = g_settings.wifiPassword();
                g_wifi.begin();
                if (g_wifi.connect(ssid.c_str(), pass.c_str())) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_sntp_stop();
                    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                    esp_sntp_setservername(0, ntp.c_str());
                    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
                    esp_sntp_init();

                    time_t now = 0;
                    for (int i = 0; i < 100; i++) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                            time(&now);
                            break;
                        }
                    }
                    if (now > 1782864000) {
                        struct tm *tm = localtime(&now);
                        char ts[32];
                        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
                        ESP_LOGI(TAG, "NTP sync succeeded: %s", ts);
                        g_rtc.setTime(now);
                    } else {
                        ESP_LOGW(TAG, "NTP sync timeout (%s)", ntp.c_str());
                    }
                    esp_sntp_stop();
                } else {
                    ESP_LOGW(TAG, "WiFi connection failed for NTP sync");
                }
                g_wifi.disconnect();
            } else {
                ESP_LOGW(TAG, "WiFi not configured, cannot NTP sync");
            }
            // Fallback: use whatever RTC has, even if old
            if (rtcTime > 1704067200) {
                struct timeval tv = {(time_t)rtcTime, 0};
                settimeofday(&tv, NULL);
                ESP_LOGW(TAG, "Fallback to RTC time");
            }
        }
    }

    // Seed RNG with hardware random for prompt selection
    srand(esp_random());

    // Initialize IME
    auto &ime = IME::getInstance();
    ime.begin();
    // Set candidate page size based on default 22pt font
    ime.setPageSize(7);
    // 候选字按显示宽度动态分页: 宽度回调复用当前字体, 可用宽度与各界面
    // 候选行渲染的 curW+partW+8>SCREEN_W 截断阈值一致(SCREEN_W=400)。
    ime.setWidthFn([](const char *s) -> int { return g_font.textWidth(s); });
    ime.setDisplayWidth(SCREEN_W - 8);

    // Initialize Bluetooth keyboard in background (non-blocking, faster boot)
    xTaskCreatePinnedToCore(btInitTask, "bt_init", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Ready!");

    // ── App State Machine ────────────────────────────────────────────────
    // 快捷编辑模式直接进入编辑器(续上次文件); 个人日记从主界面开始
    AppState currentState = g_quickEdit ? APP_EDITOR : APP_MAIN;
    ScreenContext ctx;
    if (g_quickEdit) {
        ctx.prevState = APP_SETTINGS;  // 编辑器 Esc → 设置, 设置 Esc → 编辑器
        ctx.promptMode = false;
        ctx.promptText = "";
    }
    static AppState inspReturnTo = APP_MAIN;

    // 物理按键状态(时间制,不依赖主循环节拍)
    struct BtnState {
        int64_t press_start_us = 0;   // 当前按下起始时刻(0=未按下)
        int64_t last_release_us = 0;  // 上次松开时刻(双击窗口基准)
        bool is_double = false;       // 本次按下为双击第二下
        bool long_fired = false;      // 本次按下已触发长按
    } btn_user, btn_boot;

    while (currentState != APP_QUIT) {
        checkLightSleep(currentState);

        int key = g_bt.readKey();
        if (key < 0) key = 0;

        // BLE 键盘输入视为活动,重置空闲休眠计时
        if (key > 0) s_last_activity_us = esp_timer_get_time();

        // Check for key repeat events
        g_bt.checkKeyRepeat();

        // Global Ctrl+Space IME toggle (only for editor)
        if (key == KEY_IME_TOGGLE && currentState == APP_EDITOR && !app_editor_search_active() && !app_editor_help_active()) {
            app_toggle_ime();
            key = 0;
        }
        // Shift+Space fullwidth toggle (only when IME active in editor)
        if (key == KEY_FULLWIDTH_TOGGLE && currentState == APP_EDITOR && app_ime_active() && !app_editor_search_active() && !app_editor_help_active()) {
            app_toggle_fullwidth();
            key = 0;
        }
        // Ctrl+Shift+F simplified/traditional toggle (only when IME active in editor)
        if (key == KEY_TRAD_TOGGLE && currentState == APP_EDITOR && app_ime_active() && !app_editor_search_active() && !app_editor_help_active()) {
            app_toggle_trad();
            key = 0;
        }
        // Left Shift tap → temp English mode toggle (only when IME active in editor)
        if (key == KEY_LSHIFT_TAP && currentState == APP_EDITOR && app_ime_active() && !app_editor_search_active() && !app_editor_help_active()) {
            app_toggle_english();
            key = 0;
        }

        // ── BT auto-reconnect retry ──────────────────────────────────────
        // 多设备: 断线后按最近使用顺序轮询已配对设备, 谁在线连谁
        // 面板内暂停自动重连, 避免干扰扫描/管理
        {
            static int64_t last_bt_retry_us = 0;
            static int64_t last_bt_reload_us = 0;
            static bool bt_list_loaded = false;
            static int bt_try_idx = 0;
            static bool bt_was_connected = false;

            if (currentState == APP_BT_MANAGE) {
                // 用户正在管理面板, 暂停自动重连
                bt_was_connected = false;
                last_bt_retry_us = 0;
            } else if (g_bt.isConnected()) {
                if (!bt_was_connected) {
                    ESP_LOGI(TAG, "Bluetooth connected, stopping retry logic");
                }
                bt_was_connected = true;
                last_bt_retry_us = 0;
            } else {
                if (bt_was_connected) {
                    ESP_LOGW(TAG, "Bluetooth disconnected");
                    bt_was_connected = false;
                }

                if (!bt_was_connected) {
                    // Periodically reload paired device list (反映面板增删/新连接)
                    if (g_bt.isInitialized()) {
                        int64_t now_us = esp_timer_get_time();
                        if (last_bt_reload_us == 0 || (now_us - last_bt_reload_us) > 30000000) {
                            last_bt_reload_us = now_us;
                            g_bt.loadPairedDevices();
                            bt_list_loaded = g_bt.pairedDeviceCount() > 0;
                            bt_try_idx = 0;
                            if (bt_list_loaded)
                                ESP_LOGI(TAG, "Loaded %d paired device(s)", g_bt.pairedDeviceCount());
                        }
                    }

                    if (bt_list_loaded && !g_bt.isConnecting()) {
                        int64_t now_us = esp_timer_get_time();
                        if (last_bt_retry_us == 0 || (now_us - last_bt_retry_us) > 2000000) {
                            last_bt_retry_us = now_us;
                            int n = g_bt.pairedDeviceCount();
                            if (n > 0) {
                                if (bt_try_idx >= n) bt_try_idx = 0;
                                const BtPairedDevice *p = g_bt.getPairedDevice(bt_try_idx);
                                ESP_LOGI(TAG, "BT auto-reconnect retry %d/%d...",
                                         bt_try_idx + 1, n);
                                g_bt.connectBDA(p->bda, p->addr_type);
                                bt_try_idx = (bt_try_idx + 1) % n;
                            }
                        }
                    }
                }
            }
        }

        // ── Physical button handling ──────────────────────────────────────
        // With pull-up: 1=released, 0=pressed (active LOW)
        #define PIN_LOW(gpio) (gpio_get_level(gpio) == 0)

        // 单击动作在双击窗口结束后才生效(防止双击第一下误发导航键,仅蓝牙管理面板有双击动作)
        if (s_pending_single.key != 0) {
            if (esp_timer_get_time() - s_pending_single.queued_us >= BTN_DOUBLE_WINDOW_US) {
                if (currentState == APP_BT_MANAGE ||
                    (currentState == APP_GTD && screen_gtd_accept_physical_buttons()) ||
                    currentState == APP_POLISH ||
                    currentState == APP_POLISH_PROMPT) {
                    key = s_pending_single.key;
                }
                s_pending_single.key = 0;
            }
        }

        // USER button (GPIO 18)
        {
            bool held = PIN_LOW(PIN_USER_BTN);
            if (held) {
                if (btn_user.press_start_us == 0) {
                    btn_user.press_start_us = esp_timer_get_time();
                    s_last_activity_us = btn_user.press_start_us;
                    // 上次松开后的双击窗口内再次按下 → 双击第二下,取消第一下的单击排队
                    btn_user.is_double = (btn_user.last_release_us > 0 &&
                        (btn_user.press_start_us - btn_user.last_release_us) < BTN_DOUBLE_WINDOW_US);
                    if (btn_user.is_double) s_pending_single.key = 0;
                }
                if (!btn_user.long_fired &&
                    (esp_timer_get_time() - btn_user.press_start_us) >= BTN_LONG_PRESS_US) {
                    btn_user.long_fired = true;
                    s_pending_single.key = 0;
                    if (currentState == APP_BT_MANAGE) {
                        // 蓝牙管理面板: 长按→连接选中设备(不经过短按,避免错位)
                        key = 0x0A;
                    } else if (currentState == APP_GTD && screen_gtd_accept_physical_buttons()) {
                        // GTD任务管理: 长按→Tab 切换标签
                        key = '\t';
                    } else if (currentState == APP_POLISH ||
                               currentState == APP_POLISH_PROMPT) {
                        // 润色面板/提示词编辑: 长按不动作,避免误进蓝牙管理
                        key = 0;
                    } else {
                        currentState = APP_BT_MANAGE;
                        key = 0;
                    }
                }
            } else {
                if (btn_user.press_start_us != 0) {
                    int64_t now = esp_timer_get_time();
                    int64_t dur = now - btn_user.press_start_us;
                    btn_user.press_start_us = 0;
                    if (dur >= BTN_DEBOUNCE_US && !btn_user.long_fired) {
                        bool gtdBrowse = (currentState == APP_GTD && screen_gtd_accept_physical_buttons());
                        bool gtdProjList = (currentState == APP_GTD && screen_gtd_in_project_list());
                        if (currentState == APP_BT_MANAGE && btn_user.is_double) {
                            // 双击: 管理模式→添加, 扫描模式→返回
                            key = screen_bt_manage_scan_mode() ? 0x1B : 'a';
                        } else if (currentState == APP_BT_MANAGE) {
                            // 单击: 上移,等待双击窗口确认非双击后生效
                            s_pending_single.key = KEY_UP;
                            s_pending_single.queued_us = now;
                        } else if (currentState == APP_EDITOR && btn_user.is_double && !app_editor_search_active() && !app_editor_help_active()) {
                            // 编辑器双击: 进入语音听写(会话在screen_voice_init启动)
                            currentState = APP_VOICE;
                            key = 0;
                        } else if (currentState == APP_VOICE && btn_user.is_double) {
                            // 语音听写双击: 注入ESC,由voice屏停止会话并返回编辑器
                            key = 0x1B;
                        } else if (currentState == APP_POLISH && btn_user.is_double) {
                            // 润色面板双击: 确认(Enter)
                            key = 0x0A;
                        } else if (currentState == APP_POLISH) {
                            // 润色面板单击: 上滚
                            s_pending_single.key = KEY_UP;
                            s_pending_single.queued_us = now;
                        } else if (currentState == APP_POLISH_PROMPT && btn_user.is_double) {
                            // 提示词编辑双击: 确认(Enter=换行)
                            key = 0x0A;
                        } else if (currentState == APP_POLISH_PROMPT) {
                            // 提示词编辑单击: 上移
                            s_pending_single.key = KEY_UP;
                            s_pending_single.queued_us = now;
                        } else if (gtdBrowse && btn_user.is_double) {
                            // GTD任务管理双击: 项目列表→进入选中项目; 平铺/项目树→切换任务状态
                            key = gtdProjList ? 0x0A : ' ';
                        } else if (gtdBrowse) {
                            // GTD任务管理单击: 上移,等待双击窗口确认非双击后生效
                            s_pending_single.key = KEY_UP;
                            s_pending_single.queued_us = now;
                        }
                    }
                    btn_user.last_release_us = now;
                    btn_user.long_fired = false;
                    btn_user.is_double = false;
                }
            }
        }

        // BOOT button (GPIO 0)
        {
            bool held = PIN_LOW(PIN_BOOT);
            if (held) {
                if (btn_boot.press_start_us == 0) {
                    btn_boot.press_start_us = esp_timer_get_time();
                    s_last_activity_us = btn_boot.press_start_us;
                    btn_boot.is_double = (btn_boot.last_release_us > 0 &&
                        (btn_boot.press_start_us - btn_boot.last_release_us) < BTN_DOUBLE_WINDOW_US);
                    if (btn_boot.is_double) s_pending_single.key = 0;
                }
                if (!btn_boot.long_fired &&
                    (esp_timer_get_time() - btn_boot.press_start_us) >= BTN_LONG_PRESS_US) {
                    if (currentState == APP_BT_MANAGE) {
                        // 长按→Esc(管理模式退出/扫描模式返回)
                        btn_boot.long_fired = true;
                        s_pending_single.key = 0;
                        key = 0x1B;
                    } else if (currentState == APP_GTD && screen_gtd_accept_physical_buttons()) {
                        // GTD任务管理: BOOT 长按不动作,避免面板内误触发休眠
                        btn_boot.long_fired = true;
                        s_pending_single.key = 0;
                    } else if (currentState == APP_POLISH ||
                               currentState == APP_POLISH_PROMPT) {
                        // 润色面板/提示词编辑: BOOT 长按不动作,避免误触发休眠
                        btn_boot.long_fired = true;
                        s_pending_single.key = 0;
                    }
                    // 其他界面: 长按 BOOT 在松开时进入休眠(单击不再休眠)
                }
            } else {
                // 唤醒按键可能已在扫描间隙松开,直接清除挂起的唤醒保护标记
                if (s_boot_wake_release_pending && btn_boot.press_start_us == 0) {
                    s_boot_wake_release_pending = false;
                }
                if (btn_boot.press_start_us != 0) {
                    int64_t now = esp_timer_get_time();
                    int64_t dur = now - btn_boot.press_start_us;
                    btn_boot.press_start_us = 0;
                    if (dur >= BTN_DEBOUNCE_US && !btn_boot.long_fired) {
                        if (currentState == APP_BT_MANAGE) {
                            if (btn_boot.is_double) {
                                // 双击: 管理模式→删除, 扫描模式→返回
                                key = screen_bt_manage_scan_mode() ? 0x1B : 'd';
                            } else {
                                // 单击: 下移,等待双击窗口确认非双击后生效
                                s_pending_single.key = KEY_DOWN;
                                s_pending_single.queued_us = now;
                            }
                        } else if (currentState == APP_GTD && screen_gtd_accept_physical_buttons()) {
                            if (btn_boot.is_double) {
                                // GTD任务管理双击: 项目树→返回项目选择菜单; 其他→无动作
                                screen_gtd_physical_double_boot();
                            } else {
                                // GTD任务管理单击: 下移(不触发休眠)
                                s_pending_single.key = KEY_DOWN;
                                s_pending_single.queued_us = now;
                            }
                        } else if (currentState == APP_EDITOR && btn_boot.is_double && !app_editor_search_active() && !app_editor_help_active()) {
                            // 编辑器双击: 进入AI润色面板(全文润色)
                            screen_polish_set_scope(POLISH_WHOLE);
                            currentState = APP_POLISH;
                            key = 0;
                        } else if (currentState == APP_POLISH) {
                            if (btn_boot.is_double) {
                                // 润色面板双击: 取消(Esc)
                                key = 0x1B;
                            } else {
                                // 润色面板单击: 下滚
                                s_pending_single.key = KEY_DOWN;
                                s_pending_single.queued_us = now;
                            }
                        } else if (currentState == APP_POLISH_PROMPT) {
                            if (btn_boot.is_double) {
                                // 提示词编辑双击: 取消(Esc)
                                key = 0x1B;
                            } else {
                                // 提示词编辑单击: 下移
                                s_pending_single.key = KEY_DOWN;
                                s_pending_single.queued_us = now;
                            }
                        } else if (s_boot_wake_release_pending) {
                            // 这是 BOOT 唤醒按键的释放,不进入休眠,避免唤醒即再睡
                            s_boot_wake_release_pending = false;
                        } else if (dur >= BTN_LONG_PRESS_US) {
                            // 长按 BOOT → 进入休眠(单击不再休眠)
                            enterLightSleep();
                            key = 0;  // 丢弃休眠前遗留的按键
                            s_last_activity_us = esp_timer_get_time();
                        }
                    }
                    btn_boot.last_release_us = now;
                    btn_boot.long_fired = false;
                    btn_boot.is_double = false;
                }
            }
        }

        // Global Ctrl+I → inspiration panel (works from any screen including editor)
        if (key == KEY_CTRL_I && currentState != APP_INSPIRATION && !app_editor_search_active() && !app_editor_help_active()) {
            inspReturnTo = currentState;
            currentState = APP_INSPIRATION;
            key = 0;
        }

        AppState prevState = currentState;  // before the switch, to detect transitions
        switch (currentState) {
        case APP_MAIN:
            g_font.setSize(22);
            if (key > 0) currentState = screen_main_handle(key, ctx);
            else { screen_main_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(200)); }  // 200ms for power saving
            break;

        case APP_EDITOR: {
            g_font.setSize(g_settings.fontSize());
            {
                int fs = g_font.fontSize();
                IME::getInstance().setPageSize(fs <= 22 ? 7 : 5);
            }
            static bool editorInited = false;
            if (app_editor_needs_reinit()) editorInited = false;
            if (!editorInited) { screen_editor_init(ctx); editorInited = true; }
            if (key > 0) currentState = screen_editor_handle(key, ctx);
            else { screen_editor_idle(ctx, false); vTaskDelay(pdMS_TO_TICKS(50)); }
            // Preserve editorInited when going to inspiration/polish (editor should resume)
            // Reset editorInited when editor is opened FROM another screen (new content)
            if (currentState != APP_EDITOR && currentState != APP_SYNC_SEND_FLOMO) {
                if (currentState == APP_INSPIRATION || currentState == APP_POLISH || currentState == APP_HISTORY) {
                    // editor state preserved across the overlay panel
                } else {
                    editorInited = false;
                }
            }
            break;
        }

        case APP_BROWSER: {
            g_font.setSize(22);
            static bool browserInited = false;
            if (!browserInited) { screen_browser_init(); browserInited = true; }
            if (key > 0) currentState = screen_browser_handle(key, ctx);
            else { screen_browser_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_BROWSER) browserInited = false;
            break;
        }

        case APP_VIEWER: {
            g_font.setSize(22);
            static bool viewerInited = false;
            if (!viewerInited) { screen_viewer_init(ctx.selectedEntry); viewerInited = true; }
            if (key > 0) currentState = screen_viewer_handle(key, ctx);
            else { screen_viewer_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_VIEWER) viewerInited = false;
            break;
        }

        case APP_HISTORY: {
            g_font.setSize(22);
            static bool historyInited = false;
            if (!historyInited) { screen_history_init(ctx.selectedEntry, ctx.prevState); historyInited = true; }
            if (key > 0) currentState = screen_history_handle(key, ctx);
            else { screen_history_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_HISTORY) historyInited = false;
            break;
        }

        case APP_SETTINGS: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool settingsInited = false;
            if (!settingsInited) { screen_settings_init(); settingsInited = true; }
            if (key > 0) currentState = screen_settings_handle(key, ctx);
            else { screen_settings_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_SETTINGS) settingsInited = false;
            break;
        }

        case APP_BT_MANAGE: {
            g_font.setSize(22);
            static bool btInited = false;
            if (!btInited) { screen_bt_manage_init(); btInited = true; }
            if (key > 0) currentState = screen_bt_manage_handle(key, ctx);
            else { screen_bt_manage_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_BT_MANAGE) btInited = false;
            break;
        }

        case APP_FILE_MANAGER: {
            g_font.setSize(22);
            static bool fileMgrInited = false;
            if (!fileMgrInited) { screen_file_manager_init(); fileMgrInited = true; }
            if (key > 0) currentState = screen_file_manager_handle(key, ctx);
            else { screen_file_manager_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(200)); }
            if (currentState != APP_FILE_MANAGER) fileMgrInited = false;
            break;
        }

        case APP_GTD: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool gtdInited = false;
            if (!gtdInited) { screen_gtd_init(); gtdInited = true; }
            if (key > 0) currentState = screen_gtd_handle(key, ctx);
            else { screen_gtd_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_GTD) gtdInited = false;
            break;
        }

        case APP_OUTLINE: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool outlineInited = false;
            if (!outlineInited) { screen_outline_init(); outlineInited = true; }
            if (key > 0) currentState = screen_outline_handle(key, ctx);
            else { screen_outline_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_OUTLINE) outlineInited = false;
            break;
        }

        case APP_INSPIRATION: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool inspInited = false;
            if (!inspInited) {
                screen_inspiration_init(inspReturnTo);
                inspInited = true;
            }
            if (key > 0) currentState = screen_inspiration_handle(key, ctx);
            else { screen_inspiration_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_INSPIRATION) inspInited = false;
            break;
        }

        case APP_POLISH: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool polishInited = false;
            if (!polishInited) {
                screen_polish_init();
                polishInited = true;
            }
            if (key > 0) currentState = screen_polish_handle(key, ctx);
            else { screen_polish_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_POLISH) polishInited = false;
            break;
        }

        case APP_POLISH_PROMPT: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);
            static bool ppInited = false;
            if (!ppInited) {
                screen_polish_prompt_init();
                ppInited = true;
            }
            if (key > 0) currentState = screen_polish_prompt_handle(key, ctx);
            else { screen_polish_prompt_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(100)); }
            if (currentState != APP_POLISH_PROMPT) ppInited = false;
            break;
        }

        case APP_SYNC_WEBDAV: {
            g_font.setSize(22);
            IME::getInstance().setPageSize(7);

            if (s_webdavState == AsyncUiState::Idle) {
                lockAsyncResult();
                s_webdavResult = {false, ""};
                unlockAsyncResult();
                s_webdavResultUntil = 0;
                s_webdavState = AsyncUiState::Running;
                TaskHandle_t h = nullptr;
                if (xTaskCreate(webdavSyncTask, "webdav_sync", 12288, nullptr, 1, &h) != pdPASS) {
                    s_webdavResult = {false, "系统繁忙,请重试"};
                    s_webdavState = AsyncUiState::Done;
                }
            }

            if (s_webdavState == AsyncUiState::Running) {
                drawCenteredBusy("WebDAV 同步", "正在同步...");
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            }

            if (s_webdavResultUntil == 0) {
                s_webdavResultUntil = esp_timer_get_time() + 2000000;
                lockAsyncResult();
                std::string message = s_webdavResult.message;
                unlockAsyncResult();
                drawCenteredBusy("WebDAV 同步",
                                 message.empty() ? "同步结束" : message.c_str());
                break;
            }
            if (esp_timer_get_time() < s_webdavResultUntil) {
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            }
            s_webdavState = AsyncUiState::Idle;
            currentState = APP_MAIN;
            break;
        }

        case APP_SYNC_SEND_FLOMO: {
            if (s_flomoState == AsyncUiState::Idle) {
                if (!g_flomoPendingText.empty()) {
                    s_flomoText = std::move(g_flomoPendingText);
                    g_flomoPendingText.clear();
                    s_flomoReturnTo = g_flomoReturnTo;
                } else {
                    s_flomoText = app_get_editor_text();
                    s_flomoReturnTo = APP_EDITOR;
                }
                lockAsyncResult();
                s_flomoResult = {false, ""};
                unlockAsyncResult();
                s_flomoResultUntil = 0;
                s_flomoState = AsyncUiState::Running;
                TaskHandle_t h = nullptr;
                if (xTaskCreate(flomoSendTask, "flomo_send", 8192, nullptr, 1, &h) != pdPASS) {
                    s_flomoResult = {false, "系统繁忙,请重试"};
                    s_flomoState = AsyncUiState::Done;
                }
            }

            if (s_flomoState == AsyncUiState::Running) {
                ui_clear();
                ui_show_message_centered("正在发送...");
                ui_commit();
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            }

            if (s_flomoResultUntil == 0) {
                s_flomoResultUntil = esp_timer_get_time() + 2000000;
                lockAsyncResult();
                std::string message = s_flomoResult.message;
                unlockAsyncResult();
                ui_clear();
                ui_show_message_centered(message.empty() ? "发送结束" : message.c_str());
                ui_commit();
                break;
            }
            if (esp_timer_get_time() < s_flomoResultUntil) {
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            }
            s_flomoText.clear();
            s_flomoState = AsyncUiState::Idle;
            currentState = s_flomoReturnTo;
            break;
        }

        case APP_VOICE: {
            g_font.setSize(g_settings.fontSize());
            static bool voiceInited = false;
            if (!voiceInited) { screen_voice_init(); voiceInited = true; }
            if (key > 0) currentState = screen_voice_handle(key, ctx);
            else { screen_voice_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(50)); }
            if (currentState != APP_VOICE) voiceInited = false;
            break;
        }

        default:
            currentState = APP_MAIN;
            break;
        }

        // 状态在本轮切换进编辑器时,屏幕已被上一界面盖过,置脏以便下轮重绘
        if (currentState != prevState && currentState == APP_EDITOR) screen_editor_reset_drawn();

        if (!ctx.statusMessage.empty()) {
            ui_clear();
            ui_show_message_centered(ctx.statusMessage.c_str());
            ctx.statusMessage.clear();
            vTaskDelay(pdMS_TO_TICKS(1500));
            // 编辑器空闲 tick 会跳过重绘,消息遮罩需在此手动刷掉
            if (currentState == APP_EDITOR) screen_editor_idle(ctx, true);
        }

        // Voice session WiFi idle: 5 min after the session ends, shut the radio.
        g_voice.update();
    }

    ESP_LOGI(TAG, "Goodbye.");
}
