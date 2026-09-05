#include "typing_click.h"
#include "settings_manager.h"
#include "pcf85063.h"  // pjournal_get_i2c_bus()
#include "user_config.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cmath>
#include <string>
#include <vector>

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>  // es8311_codec.h, i2c/i2s/gpio factories

#define TAG "TypeClick"

#define TC_SAMPLE_RATE  AUDIO_SAMPLE_RATE  // 16000
#define TC_STEREO       2
#define TC_IDLE_US      (30LL * 1000 * 1000)  // 闲置 30s 自动拆音频
#define TC_MAX_N        8                      // 多声连发上限(防异常长文本)
#define TC_GAP_MS       30                     // 多声间静音间隔
#define TC_CAP_MS       160                    // 单声最大时长(截断上限;风铃等长衰减音色的自然衰减仍<此值)

static i2s_chan_handle_t s_tx = nullptr;
static const audio_codec_data_if_t *s_data = nullptr;
static const audio_codec_ctrl_if_t *s_ctrl = nullptr;
static const audio_codec_gpio_if_t *s_gpio = nullptr;
static const audio_codec_if_t *s_codec = nullptr;
static esp_codec_dev_handle_t s_dev = nullptr;
static bool s_ready = false;
static int64_t s_last_play_us = 0;
static int s_vol_cache = -1;
static std::string s_timbre_cache;
static std::vector<int16_t> s_click;  // 单声立体声(交错 L/R 同值)

// 音色预设:基频、谐波、attack(ms)、衰减 tau(ms)、峰值(0..1)
// 键名须与 screen_settings.cpp TIMBRE_OPTS 同步
struct Timbre { const char *name; double f0; double h2, h3, h5; double attackMs; double tauMs; double peak; };
static const Timbre TIMBRES[] = {
    { "mechanical",  1800, 0.00, 0.35, 0.18, 0.4, 6.0, 0.28 },  // 机械:短促脆"嗒"
    { "soft",         900, 0.15, 0.00, 0.00, 1.5, 16.0, 0.18 }, // 柔和:温和低鸣
    { "electronic",  2200, 0.00, 0.00, 0.00, 0.5, 8.0, 0.25 },  // 电子:干净"滴"
    { "clack",       1000, 0.10, 0.55, 0.30, 0.4, 7.0, 0.32 },  // 打字机:低频厚重"咔哒"
    { "wooden",      1250, 0.00, 0.55, 0.30, 0.2, 3.0, 0.28 },  // 木鱼:短促空心"笃"
    { "crisp",       2600, 0.05, 0.15, 0.00, 0.2, 5.0, 0.24 },  // 清脆:明亮高音"叮"
    { "chime",       1400, 0.55, 0.18, 0.08, 0.8, 24.0, 0.15 }, // 风铃:带八度泛音,余音略长
};
static const Timbre *timbreOf(const std::string &name) {
    for (const auto &t : TIMBRES) if (name == t.name) return &t;
    return &TIMBRES[0];
}

static bool enabled() { return g_settings.inputMode() == "typewriter"; }

// 拆音频:与 voice_audio_deinit 同样逆序 + 先 disable 后 del,防泄漏控制器 0
static void tcRelease() {
    if (s_dev != nullptr) {
        esp_codec_dev_close(s_dev);
        esp_codec_dev_delete(s_dev);
        s_dev = nullptr;
    }
    if (s_codec != nullptr) { audio_codec_delete_codec_if(s_codec); s_codec = nullptr; }
    if (s_gpio != nullptr) { audio_codec_delete_gpio_if(s_gpio); s_gpio = nullptr; }
    if (s_ctrl != nullptr) { audio_codec_delete_ctrl_if(s_ctrl); s_ctrl = nullptr; }
    if (s_data != nullptr) { audio_codec_delete_data_if(s_data); s_data = nullptr; }
    if (s_tx != nullptr) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
    }
    s_ready = false;
    s_vol_cache = -1;
    s_click.clear();
}

// 合成当前音色的单声立体声缓冲(指数衰减到 <0.4% 截断,最长 TC_CAP_MS)
static void renderClick(const std::string &scheme) {
    const Timbre &t = *timbreOf(scheme);
    double ampSum = 1.0 + t.h2 + t.h3 + t.h5;
    double atkS = t.attackMs / 1000.0, tauS = t.tauMs / 1000.0;
    int capSamples = TC_CAP_MS * TC_SAMPLE_RATE / 1000;
    std::vector<int16_t> mono;
    mono.reserve((size_t)capSamples);
    const double TWO_PI = 6.283185307179586;
    for (int i = 0; i < capSamples; i++) {
        double tt = (double)i / TC_SAMPLE_RATE;
        double env = tt < atkS ? tt / atkS : exp(-(tt - atkS) / tauS);
        if (env < 0.004 && tt > atkS) break;
        double ph = TWO_PI * t.f0 * tt;
        double wave = (sin(ph) + t.h2 * sin(2.0 * ph) + t.h3 * sin(3.0 * ph) +
                       t.h5 * sin(5.0 * ph)) / ampSum;
        double v = env * wave * t.peak * 32767.0;
        int16_t s = (int16_t)(v < -32767.0 ? -32767 : v > 32767.0 ? 32767 : v);
        mono.push_back(s);
    }
    s_click.resize(mono.size() * TC_STEREO);
    for (size_t i = 0; i < mono.size(); i++) {
        s_click[i * 2] = mono[i];
        s_click[i * 2 + 1] = mono[i];
    }
    s_timbre_cache = scheme;
}

