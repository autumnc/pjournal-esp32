#include "voice_input.h"
#include "settings_manager.h"
#include "wifi_manager.h"
#include "pcf85063.h"
#include "user_config.h"
#include "json_parser.h"
#include "typing_click.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <queue>
#include <vector>
#include <mutex>

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// audio codec
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

// websocket
#include <esp_websocket_client.h>

// opus encoder
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"

#define TAG "Voice"

VoiceInput g_voice;

// ── tunables ────────────────────────────────────────────────────────────────
#define VOICE_FRAME_MS          60
#define VOICE_SAMPLE_RATE       16000
#define VOICE_FRAME_SAMPLES     (VOICE_SAMPLE_RATE * VOICE_FRAME_MS / 1000)  // 960
#define VOICE_ES7210_CHANNELS   4                                            // TDM slots, downmixed to mono
#define WIFI_IDLE_SHUTDOWN_US   (5LL * 60 * 1000 * 1000)
#define ACTIVATE_POLL_MAX       60
#define ACTIVATE_POLL_INTERVAL_MS 3000
#define WS_HELLO_TIMEOUT_MS     10000
#define VOICE_MAX_RECONNECT     5
#define BIND_URL                "https://xiaozhi.me"
#define VOICE_ERROR_HOLD_MS     3000

// ── shared state (voice task <-> UI) ────────────────────────────────────────
static TaskHandle_t s_task = nullptr;
static volatile voice_state_t s_state = VOICE_IDLE;
static std::mutex s_mutex;
static std::string s_error;
static std::string s_code;
static std::string s_code_msg;
static std::string s_last_stt;
static std::queue<std::string> s_stt_queue;
static volatile bool s_stop = false;
// WiFi idle: armed (non-zero) after a voice session ends; main loop shuts wifi.
static int64_t s_wifi_idle_armed_us = 0;

static void setState(voice_state_t st) {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_state = st;
}
static void setError(const std::string &msg) {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_state = VOICE_ERROR;
    s_error = msg;
}
static void pushStt(const std::string &text) {
    std::lock_guard<std::mutex> lk(s_mutex);
    if (!text.empty()) {
        s_last_stt = text;
        s_stt_queue.push(text);
    }
}

// ── audio input (ES7210 via I2S TDM, downmixed to mono 16k) ─────────────────
static esp_codec_dev_handle_t s_input_dev = nullptr;
static i2s_chan_handle_t s_i2s_tx = nullptr;
static i2s_chan_handle_t s_i2s_rx = nullptr;
static const audio_codec_data_if_t *s_data_if = nullptr;
static const audio_codec_ctrl_if_t *s_in_ctrl = nullptr;
static const audio_codec_if_t *s_in_codec = nullptr;

// Tear down in reverse create order: dev -> codec_if -> ctrl_if -> data_if ->
// i2s channels. All steps are guarded so partial init/teardown is safe. Without
// deleting the i2s channels the controller stays occupied ("i2s controller 0
// has been occupied by i2s_driver") and every later session fails to init.
static void voice_audio_deinit() {
    if (s_input_dev != nullptr) {
        esp_codec_dev_close(s_input_dev);
        esp_codec_dev_delete(s_input_dev);
        s_input_dev = nullptr;
    }
    if (s_in_codec != nullptr) {
        audio_codec_delete_codec_if(s_in_codec);
        s_in_codec = nullptr;
    }
    if (s_in_ctrl != nullptr) {
        audio_codec_delete_ctrl_if(s_in_ctrl);
        s_in_ctrl = nullptr;
    }
    if (s_data_if != nullptr) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = nullptr;
    }
    // The TX std channel is enabled by esp_codec_dev as the I2S clock master,
    // so it's RUNNING at teardown; i2s_del_channel() refuses to delete a running
    // channel and would leak controller 0 (blocking every later session with
    // "i2s controller 0 has been occupied"). Disable both channels first.
    if (s_i2s_tx != nullptr) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
    }
    if (s_i2s_rx != nullptr) {
        i2s_channel_disable(s_i2s_rx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = nullptr;
    }
}

