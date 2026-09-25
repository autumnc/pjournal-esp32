#include "typing_click.h"
#include "click_samples.h"
#include "settings_manager.h"
#include "pcf85063.h"  // pjournal_get_i2c_bus()
#include "user_config.h"
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cmath>
#include <string>
#include <vector>

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>  // es8311_codec.h, i2c/i2s/gpio factories

#define TAG "TypeClick"

// 采样是 48k 的(见 click_samples_data.inc),这里就不再重采样到 16k:
// 轴体的「脆」全在 8k 以上的高频,降采样会把 18 套音色糊成一团。
// 打字音走自己独立的 I2S 通道与 ES8311 实例,和 16k 的 voice_input 互不影响
// (ES8311 在 12.288MHz MCLK / 48k 有精确匹配的系数)。
#define TC_SAMPLE_RATE  48000
#define TC_STEREO       2
#define TC_IDLE_MS      (30 * 1000)  // 闲置 30s 自动拆音频
#define TC_MAX_N        8            // 待播声数上限(防异常长文本把声音拖远)
#define TC_GAP_MS       30           // 多声之间的基准静音间隔
#define TC_RELEASE_MS   3000         // release 等任务拆完音频的上限

static i2s_chan_handle_t s_tx = nullptr;
static const audio_codec_data_if_t *s_data = nullptr;
static const audio_codec_ctrl_if_t *s_ctrl = nullptr;
static const audio_codec_gpio_if_t *s_gpio = nullptr;
static const audio_codec_if_t *s_codec = nullptr;
static esp_codec_dev_handle_t s_dev = nullptr;
static bool s_ready = false;
static int s_vol_cache = -1;

static QueueHandle_t s_q = nullptr;         // 每项 = 一次 play 请求要连响几声;0 = 拆音频
static TaskHandle_t s_task = nullptr;
static SemaphoreHandle_t s_closed = nullptr;  // 任务拆完音频后 give
static volatile bool s_abort = false;       // 请求中断当前连发
static portMUX_TYPE s_pend_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_pending = 0;                   // 已排队待播的声数

static bool enabled() {
    return g_settings.inputMode() == "typewriter" && g_settings.typingClickEnabled();
}

static double rand01() { return (double)esp_random() / 4294967296.0; }
static int clamp16(long v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : (int)v; }

// 一声敲击:随机挑一个 take,按随机步长重采样(步长>1 读得快=音变高),再叠上
// 这次的音量与左右微差。三个随机量合起来,连着敲才不像一串复读。
static void makeHit(const ClickSet &set, std::vector<int16_t> &out) {
    const ClickTake &tk = set.takes[esp_random() % (uint32_t)set.n];
    const double step = 1.0 + (rand01() * 2.0 - 1.0) * set.detune_pct / 100.0;
    const double gain = set.gain_pct / 100.0 * (0.92 + 0.16 * rand01());
    const double pan = (rand01() * 2.0 - 1.0) * 0.12;
    const double gl = gain * (1.0 - pan * 0.5), gr = gain * (1.0 + pan * 0.5);

    const int n = (int)(tk.frames / step);
    out.resize((size_t)(n > 0 ? n : 1) * TC_STEREO);
    for (int i = 0; i < n; i++) {
        const double pos = i * step;
        const int i0 = (int)pos;
        const int i1 = i0 + 1 < tk.frames ? i0 + 1 : tk.frames - 1;
        const double f = pos - i0;
        const double v = tk.pcm[i0] * (1.0 - f) + tk.pcm[i1] * f;
        out[(size_t)i * TC_STEREO] = (int16_t)clamp16(lround(v * gl));
        out[(size_t)i * TC_STEREO + 1] = (int16_t)clamp16(lround(v * gr));
    }
}

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

    int vol = g_settings.typingClickVolume();
    if (vol > 0) esp_codec_dev_set_out_vol(s_dev, vol);
    s_vol_cache = vol;
    s_ready = true;
    ESP_LOGI(TAG, "ES8311 DAC ready @%dHz (vol=%d%%)", TC_SAMPLE_RATE, vol);
    return true;
}

