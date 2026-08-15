#include "screen_bt_manage.h"
#include "bt_keyboard.h"
#include "ui_helpers.h"
#include "quick_edit.h"
#include <cstdio>
#include <esp_timer.h>

extern u8g2_t *g_u8g2;

extern "C" {
    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);
}

// ── BT manage state ───────────────────────────────────────────────────────
enum BtMode { BT_MANAGE, BT_SCAN };

static struct {
    BtMode mode = BT_MANAGE;    // 默认进入已配对设备管理
    int selection = 0;
    int scroll = 0;
    bool scanning = false;
    bool connecting = false;
    int64_t conn_start_ms = 0;
    char statusMsg[64];
    bool showHelp = false;
    int helpScroll = 0;
} g_btState;

bool screen_bt_manage_scan_mode() { return g_btState.mode == BT_SCAN; }

// ── Screen entry points ──────────────────────────────────────────────────
void screen_bt_manage_init() {
    g_btState.mode = BT_MANAGE;
    g_btState.selection = 0;
    g_btState.scroll = 0;
    g_btState.scanning = false;
    g_btState.connecting = false;
    g_btState.conn_start_ms = 0;
    g_btState.statusMsg[0] = '\0';
    g_btState.showHelp = false;
    g_btState.helpScroll = 0;
    g_bt.loadPairedDevices();
}

static void drawManage() {
    ui_clear();
    ui_draw_text_centered(g_font.ascent(), "蓝牙设备管理", false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);
    int y = FONT_H + 8 + LINE_SPACING;

    if (g_btState.connecting) {
        ui_draw_text_centered(y, g_btState.statusMsg, true); y += FONT_H;
    }

    int n = g_bt.pairedDeviceCount();
    int connIdx = g_bt.connectedPairedIndex();
    if (n == 0) {
        ui_draw_text_centered(y, "暂无已配对设备"); y += FONT_H;
        ui_draw_text_centered(y, "按 a 扫描添加"); y += FONT_H;
    } else {
        int visible = (STATUS_Y - y + FONT_H - 1) / FONT_H;
        if (g_btState.selection < g_btState.scroll) g_btState.scroll = g_btState.selection;
        if (g_btState.selection >= g_btState.scroll + visible)
            g_btState.scroll = g_btState.selection - visible + 1;

        for (int i = 0; i < visible && (g_btState.scroll + i) < n; i++) {
            int idx = g_btState.scroll + i;
            const BtPairedDevice *p = g_bt.getPairedDevice(idx);
            char buf[48];
            if (idx == connIdx)
                snprintf(buf, sizeof(buf), "\xe2\x97\x8f %s", p->name);  // ●
            else
                snprintf(buf, sizeof(buf), "  %s", p->name);
            bool sel = (idx == g_btState.selection);
            ui_draw_text(8, y + i * FONT_H, buf, sel);
        }
    }

    ui_draw_status("a:添加 d:删除 Enter:连接", "?:帮助");
    ui_commit();
}