static bool voice_audio_init() {
    // 打字音效可能占用同一 I2S 控制器与 ES8311,听写前先归还控制权。
    typingClickRelease();
    i2c_master_bus_handle_t bus = pjournal_get_i2c_bus();
    if (bus == nullptr) {
        ESP_LOGE(TAG, "I2C bus not ready");
        return false;
    }

    // Clear any channels/interfaces leaked by a prior session or partial init.
    voice_audio_deinit();

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };
    if (i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }

    // TX: ES8311 output (std). Not enabled for MVP but keeps the I2S clock running.
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = VOICE_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = AUDIO_I2S_GPIO_MCLK,
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_WS,
            .dout = AUDIO_I2S_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    // RX: ES7210 input (TDM, 4 mics).
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = VOICE_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = AUDIO_I2S_GPIO_MCLK,
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_I2S_GPIO_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(s_i2s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_init_tdm_mode(s_i2s_rx, &tdm_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel init failed");
        voice_audio_deinit();
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_i2s_rx, .tx_handle = s_i2s_tx, .clk_src = 0 };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == nullptr) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        voice_audio_deinit();
        return false;
    }

    audio_codec_i2c_cfg_t i2c_cfg = { .port = (i2c_port_t)1, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = bus, .clock_speed_hz = 0 };
    s_in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_in_ctrl == nullptr) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        voice_audio_deinit();
        return false;
    }

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = s_in_ctrl;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    s_in_codec = es7210_codec_new(&es7210_cfg);
    if (s_in_codec == nullptr) {
        ESP_LOGE(TAG, "es7210_codec_new failed");
        voice_audio_deinit();
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = s_in_codec;
    dev_cfg.data_if = s_data_if;
    s_input_dev = esp_codec_dev_new(&dev_cfg);
    if (s_input_dev == nullptr) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        voice_audio_deinit();
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = VOICE_ES7210_CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
        .sample_rate = VOICE_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(s_input_dev, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        voice_audio_deinit();
        return false;
    }
    // Max analog gain (37.5dB): the board's mics run quiet and the server's VAD
    // won't trigger below a few hundred RMS.
    esp_codec_dev_set_in_channel_gain(s_input_dev,
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3), 37.5f);

    ESP_LOGI(TAG, "Audio input initialized (ES7210 %dch @%dHz)", VOICE_ES7210_CHANNELS, VOICE_SAMPLE_RATE);
    return true;
}

// Read 60ms of mono PCM (takes mic1 / slot0 only; the reference xiaozhi build
// uses channel_mask=MASK(0), and averaging all 4 slots attenuates the signal
// when the other mics are dead/noisy).
static bool voice_audio_read_mono(int16_t *out, size_t samples) {
    static std::vector<int16_t> raw;
    size_t raw_sz = samples * VOICE_ES7210_CHANNELS;
    raw.resize(raw_sz);
    int bytes = raw_sz * sizeof(int16_t);
    // esp_codec_dev_read returns ESP_CODEC_DEV_OK (0) on success and fills the
    // whole buffer (i2s_channel_read blocks until `bytes` are read); any other
    // return value is an error/not-ready condition to retry.
    int ret = esp_codec_dev_read(s_input_dev, raw.data(), bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "codec read not ready: %d", ret);
        return false;
    }
    for (size_t i = 0; i < samples; i++)
        out[i] = raw[i * VOICE_ES7210_CHANNELS];
    return true;
}

// ── HTTP helper (esp_http_client + crt bundle) ──────────────────────────────
struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string err;
};