// 连响 count 声。每声都重新合成,所以「上屏按字数」触发的连着几下也是不同的声音。
static void playBatch(int count) {
    const ClickSet *set = click_sample_set(g_settings.typingClickTimbre().c_str());
    if (set == nullptr) return;

    const int vol = g_settings.typingClickVolume();
    if (vol <= 0) return;  // 静音档
    if (vol != s_vol_cache) {
        esp_codec_dev_set_out_vol(s_dev, vol);
        s_vol_cache = vol;
    }

    std::vector<int16_t> hit;  // 一声(立体声交错)
    std::vector<int16_t> gap;  // 声与声之间的静音
    for (int k = 0; k < count && !s_abort; k++) {
        makeHit(*set, hit);
        esp_codec_dev_write(s_dev, hit.data(), (int)(hit.size() * sizeof(int16_t)));
        if (k + 1 == count) break;
        // 间隔也抖一点:固定的 30ms 听起来像节拍器
        const int n = (int)(TC_GAP_MS * (0.8 + 0.45 * rand01())) * TC_SAMPLE_RATE / 1000;
        gap.assign((size_t)(n > 0 ? n : 1) * TC_STEREO, 0);
        esp_codec_dev_write(s_dev, gap.data(), (int)(gap.size() * sizeof(int16_t)));
    }
}

// 常驻线程:play() 只入队就返回,渲染和写 I2S 都在这里,UI 线程不会被 DMA 阻塞。
// 没活干就等着,闲置超过 TC_IDLE_MS 就把设备关掉(别一直占着 ES8311 与控制器 0)。
static void clickTask(void *) {
    for (;;) {
        int n = 0;
        if (xQueueReceive(s_q, &n, pdMS_TO_TICKS(TC_IDLE_MS)) != pdTRUE) {
            if (s_ready) { tcRelease(); xSemaphoreGive(s_closed); }
            continue;
        }
        if (n <= 0) {  // 拆音频请求
            tcRelease();
            xSemaphoreGive(s_closed);
            continue;
        }
        portENTER_CRITICAL(&s_pend_mux);
        s_pending -= n;
        if (s_pending < 0) s_pending = 0;
        portEXIT_CRITICAL(&s_pend_mux);

        s_abort = false;  // 放在 tcInit 之前:期间来的 release 才不会被这次覆盖掉
        if (tcInit()) playBatch(n);
    }
}

// 首次发声时才建队列与播放线程(和 ES8311 一样懒加载:不打字就不占资源)。
static bool ensureTask() {
    if (s_task != nullptr) return true;
    if (s_q == nullptr) s_q = xQueueCreate(TC_MAX_N, sizeof(int));
    if (s_closed == nullptr) s_closed = xSemaphoreCreateBinary();
    if (s_q == nullptr || s_closed == nullptr) {
        ESP_LOGE(TAG, "click queue alloc failed");
        return false;
    }
    if (xTaskCreate(clickTask, "click", 6144, nullptr, 2, &s_task) != pdPASS) {
        s_task = nullptr;
        ESP_LOGE(TAG, "click task create failed");
        return false;
    }
    return true;
}

void typingClickRelease() {
    if (s_task == nullptr) return;
    s_abort = true;  // 让任务尽快收尾当前连发
    xQueueReset(s_q);
    portENTER_CRITICAL(&s_pend_mux);
    s_pending = 0;
    portEXIT_CRITICAL(&s_pend_mux);
    xSemaphoreTake(s_closed, 0);  // 清掉闲置自停留下的陈旧令牌

    const int zero = 0;
    if (xQueueSend(s_q, &zero, 0) != pdTRUE) return;
    // 等到真正拆完再返回:调用方(voice_input / 切模式)紧接着要独占控制器 0
    if (xSemaphoreTake(s_closed, pdMS_TO_TICKS(TC_RELEASE_MS)) != pdTRUE)
        ESP_LOGW(TAG, "click task did not close in %dms", TC_RELEASE_MS);
}

void typingClickPlay(int count) {
    if (!enabled()) return;
    if (count < 1) count = 1;
    if (count > TC_MAX_N) count = TC_MAX_N;
    if (!ensureTask()) return;

    portENTER_CRITICAL(&s_pend_mux);
    const bool room = s_pending + count <= TC_MAX_N;
    if (room) s_pending += count;
    portEXIT_CRITICAL(&s_pend_mux);
    if (!room) return;  // 已经排满:丢掉这次,别让声音越拖越远

    if (xQueueSend(s_q, &count, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_pend_mux);
        s_pending -= count;
        if (s_pending < 0) s_pending = 0;
        portEXIT_CRITICAL(&s_pend_mux);
    }
}
