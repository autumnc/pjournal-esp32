#include "user_config.h"
#include "font_renderer.h"
#include "bt_keyboard.h"
#include "wifi_manager.h"
#include "settings_manager.h"
#include "journal_storage.h"
#include "webdav_client.h"
#include "flomo_client.h"
#include "ime/IME.h"
#include "pjournal_app.h"
#include "u8g2_st7305.h"

#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_sntp.h>
#include <driver/gpio.h>

static const char *TAG = "Main";

// Display device (global, used by font_renderer.cpp)
static u8g2_st7305_t s_lcd_dev;
void *g_u8g2 = nullptr;

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

    // Initialize buttons with pull-up for stable reading
    gpio_reset_pin(PIN_USER_BTN);
    gpio_set_direction(PIN_USER_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_USER_BTN, GPIO_PULLUP_ONLY);
    gpio_reset_pin(PIN_BOOT);
    gpio_set_direction(PIN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BOOT, GPIO_PULLUP_ONLY);

    // Initialize settings
    g_settings.begin();

    // Initialize battery ADC
    battery_init();

    // Initialize SPIFFS for journal storage
    g_journal.begin();
    ESP_LOGI(TAG, "Journal entries: %d", g_journal.totalEntries());

    // Initialize font renderer
    g_font.begin();

    // Initialize display
    initDisplay();
    ui_clear();
    ui_draw_text_centered(100, "个人日记");
    char ver[32];
    snprintf(ver, sizeof(ver), "v" PJOURNAL_VERSION);
    ui_draw_text_centered(135, ver);
    ui_commit();

    // Initialize WiFi with stored credentials
    {
        std::string ssid = g_settings.wifiSsid();
        std::string pass = g_settings.wifiPassword();
        if (!ssid.empty()) {
            ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid.c_str());
            g_wifi.begin();
            g_wifi.connect(ssid.c_str(), pass.c_str());

            // Initialize NTP time
            std::string ntp = g_settings.ntpServer();
            std::string tz = g_settings.timezone();
            if (tz.empty()) tz = "CST-8";
            if (!ntp.empty()) {
                esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                esp_sntp_setservername(0, ntp.c_str());
                esp_sntp_init();
            }
            setenv("TZ", tz.c_str(), 1);
            tzset();
        }
    }

    // Initialize IME
    auto &ime = IME::getInstance();
    ime.begin();
    ime.setPageSize(5);

    // Initialize Bluetooth keyboard
    ESP_LOGI(TAG, "Starting Bluetooth...");
    g_bt.init();

    // Auto-connect to saved keyboard if available
    {
        uint8_t saved_bda[6];
        esp_ble_addr_type_t saved_addr_type;
        if (g_bt.loadPairedDevice(saved_bda, saved_addr_type)) {
            ESP_LOGI(TAG, "Found saved keyboard, auto-connecting...");
            g_bt.connectBDA(saved_bda, saved_addr_type);
            // Wait up to 8 seconds for connection
            for (int i = 0; i < 80; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
                if (g_bt.isConnected()) break;
            }
        }
    }

    // Show ready
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Ready!");

    // ── App State Machine ────────────────────────────────────────────────
    // Start with BT management screen on first boot for keyboard pairing
    AppState currentState = g_bt.isConnected() ? APP_MAIN : APP_BT_MANAGE;
    ScreenContext ctx;

    // Button debounce counters
    struct { int count; bool fired_long; } btn_user = {}, btn_boot = {};

    while (currentState != APP_QUIT) {
        int key = g_bt.readKey();
        if (key < 0) key = 0;

        // Global Ctrl+Space IME toggle
        if (key == KEY_IME_TOGGLE) {
            app_toggle_ime();
            key = 0;
        }

        // ── Physical button handling ──────────────────────────────────────
        // With pull-up: 1=released, 0=pressed (active LOW)
        // Simple counters, no auto-detection — just read the pin directly.
        #define PIN_LOW(gpio) (gpio_get_level(gpio) == 0)

        // USER button (GPIO 18)
        {
            bool held = PIN_LOW(PIN_USER_BTN);
            if (held) {
                btn_user.count++;
                // Short press ~150ms (3 iterations)
                if (btn_user.count == 3 && currentState == APP_BT_MANAGE)
                    key = KEY_UP;
                // Long press ~700ms (14 iterations)
                if (btn_user.count == 14) {
                    if (currentState != APP_BT_MANAGE) {
                        currentState = APP_BT_MANAGE;
                        key = 0;
                    }
                }
            } else {
                btn_user.count = 0;
            }
        }

        // BOOT button (GPIO 0)
        {
            bool held = PIN_LOW(PIN_BOOT);
            if (held) {
                btn_boot.count++;
                // Long press → confirm (in BT screen only)
                if (btn_boot.count == 14 && currentState == APP_BT_MANAGE) {
                    key = 0x0A;
                    btn_boot.fired_long = true;
                }
            } else {
                // Short press on release (only if didn't long-press)
                if (btn_boot.count >= 3 && btn_boot.count < 14 && !btn_boot.fired_long) {
                    if (currentState == APP_BT_MANAGE) key = KEY_DOWN;
                }
                btn_boot.count = 0;
                btn_boot.fired_long = false;
            }
        }
        switch (currentState) {
        case APP_MAIN:
            if (key > 0) currentState = screen_main_handle(key, ctx);
            else { screen_main_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(50)); }
            break;

        case APP_EDITOR: {
            static bool editorInited = false;
            if (!editorInited) { screen_editor_init(ctx); editorInited = true; }
            if (key > 0) currentState = screen_editor_handle(key, ctx);
            else { screen_editor_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(50)); }
            if (currentState != APP_EDITOR) editorInited = false;
            break;
        }

        case APP_BROWSER: {
            static bool browserInited = false;
            if (!browserInited) { screen_browser_init(); browserInited = true; }
            if (key > 0) currentState = screen_browser_handle(key, ctx);
            else { screen_browser_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(50)); }
            if (currentState != APP_BROWSER) browserInited = false;
            break;
        }

        case APP_VIEWER: {
            static bool viewerInited = false;
            if (!viewerInited) { screen_viewer_init(ctx.selectedEntry); viewerInited = true; }
            if (key > 0) currentState = screen_viewer_handle(key, ctx);
            else { screen_viewer_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(50)); }
            if (currentState != APP_VIEWER) viewerInited = false;
            break;
        }

        case APP_SETTINGS: {
            static bool settingsInited = false;
            if (!settingsInited) { screen_settings_init(); settingsInited = true; }
            if (key > 0) currentState = screen_settings_handle(key, ctx);
            else { screen_settings_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(50)); }
            if (currentState != APP_SETTINGS) settingsInited = false;
            break;
        }

        case APP_BT_MANAGE: {
            static bool btInited = false;
            if (!btInited) { screen_bt_manage_init(); btInited = true; }
            if (key > 0) currentState = screen_bt_manage_handle(key, ctx);
            else { screen_bt_manage_handle(0, ctx); vTaskDelay(pdMS_TO_TICKS(50)); }
            if (currentState != APP_BT_MANAGE) btInited = false;
            break;
        }

        case APP_SYNC_WEBDAV: {
            // Auto-connect WiFi if needed
            bool wifiWasConnected = g_wifi.isConnected();
            if (!wifiWasConnected) {
                std::string ssid = g_settings.wifiSsid();
                std::string pass = g_settings.wifiPassword();
                if (!ssid.empty()) {
                    g_wifi.begin();
                    g_wifi.connect(ssid.c_str(), pass.c_str());
                    // Wait up to 10s for WiFi connection
                    for (int i = 0; i < 100; i++) {
                        if (g_wifi.isConnected()) break;
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                }
            }

            ui_clear();
            ui_show_message_centered("正在同步...");
            std::string url = g_settings.webdavUrl();
            std::string user = g_settings.webdavUsername();
            std::string pass = g_settings.webdavPassword();
            if (url.empty() || user.empty()) {
                ui_show_message_centered("请先配置WebDAV");
                vTaskDelay(pdMS_TO_TICKS(2000));
            } else {
                g_webdav.configure(url, user, pass);
                auto result = g_webdav.sync("/sdcard/pjournal");
                ui_show_message_centered(result.message.c_str());
                vTaskDelay(pdMS_TO_TICKS(2000));
            }

            if (!wifiWasConnected) g_wifi.disconnect();
            currentState = APP_MAIN;
            break;
        }

        case APP_SYNC_SEND_FLOMO: {
            // Auto-connect WiFi if needed
            bool wifiWasConnected = g_wifi.isConnected();
            if (!wifiWasConnected) {
                std::string ssid = g_settings.wifiSsid();
                std::string pass = g_settings.wifiPassword();
                if (!ssid.empty()) {
                    g_wifi.begin();
                    g_wifi.connect(ssid.c_str(), pass.c_str());
                    // Wait up to 10s for WiFi connection
                    for (int i = 0; i < 100; i++) {
                        if (g_wifi.isConnected()) break;
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                }
            }

            ui_clear();
            ui_show_message_centered("正在发送...");
            std::string text = app_get_editor_text();
            if (!text.empty()) {
                auto result = g_flomo.send(text);
                ui_show_message_centered(result.message.c_str());
            } else {
                ui_show_message_centered("内容为空");
            }
            vTaskDelay(pdMS_TO_TICKS(2000));

            if (!wifiWasConnected) g_wifi.disconnect();
            currentState = APP_MAIN;
            break;
        }

        default:
            currentState = APP_MAIN;
            break;
        }

        if (!ctx.statusMessage.empty()) {
            ui_show_message_centered(ctx.statusMessage.c_str());
            ctx.statusMessage.clear();
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }

    ESP_LOGI(TAG, "Goodbye.");
}