static HttpResult voice_http(const std::string &url, const std::string &method,
                             const std::string &body, const std::vector<std::string> &headers) {
    HttpResult res;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = 15000;
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        res.err = "HTTP初始化失败";
        return res;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    for (auto &h : headers) {
        size_t pos = h.find(':');
        if (pos != std::string::npos) {
            std::string k = h.substr(0, pos);
            std::string v = h.substr(pos + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            esp_http_client_set_header(client, k.c_str(), v.c_str());
        }
    }
    esp_err_t err = esp_http_client_open(client, (int)body.size());
    if (err == ESP_OK) {
        if (!body.empty()) {
            esp_http_client_write(client, body.c_str(), (int)body.size());
        }
        esp_http_client_fetch_headers(client);
        res.status = esp_http_client_get_status_code(client);
        char buf[512];
        int len;
        while ((len = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
            buf[len] = 0;
            res.body += buf;
            if (res.body.size() > 16384) break;
        }
        res.ok = true;
    } else {
        res.err = esp_err_to_name(err);
    }
    esp_http_client_cleanup(client);
    return res;
}

static std::string voice_mac_str() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

// ── activation / check-version ──────────────────────────────────────────────
struct WsConfig {
    std::string url;
    std::string token;
    int version = 3;
};

// POST system-info to ota_url; fills ws_cfg (may be empty) and, if the device
// isn't bound yet, activation {code, message}.
static HttpResult voice_check_version(const std::string &ota_url, WsConfig &ws_cfg,
                                      std::string &code, std::string &message) {
    std::string mac = voice_mac_str();
    // Minimal system-info payload the xiaozhi OTA/activation server expects.
    std::string body = "{\"version\":2,\"language\":\"zh-CN\","
                       "\"mac_address\":\"" + mac + "\","
                       "\"chip_model_name\":\"esp32s3\","
                       "\"application\":{\"name\":\"pjournal\",\"version\":\"" PJOURNAL_VERSION "\"}}";
    std::vector<std::string> headers;
    headers.push_back("Activation-Version:1");
    headers.push_back("Device-Id:" + mac);
    headers.push_back("Client-Id:" + g_settings.clientId());
    headers.push_back("User-Agent:pjournal/" PJOURNAL_VERSION);

    HttpResult res = voice_http(ota_url, "POST", body, headers);
    if (!res.ok) {
        ESP_LOGE(TAG, "check-version http failed: %s", res.err.c_str());
        return res;
    }
    if (res.status != 200) {
        res.ok = false;
        res.err = "服务器返回 " + std::to_string(res.status);
        return res;
    }
    JsonValue root = JsonValue::parse(res.body);
    if (root.isObject()) {
        if (root.has("activation")) {
            JsonValue act = root["activation"];
            if (act.has("code")) code = act["code"].asString();
            if (act.has("message")) message = act["message"].asString();
        }
        if (root.has("websocket")) {
            JsonValue ws = root["websocket"];
            if (ws.has("url")) ws_cfg.url = ws["url"].asString();
            if (ws.has("token")) ws_cfg.token = ws["token"].asString();
            if (ws.has("version")) ws_cfg.version = ws["version"].asInt(3);
        }
    }
    return res;
}

static HttpResult voice_activate(const std::string &ota_url) {
    std::string url = ota_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/activate";
    // No efuse serial number (Activation-Version:1): empty payload is fine.
    std::vector<std::string> headers;
    headers.push_back("Activation-Version:1");
    headers.push_back("Device-Id:" + voice_mac_str());
    headers.push_back("Client-Id:" + g_settings.clientId());
    return voice_http(url, "POST", "{}", headers);
}

// ── WebSocket + 小智 protocol ───────────────────────────────────────────────
static esp_websocket_client_handle_t s_ws = nullptr;
static volatile bool s_ws_connected = false;
static volatile bool s_ws_hello = false;
static std::string s_session_id;

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "websocket connected");
            s_ws_connected = true;
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "websocket disconnected");
            s_ws_connected = false;
            break;
        case WEBSOCKET_EVENT_DATA:
            if (e->op_code == 0x1 && e->data_len > 0) {
                std::string text(e->data_ptr, e->data_len);
                JsonValue root = JsonValue::parse(text);
                if (root.isObject() && root.has("type")) {
                    std::string type = root["type"].asString();
                    if (type == "hello") {
                        if (root.has("session_id")) s_session_id = root["session_id"].asString();
                        s_ws_hello = true;
                        ESP_LOGI(TAG, "server hello, session=%s", s_session_id.c_str());
                    } else if (type == "stt" && root.has("text")) {
                        std::string t = root["text"].asString();
                        ESP_LOGI(TAG, "stt: %s", t.c_str());
                        pushStt(t);
                    }
                    // Everything else (llm / tts / system) is AI content the
                    // device deliberately ignores — dictation is STT-only.
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "websocket error");
            break;
        default:
            break;
    }
}

static bool ws_send_text(const std::string &text) {
    if (s_ws == nullptr || !s_ws_connected) return false;
    return esp_websocket_client_send_text(s_ws, text.c_str(), (int)text.size(), pdMS_TO_TICKS(2000)) >= 0;
}

