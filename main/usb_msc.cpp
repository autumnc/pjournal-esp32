/*
 * U盘模式:把 SD 卡整卡经 TinyUSB MSC 暴露给电脑, 设备侧不再访问 SD。
 *
 * 与 pico 固件 main/usb_msc.cpp 的差别只有 UI 部分:那边在一个阻塞循环里轮询
 * 触摸/按键, 这里由 APP_USB_MSC 屏幕轮询。SD 生命周期、TinyUSB 配置、拆除顺序
 * 与 FSLS PHY 恢复都保持一致。
 */

#include "usb_msc.h"

#include "bt_keyboard.h"
#include "file_manager_server.h"
#include "ime/IME.h"
#include "journal_storage.h"
#include "pjournal_app.h"

#include <cstdlib>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/sdmmc_default_configs.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

// 退出时要把共用的 FSLS PHY 拨回 USB-Serial-JTAG。
#include <soc/soc_caps.h>
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include <hal/usb_serial_jtag_ll.h>
#endif

static const char *TAG = "usb_msc";

// SD 卡引脚与 journal_storage.cpp 一致:CLK=38, CMD=21, D0=39, 1-bit。
#define SDMMC_CLK GPIO_NUM_38
#define SDMMC_CMD GPIO_NUM_21
#define SDMMC_D0  GPIO_NUM_39

static volatile bool s_active = false;       // 裸卡被 MSC 占用(唯一的跨模块守卫)
static bool s_usb_installed = false;
static volatile bool s_host_hold = false;    // 电脑仍占用介质(未安全弹出)
static sdmmc_card_t *s_card = nullptr;
static tinyusb_msc_storage_handle_t s_storage = nullptr;
static uint64_t s_capacity_bytes = 0;

// base_path 是 char*(非 const), 必须给可写缓冲; mount_point=USB 时设备侧不会
// 真的挂到这个路径, 写上只为与应用的 /sdcard 约定一致。
static char s_msc_base_path[] = "/sdcard";

// ── 主机占用检测 ─────────────────────────────────────────────────────────
// esp_tinyusb 已经强实现 start_stop_cb(弹出时把卡挂回 APP), 不能 override 它;
// PREVENT/ALLOW(Windows 挂载/安全删除)和 START STOP UNIT(macOS/Linux 弹出)
// 都落到 tud_msc_scsi_complete_cb 的 default 分支, override 这一个弱回调即可
// 同时拿到两路信号。
extern "C" void tud_msc_scsi_complete_cb(uint8_t lun, uint8_t const scsi_cmd[16]) {
    (void)lun;
    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL: {
            const scsi_prevent_allow_medium_removal_t *cmd =
                (const scsi_prevent_allow_medium_removal_t *)scsi_cmd;
            s_host_hold = (cmd->prohibit_removal != 0);
            break;
        }
        case SCSI_CMD_START_STOP_UNIT: {
            const scsi_start_stop_unit_t *cmd =
                (const scsi_start_stop_unit_t *)scsi_cmd;
            if (cmd->load_eject && !cmd->start) s_host_hold = false;  // 主机弹出介质
            break;
        }
        default:
            break;
    }
}

// ── 裸卡初始化/拆除(不挂 FATFS) ─────────────────────────────────────────

static esp_err_t rawSdInit(sdmmc_card_t **out_card) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();  // 20MHz, 与应用挂载一致

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk   = SDMMC_CLK;
    slot.cmd   = SDMMC_CMD;
    slot.d0    = SDMMC_D0;
    slot.d1    = GPIO_NUM_NC;
    slot.d2    = GPIO_NUM_NC;
    slot.d3    = GPIO_NUM_NC;
    slot.cd    = GPIO_NUM_NC;
    slot.wp    = GPIO_NUM_NC;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (card == nullptr) return ESP_ERR_NO_MEM;

    esp_err_t ret = host.init();
    if (ret != ESP_OK) {
        free(card);
        return ret;
    }
    ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret != ESP_OK) {
        host.deinit_p(host.slot);
        free(card);
        return ret;
    }

    // 上电后首次时钟协商偶尔超时。给 ~3s 窗口, 仍失败就按"无卡/卡坏"回滚,
    // 不能无限重试把整机按死在 U 盘页。
    bool ok = false;
    for (int i = 0; i < 30; i++) {
        if (sdmmc_card_init(&host, card) == ESP_OK) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!ok) {
        host.deinit_p(host.slot);
        free(card);
        return ESP_ERR_NOT_FOUND;
    }

    *out_card = card;
    return ESP_OK;
}