// 逐个打开 ES8311 DAC 输出(TX std),路径与 voice_input 的 TX 配置一致
static bool tcInit() {
    if (s_ready) return true;
    i2c_master_bus_handle_t bus = pjournal_get_i2c_bus();
    if (bus == nullptr) { ESP_LOGE(TAG, "I2C bus not ready"); return false; }

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
    if (i2s_new_channel(&chan_cfg, &s_tx, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = TC_SAMPLE_RATE,
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
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
        tcRelease();
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = nullptr,
                                      .tx_handle = s_tx, .clk_src = 0 };
    s_data = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data == nullptr) { ESP_LOGE(TAG, "audio_codec_new_i2s_data failed"); tcRelease(); return false; }

    audio_codec_i2c_cfg_t i2c_cfg = { .port = (i2c_port_t)1, .addr = ES8311_CODEC_DEFAULT_ADDR,
                                      .bus_handle = bus, .clock_speed_hz = 0 };
    s_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_ctrl == nullptr) { ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed"); tcRelease(); return false; }

    s_gpio = audio_codec_new_gpio();
    es8311_codec_cfg_t es_cfg = {};
    es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es_cfg.ctrl_if = s_ctrl;
    es_cfg.gpio_if = s_gpio;
    es_cfg.pa_pin = AUDIO_CODEC_PA_PIN;
    es_cfg.pa_reverted = false;
    es_cfg.master_mode = false;
    es_cfg.use_mclk = true;
    es_cfg.digital_mic = false;
    es_cfg.invert_mclk = false;
    es_cfg.invert_sclk = false;
    es_cfg.no_dac_ref = false;
    s_codec = es8311_codec_new(&es_cfg);
    if (s_codec == nullptr) { ESP_LOGE(TAG, "es8311_codec_new failed"); tcRelease(); return false; }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = s_codec;
    dev_cfg.data_if = s_data;
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (s_dev == nullptr) { ESP_LOGE(TAG, "esp_codec_dev_new failed"); tcRelease(); return false; }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = TC_STEREO,
        .channel_mask = 0,
        .sample_rate = TC_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        tcRelease();
        return false;
    }

    renderClick(g_settings.typingClickTimbre());
    int vol = g_settings.typingClickVolume();
    if (vol > 0) esp_codec_dev_set_out_vol(s_dev, vol);
    s_vol_cache = vol;
    s_ready = true;
    s_last_play_us = esp_timer_get_time();
    ESP_LOGI(TAG, "ES8311 DAC ready (vol=%d%%)", vol);
    return true;
}

void typingClickRelease() { tcRelease(); }

void typingClickPlay(int count) {
    if (!enabled()) return;
    if (count < 1) count = 1;
    if (count > TC_MAX_N) count = TC_MAX_N;

    int64_t now = esp_timer_get_time();
    if (s_ready && now - s_last_play_us > TC_IDLE_US) tcRelease();  // 闲置自停
    if (!s_ready && !tcInit()) return;

    std::string timbre = g_settings.typingClickTimbre();
    if (timbre != s_timbre_cache) renderClick(timbre);  // 音色改了 → 重合成

    int vol = g_settings.typingClickVolume();
    if (vol <= 0) { s_last_play_us = now; return; }  // 静音档
    if (vol != s_vol_cache) {
        esp_codec_dev_set_out_vol(s_dev, vol);
        s_vol_cache = vol;
    }

    if (count == 1) {
        esp_codec_dev_write(s_dev, s_click.data(), (int)(s_click.size() * sizeof(int16_t)));
    } else {
        int clickSamples = (int)s_click.size() / TC_STEREO;
        int gapSamples = TC_GAP_MS * TC_SAMPLE_RATE / 1000;
        int per = clickSamples + gapSamples;
        std::vector<int16_t> pcm;
        pcm.resize((size_t)count * per * TC_STEREO, 0);
        for (int k = 0; k < count; k++) {
            int16_t *dst = pcm.data() + (size_t)k * per * TC_STEREO;
            for (int i = 0; i < clickSamples; i++) {
                int16_t v = s_click[i * 2];
                dst[i * 2] = v;
                dst[i * 2 + 1] = v;
            }
        }
        esp_codec_dev_write(s_dev, pcm.data(), (int)(pcm.size() * sizeof(int16_t)));
    }
    s_last_play_us = now;
}