static bool ws_send_audio(const void *payload, size_t len) {
    if (s_ws == nullptr || !s_ws_connected) return false;
    // BinaryProtocol3: uint8 type(0) + uint8 reserved + uint16 payload_size (big-endian)
    std::string frame;
    frame.resize(4 + len);
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = (char)((len >> 8) & 0xFF);
    frame[3] = (char)(len & 0xFF);
    memcpy(&frame[4], payload, len);
    // Short timeout: a stall here means the server stopped reading (a dropped
    // socket); the voice task reconnects, so detect it fast instead of hanging.
    return esp_websocket_client_send_bin(s_ws, frame.data(), (int)frame.size(), pdMS_TO_TICKS(2000)) >= 0;
}

static void ws_send_hello() {
    ws_send_text("{\"type\":\"hello\",\"version\":3,\"features\":{},\"transport\":\"websocket\","
                 "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}");
}

static void ws_send_listen_start() {
    // "realtime" streams interim STT during speech (manual only caches audio and
    // runs ASR once on listen stop, so nothing arrives until the session ends).
    ws_send_text("{\"session_id\":\"" + s_session_id + "\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"realtime\"}");
}

static void ws_send_listen_stop() {
    ws_send_text("{\"session_id\":\"" + s_session_id + "\",\"type\":\"listen\",\"state\":\"stop\"}");
}

static bool ws_connect(const WsConfig &cfg) {
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = cfg.url.c_str();
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.disable_auto_reconnect = true;
    ws_cfg.network_timeout_ms = 10000;
    std::string mac = voice_mac_str();
    std::string headers = "Protocol-Version: 3\r\nDevice-Id: " + mac +
                          "\r\nClient-Id: " + g_settings.clientId() + "\r\n";
    if (!cfg.token.empty()) {
        headers += "Authorization: Bearer " + cfg.token + "\r\n";
    }
    ws_cfg.headers = headers.c_str();

    s_ws = esp_websocket_client_init(&ws_cfg);
    if (s_ws == nullptr) return false;
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, nullptr);

    s_ws_connected = false;
    s_ws_hello = false;
    s_session_id.clear();
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
        return false;
    }
    // Wait for the TCP+TLS+WS handshake.
    int64_t deadline = esp_timer_get_time() + WS_HELLO_TIMEOUT_MS * 1000;
    while (!s_ws_connected && esp_timer_get_time() < deadline) {
        if (s_stop) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_ws_connected) return false;
    // Send hello FIRST, then wait for the server's hello + session_id.
    ws_send_hello();
    deadline = esp_timer_get_time() + WS_HELLO_TIMEOUT_MS * 1000;
    while (!s_ws_hello && esp_timer_get_time() < deadline) {
        if (s_stop) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return s_ws_hello;
}

static void ws_close() {
    if (s_ws != nullptr) {
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(s_ws);
        s_ws = nullptr;
    }
    s_ws_connected = false;
    s_ws_hello = false;
    s_session_id.clear();
}