// TinyUSB 的 storage_sdmmc_close 只清内部指针, 不碰 host/卡内存, 这里自己收。
static void rawSdTeardown() {
    if (s_card == nullptr) return;
    sdmmc_host_t *host = &s_card->host;
    if (host->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        host->deinit_p(host->slot);
    } else if (host->deinit) {
        host->deinit();
    }
    free(s_card);
    s_card = nullptr;
}

// 把共用的 FSLS PHY 交还给 USB-Serial-JTAG: 装 OTG 时 mux 被拨到 OTG 侧,
// 卸载路径不会自动拨回来, 不补这两步串口/刷机口会一直消失到复位。
// 安装失败时也调用一次(防御性: PHY 可能已切换)。
static void restoreUsjPhy() {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
#endif
}

static bool remountJournal() {
    for (int i = 0; i < 3; i++) {
        if (g_journal.begin()) return true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

// ── 进入/退出 ────────────────────────────────────────────────────────────

usb_msc_result_t usbMscEnter() {
    if (s_active) return USB_MSC_ERR_ALREADY_ACTIVE;
    if (!g_journal.isMounted()) return USB_MSC_ERR_NOT_MOUNTED;

    // 词库异步写盘必须先排空: 卸载之后写盘任务再碰 SD 会写进已释放的 FATFS。
    IME &ime = IME::getInstance();
    ime.flushUserDictSavesNow();
    if (!ime.waitUserDictWritesDrained(10000) || ime.hasPendingUserDictWrites()) {
        ESP_LOGE(TAG, "user dict writes still pending, refuse to unmount SD");
        return USB_MSC_ERR_TEARDOWN;
    }

    // 文件管理 HTTP 服务可能还开着(它有自己的生命周期), 绝不能让它继续读卡。
    file_manager_server_stop();

    // 先置位再卸载: 其它任务在拿到 SD 锁后还会复查这个标志。
    s_active = true;
    s_host_hold = false;

    esp_err_t err = g_journal.deinit();
    if (err != ESP_OK) {
        s_active = false;
        if (!g_journal.isMounted() && !remountJournal()) return USB_MSC_ERR_REMOUNT;
        return USB_MSC_ERR_UNMOUNT;
    }

    err = rawSdInit(&s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "raw SD init failed: %s", esp_err_to_name(err));
        s_active = false;
        if (!remountJournal()) return USB_MSC_ERR_REMOUNT;
        return USB_MSC_ERR_NO_CARD;
    }

    tinyusb_msc_storage_config_t cfg = {};
    cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    cfg.medium.card = s_card;
    cfg.fat_fs.base_path = s_msc_base_path;
    cfg.fat_fs.config.max_files = 5;
    cfg.fat_fs.config.format_if_mount_failed = false;
    cfg.fat_fs.do_not_format = true;
    cfg.fat_fs.format_flags = 0;

    err = tinyusb_msc_new_storage_sdmmc(&cfg, &s_storage);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage create failed: %s", esp_err_to_name(err));
        if (s_storage) {
            tinyusb_msc_delete_storage(s_storage);
            s_storage = nullptr;
        }
        rawSdTeardown();
        s_active = false;
        if (!remountJournal()) return USB_MSC_ERR_REMOUNT;
        return USB_MSC_ERR_STORAGE;
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb install failed: %s", esp_err_to_name(err));
        restoreUsjPhy();
        if (s_storage) {
            tinyusb_msc_delete_storage(s_storage);
            s_storage = nullptr;
        }
        rawSdTeardown();
        s_active = false;
        if (!remountJournal()) return USB_MSC_ERR_REMOUNT;
        return USB_MSC_ERR_USB;
    }

    s_usb_installed = true;

    uint32_t sectors = 0;
    if (tinyusb_msc_get_storage_capacity(s_storage, &sectors) == ESP_OK) {
        s_capacity_bytes = (uint64_t)sectors * 512;
    }
    ESP_LOGI(TAG, "U-disk mode active, %llu MB",
             (unsigned long long)(s_capacity_bytes / (1024 * 1024)));
    return USB_MSC_OK;
}