static void drawScan() {
    ui_clear();
    ui_draw_text_centered(g_font.ascent(), "扫描添加设备", false, true);
    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);
    int y = FONT_H + 8 + LINE_SPACING;

    if (g_btState.scanning) {
        ui_draw_text_centered(y, "正在扫描蓝牙键盘..."); y += FONT_H;
        if (y + FONT_H <= SCREEN_H)
            ui_draw_text_centered(y, "请确保键盘处于配对模式");
    } else {
        if (g_btState.connecting) {
            ui_draw_text_centered(y, g_btState.statusMsg, true); y += FONT_H;
        }
        int n = g_bt.deviceCount();
        if (n == 0) {
            ui_draw_text_centered(y, "未找到蓝牙键盘"); y += FONT_H;
            ui_draw_text_centered(y, "Esc 返回后重试");
        } else {
            int visible = (STATUS_Y - y + FONT_H - 1) / FONT_H;
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

    ui_draw_status("Enter:连接 Esc:返回", "?:帮助");
    ui_commit();
}

// ── 快捷键帮助对话框 ─────────────────────────────────────────────────────
static const char *BT_HELP_LINES[] = {
    "── 已配对设备管理 ──",
    "a      扫描添加设备",
    "d      删除选中配对",
    "Enter  连接选中设备",
    "?      帮助",
    "q/Esc  返回",
    "",
    "── 扫描添加设备 ──",
    "Enter  连接选中设备",
    "q/Esc  返回",
    "",
    "── 物理按键 ──",
    "USER 短按 上移",
    "BOOT 短按 下移",
    "USER 双击 添加设备",
    "BOOT 双击 删除配对",
    "USER 长按 连接",
    "BOOT 长按 Esc退出",
};
static const int BT_HELP_LINE_COUNT = sizeof(BT_HELP_LINES) / sizeof(BT_HELP_LINES[0]);

static void drawHelp() {
    ui_clear();
    ui_draw_text_centered(28, "快捷键帮助", false, true);
    u8g2_DrawHLine(g_u8g2, 0, 28 + g_font.descent() + 4, SCREEN_W);
    int contentY = 28 + g_font.descent() + 12;
    int contentMaxY = STATUS_Y;
    int maxVis = (contentMaxY - contentY) / LINE_SPACING;
    if (maxVis < 1) maxVis = 1;
    int maxScroll = BT_HELP_LINE_COUNT - maxVis;
    if (maxScroll < 0) maxScroll = 0;
    if (g_btState.helpScroll > maxScroll) g_btState.helpScroll = maxScroll;
    if (g_btState.helpScroll < 0) g_btState.helpScroll = 0;
    for (int i = 0; i < maxVis && (g_btState.helpScroll + i) < BT_HELP_LINE_COUNT; i++) {
        int ly = contentY + i * LINE_SPACING;
        const char *line = BT_HELP_LINES[g_btState.helpScroll + i];
        bool isHeader = ((unsigned char)line[0] == 0xE2);
        ui_draw_text(12, ly + g_font.ascent(), line, false, isHeader);
    }
    ui_draw_status("Esc返回", "");
    ui_commit();
}

AppState screen_bt_manage_handle(int key, ScreenContext &ctx) {
    // 扫描完成跟踪
    if (g_btState.scanning && !g_bt.isScanning()) {
        g_btState.scanning = false;
        g_btState.selection = 0;
        g_btState.scroll = 0;
    }

    // 连接成功或超时清除"连接中"状态
    const int64_t CONN_TIMEOUT_MS = 8000;
    if (g_btState.connecting && g_btState.conn_start_ms > 0) {
        if (g_bt.isConnected()) {
            g_btState.connecting = false;
            g_btState.conn_start_ms = 0;
            g_btState.statusMsg[0] = '\0';
        } else {
            int64_t elapsed = (esp_timer_get_time() / 1000) - g_btState.conn_start_ms;
            if (elapsed > CONN_TIMEOUT_MS) {
                g_btState.connecting = false;
                g_btState.conn_start_ms = 0;
                g_btState.statusMsg[0] = '\0';
            }
        }
    }

    // ── 快捷键帮助对话框 ──
    if (g_btState.showHelp) {
        if (key == KEY_UP) {
            if (g_btState.helpScroll > 0) g_btState.helpScroll--;
        } else if (key == KEY_DOWN) {
            g_btState.helpScroll++;
        } else if (key == 0x1B || key == 'q' || key == 'Q' || key == '?') {
            g_btState.showHelp = false;
        }
        drawHelp();
        return APP_BT_MANAGE;
    }

    // ── 扫描模式(添加设备) ──
    if (g_btState.mode == BT_SCAN) {
        if (key == 'q' || key == 'Q' || key == 0x1B) {
            g_btState.mode = BT_MANAGE;
            g_btState.connecting = false;
            g_btState.statusMsg[0] = '\0';
            g_btState.selection = 0; g_btState.scroll = 0;
            g_bt.loadPairedDevices();
        } else if (key == '?') {
            g_btState.helpScroll = 0;
            g_btState.showHelp = true;
            drawHelp();
            return APP_BT_MANAGE;
        } else if (g_bt.isConnected()) {
            // 连接成功 → 返回管理列表并选中该设备
            g_btState.mode = BT_MANAGE;
            g_btState.connecting = false;
            g_btState.statusMsg[0] = '\0';
            g_bt.loadPairedDevices();
            int ci = g_bt.connectedPairedIndex();
            g_btState.selection = (ci >= 0) ? ci : 0;
            g_btState.scroll = 0;
        } else if (!g_btState.scanning && !g_btState.connecting) {
            if (key == KEY_UP) {
                if (g_btState.selection > 0) g_btState.selection--;
            } else if (key == KEY_DOWN) {
                if (g_btState.selection < g_bt.deviceCount() - 1) g_btState.selection++;
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
        }
        drawScan();
        return APP_BT_MANAGE;
    }

    // ── 已配对设备管理模式(默认) ──
    if (key == 'q' || key == 'Q' || key == 0x1B) {
        if (g_btState.connecting) g_bt.disconnect();
        ctx.nextState = g_quickEdit ? APP_SETTINGS : APP_MAIN;
        return ctx.nextState;
    } else if (key == 'a' || key == 'A') {
        g_btState.mode = BT_SCAN;
        g_btState.connecting = false;
        g_btState.conn_start_ms = 0;
        g_btState.statusMsg[0] = '\0';
        g_btState.selection = 0; g_btState.scroll = 0;
        g_btState.scanning = true;
        g_bt.scanDevices();
    } else if (key == 'd' || key == 'D') {
        int n = g_bt.pairedDeviceCount();
        if (n > 0 && g_btState.selection < n) {
            const BtPairedDevice *p = g_bt.getPairedDevice(g_btState.selection);
            if (g_bt.connectedPairedIndex() == g_btState.selection) g_bt.disconnect();
            g_bt.removePairedDevice(p->bda);
            if (g_btState.selection >= g_bt.pairedDeviceCount())
                g_btState.selection = g_bt.pairedDeviceCount() - 1;
            if (g_btState.selection < 0) g_btState.selection = 0;
            g_btState.statusMsg[0] = '\0';
        }
    } else if (key == '?') {
        g_btState.helpScroll = 0;
        g_btState.showHelp = true;
        drawHelp();
        return APP_BT_MANAGE;
    } else if (g_btState.connecting) {
        // 连接中忽略导航键
    } else if (key == KEY_UP) {
        if (g_btState.selection > 0) g_btState.selection--;
    } else if (key == KEY_DOWN) {
        if (g_btState.selection < g_bt.pairedDeviceCount() - 1) g_btState.selection++;
    } else if (key == 0x0A || key == 0x0D) {
        int n = g_bt.pairedDeviceCount();
        if (n > 0 && g_btState.selection < n) {
            const BtPairedDevice *p = g_bt.getPairedDevice(g_btState.selection);
            g_btState.connecting = true;
            g_btState.conn_start_ms = esp_timer_get_time() / 1000;
            snprintf(g_btState.statusMsg, sizeof(g_btState.statusMsg),
                     "正在连接 %s...", p->name);
            g_bt.connectBDA(p->bda, p->addr_type);
        }
    }

    drawManage();
    return APP_BT_MANAGE;
}
