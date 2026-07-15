#include "screen_bt_manage.h"
#include "bt_keyboard.h"
#include "ui_helpers.h"
#include <cstdio>
#include <esp_timer.h>

// ── BT manage state ───────────────────────────────────────────────────────
static struct {
    int selection = 0;
    int scroll = 0;
    bool scanning = true;
    bool connecting = false;
    int64_t conn_start_ms = 0;
    char statusMsg[64];
} g_btState;

// ── Screen entry points ──────────────────────────────────────────────────
void screen_bt_manage_init() {
    g_btState.selection = 0;
    g_btState.scroll = 0;
    g_btState.scanning = true;
    g_btState.connecting = false;
    g_btState.conn_start_ms = 0;
    g_btState.statusMsg[0] = '\0';
    g_bt.scanDevices();
}

AppState screen_bt_manage_handle(int key, ScreenContext &ctx) {
    if (g_btState.scanning && !g_bt.isScanning()) {
        g_btState.scanning = false;
    }

    if (g_bt.isConnected()) {
        ctx.nextState = APP_MAIN;
        return APP_MAIN;
    }

    const int64_t CONN_TIMEOUT_MS = 5000;
    if (g_btState.connecting && g_btState.conn_start_ms > 0) {
        int64_t elapsed = (esp_timer_get_time() / 1000) - g_btState.conn_start_ms;
        if (elapsed > CONN_TIMEOUT_MS) {
            g_btState.connecting = false;
            g_btState.statusMsg[0] = '\0';
        }
    }

    if (key == 'q' || key == 'Q' || key == 0x1B) {
        if (g_bt.isScanning()) return APP_BT_MANAGE;
        if (g_btState.connecting) {
            g_btState.connecting = false;
            g_bt.disconnect();
        }
        ctx.nextState = APP_MAIN; return APP_MAIN;
    }
    if (g_btState.scanning || g_btState.connecting) {
        // Don't navigate while scanning/connecting
    } else if (key == KEY_UP) {
        if (g_btState.selection > 0) g_btState.selection--;
    } else if (key == KEY_DOWN) {
        int n = g_bt.deviceCount();
        if (g_btState.selection < n - 1) g_btState.selection++;
    } else if (key == 0x0A || key == 0x0D) {
        int n = g_bt.deviceCount();
        if (n > 0 && g_btState.selection < n) {
            g_btState.connecting = true;
            g_btState.conn_start_ms = esp_timer_get_time() / 1000;
            snprintf(g_btState.statusMsg, sizeof(g_btState.statusMsg),
                     "正在连接 %s...", g_bt.getDevice(g_btState.selection)->name);
            g_bt.connectDevice(g_btState.selection);
        }
    }

    ui_clear(); int y = 28;
    ui_draw_text_centered(y, "蓝牙键盘管理", false, true); y += FONT_H;

    if (g_btState.scanning) {
        ui_draw_text_centered(y, "正在扫描蓝牙键盘..."); y += FONT_H;
        if (y + FONT_H <= SCREEN_H)
            ui_draw_text_centered(y, "请确保键盘处于配对模式");
    } else {
        if (g_btState.connecting) {
            snprintf(g_btState.statusMsg, sizeof(g_btState.statusMsg),
                     "正在连接 %s...", g_bt.getDevice(g_btState.selection)->name);
        }
        if (g_btState.statusMsg[0]) {
            ui_draw_text_centered(y, g_btState.statusMsg, true); y += FONT_H;
        }

        int n = g_bt.deviceCount();
        if (n == 0) {
            ui_draw_text_centered(y, "未找到蓝牙键盘"); y += FONT_H;
            ui_draw_text_centered(y, "双击 USER 按钮返回后重试");
        } else {
            int visible = (SCREEN_H - y + FONT_H - 1) / FONT_H;
            if (g_btState.selection < g_btState.scroll) g_btState.scroll = g_btState.selection;
            if (g_btState.selection >= g_btState.scroll + visible)
                g_btState.scroll = g_btState.selection - visible + 1;

            for (int i = 0; i < visible && (g_btState.scroll + i) < n; i++) {
                int idx = g_btState.scroll + i;
                auto *dev = g_bt.getDevice(idx);
                bool sel = (idx == g_btState.selection);
                char buf[48];
                snprintf(buf, sizeof(buf), "  %s", dev->name);
                ui_draw_text(8, y + i * FONT_H, buf, sel);
            }
        }
    }

    ui_commit();
    return APP_BT_MANAGE;
}