usb_msc_result_t usbMscExit() {
    if (!s_active) return USB_MSC_OK;

    // 电脑还占着介质就什么都不拆。拔线后 tud_mounted()==false, 可以直接退。
    if (s_usb_installed && tud_mounted() && s_host_hold) {
        return USB_MSC_ERR_HOST_HOLD;
    }

    if (s_usb_installed) {
        esp_err_t err = tinyusb_driver_uninstall();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb uninstall failed: %s", esp_err_to_name(err));
            return USB_MSC_ERR_TEARDOWN;  // 保持 s_active, 允许再按一次退出
        }
        s_usb_installed = false;
    }

    restoreUsjPhy();

    if (s_storage) {
        // 拆除顺序不能反: 先停 USB 栈, 再删存储。此时 TinyUSB 任务已停,
        // 即便仍有 deferred write 也只会返回错误, 不会和主机的读写撞车。
        esp_err_t err = tinyusb_msc_delete_storage(s_storage);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MSC storage delete: %s (continuing teardown)",
                     esp_err_to_name(err));
        }
        s_storage = nullptr;
    }

    rawSdTeardown();

    bool remounted;
    {
        SemaphoreHandle_t m = g_journal.sdMutex();
        if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
        remounted = remountJournal();
        if (m) xSemaphoreGiveRecursive(m);
    }
    if (!remounted) {
        // s_active 保持 true: 应用仍然不能碰 SD, 再按 ESC 只会重试重挂。
        ESP_LOGE(TAG, "SD remount failed after U-disk exit");
        return USB_MSC_ERR_REMOUNT;
    }

    s_active = false;
    s_host_hold = false;
    s_capacity_bytes = 0;
    g_bt.flushKeys();
    ESP_LOGI(TAG, "U-disk mode exited, SD remounted");
    return USB_MSC_OK;
}

bool usbMscActive() { return s_active; }

bool usbMscHostMounted() { return s_usb_installed && tud_mounted(); }

bool usbMscHostHold() { return s_active && s_host_hold; }

uint64_t usbMscCardCapacityBytes() { return s_active ? s_capacity_bytes : 0; }

const char *usbMscResultText(usb_msc_result_t r) {
    switch (r) {
        case USB_MSC_OK:                 return "U盘模式已就绪";
        case USB_MSC_ERR_ALREADY_ACTIVE: return "U盘模式已经开启";
        case USB_MSC_ERR_NOT_MOUNTED:    return "SD卡未挂载";
        case USB_MSC_ERR_UNMOUNT:        return "SD卡卸载失败";
        case USB_MSC_ERR_NO_CARD:        return "未检测到SD卡";
        case USB_MSC_ERR_STORAGE:        return "U盘存储初始化失败";
        case USB_MSC_ERR_USB:            return "USB 设备启动失败";
        case USB_MSC_ERR_HOST_HOLD:      return "电脑仍占用U盘，请先安全弹出";
        case USB_MSC_ERR_TEARDOWN:       return "设备忙，请稍后再试";
        case USB_MSC_ERR_REMOUNT:        return "SD卡重新挂载失败，请重启设备";
        default:                         return "U盘模式出错";
    }
}