// ── voice task ──────────────────────────────────────────────────────────────
static void voice_task(void *arg) {
    ESP_LOGI(TAG, "voice task started");

    // 1. WiFi (reuse if already connected)
    setState(VOICE_CONNECTING_WIFI);
    if (!g_wifi.isConnected()) {
        std::string ssid = g_settings.wifiSsid();
        std::string pwd = g_settings.wifiPassword();
        if (ssid.empty()) {
            setError("未配置WiFi,请在设置中填写");
            goto cleanup;
        }
        g_wifi.begin();
        if (!g_wifi.connect(ssid.c_str(), pwd.c_str())) {
            setError("WiFi连接失败");
            goto cleanup;
        }
    }
    ESP_LOGI(TAG, "wifi ok, ip=%s", g_wifi.getIp().c_str());

    // 2. OTA check-version + activation
    setState(VOICE_CONNECTING_SERVER);
    {
        std::string ota_url = g_settings.xiaozhiOtaUrl();
        WsConfig ws_cfg;
        std::string code, msg;
        HttpResult res = voice_check_version(ota_url, ws_cfg, code, msg);
        if (!res.ok) {
            setError("连接小智服务器失败: " + res.err);
            goto cleanup;
        }
        ESP_LOGI(TAG, "check-version ok: activation=%s ws_url=%s token_len=%zu",
                 code.empty() ? "(none)" : code.c_str(), ws_cfg.url.c_str(), ws_cfg.token.size());
        if (!code.empty()) {
            setState(VOICE_WAIT_ACTIVATE);
            {
                std::lock_guard<std::mutex> lk(s_mutex);
                s_code = code;
                s_code_msg = msg.empty() ? ("请在 " BIND_URL " 输入验证码绑定设备") : msg;
            }
            // Poll /activate until bound (or user exits / timeout).
            setState(VOICE_ACTIVATING);
            int tries = 0;
            bool bound = false;
            while (!s_stop && tries < ACTIVATE_POLL_MAX) {
                HttpResult act = voice_activate(ota_url);
                if (act.status == 200) { bound = true; break; }
                if (act.status != 202) {
                    ESP_LOGW(TAG, "activate status=%d body=%s", act.status, act.body.c_str());
                    if (act.ok && act.status != 202) {
                        setError("绑定请求失败(" + std::to_string(act.status) + ")");
                        goto cleanup;
                    }
                }
                tries++;
                vTaskDelay(pdMS_TO_TICKS(ACTIVATE_POLL_INTERVAL_MS));
            }
            if (!bound && !s_stop) {
                setError("绑定超时,请重试");
                goto cleanup;
            }
            // Re-fetch to get the websocket config after binding.
            setState(VOICE_CONNECTING_SERVER);
            WsConfig ws2;
            std::string c2, m2;
            res = voice_check_version(ota_url, ws2, c2, m2);
            if (!res.ok) {
                setError("绑定后获取服务器配置失败");
                goto cleanup;
            }
            ws_cfg = ws2;
        }
        if (ws_cfg.url.empty()) {
            setError("服务器未返回WebSocket地址");
            goto cleanup;
        }

        // 3. audio capture + opus, once per session (survives WS reconnects)
        if (s_stop) goto cleanup;
        if (!voice_audio_init()) {
            setError("麦克风初始化失败");
            goto cleanup;
        }
        std::vector<int16_t> pcm(VOICE_FRAME_SAMPLES);
        std::vector<uint8_t> obuf(2048);
        void *opus = nullptr;
        {
            esp_opus_enc_config_t opus_cfg = {
                .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
                .channel = ESP_AUDIO_MONO,
                .bits_per_sample = ESP_AUDIO_BIT16,
                .bitrate = ESP_OPUS_BITRATE_AUTO,
                .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
                .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
                .complexity = 0,
                .enable_fec = false,
                .enable_dtx = true,
                .enable_vbr = true,
            };
            if (esp_opus_enc_open(&opus_cfg, sizeof(esp_opus_enc_config_t), &opus) != ESP_AUDIO_ERR_OK || opus == nullptr) {
                ESP_LOGE(TAG, "opus enc open failed");
                setError("Opus编码器初始化失败");
                goto cleanup_audio;
            }
            int in_size = 0, out_size = 0;
            esp_opus_enc_get_frame_size(opus, &in_size, &out_size);
            if (out_size > 0) obuf.resize((size_t)out_size);
        }

        // 4. Connect + stream, auto-reconnecting if the socket drops mid-session.
        {
            int reconnect = 0;
            while (!s_stop) {
                setState(VOICE_CONNECTING_SERVER);
                if (!ws_connect(ws_cfg)) {
                    if (s_stop) break;
                    if (reconnect >= VOICE_MAX_RECONNECT) {
                        setError("连接语音服务器失败");
                        break;
                    }
                    reconnect++;
                    ESP_LOGW(TAG, "ws connect failed, retry %d/%d", reconnect, VOICE_MAX_RECONNECT);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
                reconnect = 0;
                // realtime streams interim stt during speech.
                ws_send_listen_start();
                setState(VOICE_LISTENING);

                while (!s_stop && s_ws_connected) {
                    if (!voice_audio_read_mono(pcm.data(), VOICE_FRAME_SAMPLES)) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                        continue;
                    }
                    // Periodic RMS diagnostic (every ~1s) to confirm real mic data.
                    {
                        static int64_t last_rms_log = 0;
                        static int64_t rms_acc = 0;
                        static int frames = 0;
                        for (int i = 0; i < VOICE_FRAME_SAMPLES; i++)
                            rms_acc += (int64_t)pcm[i] * pcm[i];
                        frames++;
                        int64_t now = esp_timer_get_time();
                        if (now - last_rms_log > 1000000) {
                            int64_t mean = rms_acc / frames / VOICE_FRAME_SAMPLES;
                            int rms = (int)std::sqrt((double)mean);
                            ESP_LOGI(TAG, "mic rms=%d frames=%d", rms, frames);
                            last_rms_log = now;
                            rms_acc = 0;
                            frames = 0;
                        }
                    }
                    esp_audio_enc_in_frame_t in = {
                        .buffer = (uint8_t *)pcm.data(),
                        .len = (uint32_t)(VOICE_FRAME_SAMPLES * sizeof(int16_t)),
                    };
                    esp_audio_enc_out_frame_t out = {
                        .buffer = obuf.data(),
                        .len = (uint32_t)obuf.size(),
                        .encoded_bytes = 0,
                        .pts = 0,
                    };
                    if (esp_opus_enc_process(opus, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
                        ws_send_audio(obuf.data(), out.encoded_bytes);
                    }
                }

                // Finalize this connection: stop the utterance, let the last stt land.
                if (s_ws_connected) {
                    ws_send_listen_stop();
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                ws_close();
                if (s_stop) break;
                if (reconnect >= VOICE_MAX_RECONNECT) {
                    setError("连接中断,请重试");
                    break;
                }
                reconnect++;
                ESP_LOGW(TAG, "ws dropped, reconnecting %d/%d", reconnect, VOICE_MAX_RECONNECT);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            esp_opus_enc_close(opus);
        }

cleanup_audio:
        voice_audio_deinit();
    }

cleanup:
    if (!s_stop && s_state == VOICE_ERROR) {
        // Hold the error on screen until the user double-clicks to exit
        // or the hold expires, so the UI has time to render it.
        int64_t t0 = esp_timer_get_time();
        while (!s_stop && (esp_timer_get_time() - t0) < VOICE_ERROR_HOLD_MS * 1000)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    setState(VOICE_IDLE);
    // Arm 5-min wifi idle shutdown (main loop will disconnect the radio).
    s_wifi_idle_armed_us = esp_timer_get_time();
    ESP_LOGI(TAG, "voice task done, stack watermark %u",
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// ── public API ──────────────────────────────────────────────────────────────
bool VoiceInput::start() {
    if (s_task != nullptr) return false;
    s_stop = false;
    s_error.clear();
    s_code.clear();
    s_code_msg.clear();
    s_last_stt.clear();
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        while (!s_stt_queue.empty()) s_stt_queue.pop();
    }
    setState(VOICE_CONNECTING_WIFI);
    s_wifi_idle_armed_us = 0;
    // Opus encoding is stack-hungry (xiaozhi gives it a 24KB task); this task
    // also does WiFi/HTTP/WS, so give it plenty of headroom.
    if (xTaskCreate(voice_task, "voice", 24576, nullptr, 2, &s_task) != pdPASS) {
        s_task = nullptr;
        return false;
    }
    return true;
}

void VoiceInput::requestStop() {
    s_stop = true;
}

bool VoiceInput::isActive() const {
    return s_task != nullptr;
}

voice_state_t VoiceInput::state() const {
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_state;
}

std::string VoiceInput::errorMessage() const {
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_error;
}

std::string VoiceInput::activationCode() const {
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_code;
}

std::string VoiceInput::activationMessage() const {
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_code_msg;
}

std::string VoiceInput::lastStt() const {
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_last_stt;
}

bool VoiceInput::popStt(std::string &out) {
    std::lock_guard<std::mutex> lk(s_mutex);
    if (s_stt_queue.empty()) return false;
    out = std::move(s_stt_queue.front());
    s_stt_queue.pop();
    return true;
}

void VoiceInput::update() {
    // WiFi idle shutdown: 5 min after the last voice session ends, turn the radio off.
    if (s_wifi_idle_armed_us == 0) return;
    if (s_task != nullptr) return;          // session still active
    if (!g_wifi.isConnected()) { s_wifi_idle_armed_us = 0; return; }
    if (esp_timer_get_time() - s_wifi_idle_armed_us >= WIFI_IDLE_SHUTDOWN_US) {
        ESP_LOGI(TAG, "voice wifi idle 5min, shutting down radio");
        g_wifi.disconnect();
        s_wifi_idle_armed_us = 0;
    }
}
