#include "screen_usb_msc.h"

#include "font_renderer.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "usb_msc.h"

#include <cstdio>
#include <esp_log.h>
#include <esp_timer.h>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
}

static enum class Phase {
    ACTIVE,
    HOLD_WARN,     // 退出被拒:电脑还占着
    EXIT_ERR,      // 其它退出失败(设备忙 / 词库仍在写盘)
    REMOUNT_ERR,   // 退出后 SD 重挂失败
} s_phase = Phase::ACTIVE;

static const char *s_exitErrText = nullptr;

static struct {
    bool mounted = false;
    bool hold = false;
    Phase phase = Phase::ACTIVE;
    bool drawn = false;
    int64_t warnUntilUs = 0;
} s_ui;

static const int64_t WARN_SHOW_US = 3 * 1000000LL;

static void drawMainPage() {
    bool mounted = usbMscHostMounted();
    bool hold = usbMscHostHold();

    ui_clear();
    int y = 40;
    ui_draw_text_centered(y, "U盘模式", false, true);
    y += FONT_H;
    u8g2_DrawHLine(g_u8g2, 0, y, SCREEN_W);
    y += 24;

    const char *state;
    if (s_phase == Phase::HOLD_WARN) {
        state = "电脑仍占用U盘";
    } else if (s_phase == Phase::EXIT_ERR) {
        state = "无法退出U盘模式";
    } else if (!mounted) {
        state = "等待电脑连接...";
    } else if (hold) {
        state = "电脑占用中";
    } else {
        state = "已连接电脑";
    }
    ui_draw_text_centered(y, state, false, true);
    y += FONT_H + 6;

    uint64_t cap = usbMscCardCapacityBytes();
    if (cap > 0) {
        char line[48];
        uint64_t mb = cap / (1024 * 1024);
        if (mb >= 1024) {
            uint64_t tenths = mb * 10 / 1024;  // 7.9 GB 这样的精度
            snprintf(line, sizeof(line), "容量: %llu.%llu GB",
                     (unsigned long long)(tenths / 10),
                     (unsigned long long)(tenths % 10));
        } else {
            snprintf(line, sizeof(line), "容量: %llu MB", (unsigned long long)mb);
        }
        ui_draw_text_centered(y, line);
        y += FONT_H;
    }

    y += 8;
    ui_draw_text_centered(y, "电脑上可管理 SD 卡文件");
    y += FONT_H;
    if (s_phase == Phase::HOLD_WARN) {
        ui_draw_text_centered(y, "请先在电脑上安全弹出");
    } else if (s_phase == Phase::EXIT_ERR) {
        ui_draw_text_centered(y, s_exitErrText ? s_exitErrText : "请再按一次 ESC 重试");
    } else {
        ui_draw_text_centered(y, "退出前请先在电脑上安全弹出");
    }
    y += FONT_H;
    ui_draw_text_centered(y, "ESC / 长按BOOT 退出");

    ui_draw_status("U盘模式", battery_text().c_str());
    ui_commit();
}

static void drawRemountError() {
    ui_clear();
    ui_draw_text_centered(SCREEN_H / 2 - FONT_H, "SD卡重新挂载失败", false, true);
    ui_draw_text_centered(SCREEN_H / 2, "按 ESC 重试，或重启设备");
    ui_draw_status("U盘模式", "");
    ui_commit();
}

static void drawCurrent() {
    if (s_phase == Phase::REMOUNT_ERR) {
        drawRemountError();
    } else {
        drawMainPage();
    }
    s_ui.mounted = usbMscHostMounted();
    s_ui.hold = usbMscHostHold();
    s_ui.phase = s_phase;
    s_ui.drawn = true;
}

AppState screen_usb_msc_init(ScreenContext &ctx) {
    s_phase = Phase::ACTIVE;
    s_ui = {};

    g_font.setSize(22);
    ui_clear();
    ui_draw_text_centered(SCREEN_H / 2 - FONT_H, "正在进入U盘模式...", false, true);
    ui_draw_text_centered(SCREEN_H / 2, "请勿在读写时拔出USB");
    ui_commit();

    usb_msc_result_t r = usbMscEnter();
    if (r != USB_MSC_OK) {
        ESP_LOGW("usb_msc_ui", "enter failed: %s", usbMscResultText(r));
        ctx.statusMessage = usbMscResultText(r);
        ctx.statusDuration = 30;
        return APP_SETTINGS;
    }

    drawCurrent();
    return APP_USB_MSC;
}

AppState screen_usb_msc_handle(int key, ScreenContext &ctx) {
    int64_t now = esp_timer_get_time();

    if (key == 0x1B) {  // ESC / BOOT 长按
        if (usbMscHostMounted() && usbMscHostHold()) {
            s_phase = Phase::HOLD_WARN;
            s_ui.warnUntilUs = now + WARN_SHOW_US;
            drawCurrent();
            return APP_USB_MSC;
        }

        ui_clear();
        ui_draw_text_centered(SCREEN_H / 2 - FONT_H / 2, "正在退出U盘模式...");
        ui_commit();

        usb_msc_result_t r = usbMscExit();
        if (r == USB_MSC_OK) {
            // 电脑可能改过设置文件, 清掉内存缓存, 回到设置时重新读盘。
            g_settings.invalidateCache();
            ctx.statusMessage = "已退出U盘模式";
            ctx.statusDuration = 20;
            return APP_SETTINGS;
        }

        if (r == USB_MSC_ERR_HOST_HOLD) {
            s_phase = Phase::HOLD_WARN;
            s_ui.warnUntilUs = now + WARN_SHOW_US;
        } else if (r == USB_MSC_ERR_REMOUNT) {
            s_phase = Phase::REMOUNT_ERR;
        } else {
            s_phase = Phase::EXIT_ERR;
            s_exitErrText = usbMscResultText(r);
            s_ui.warnUntilUs = now + WARN_SHOW_US;
        }
        drawCurrent();
        return APP_USB_MSC;
    }

    // 警告只在屏幕上停几秒; 到点回到正常页面。
    if ((s_phase == Phase::HOLD_WARN || s_phase == Phase::EXIT_ERR) &&
        s_ui.warnUntilUs != 0 && now > s_ui.warnUntilUs) {
        s_phase = Phase::ACTIVE;
        s_ui.warnUntilUs = 0;
    }

    bool mounted = usbMscHostMounted();
    bool hold = usbMscHostHold();
    if (!s_ui.drawn || mounted != s_ui.mounted || hold != s_ui.hold ||
        s_phase != s_ui.phase) {
        drawCurrent();
    }

    return APP_USB_MSC;
}
