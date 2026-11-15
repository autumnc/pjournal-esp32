// 编辑器 Win 快捷菜单：单按左 Win 弹出，列出「快捷键 + 功能」。
//
// 绘制方式与剪贴板菜单一致：打开时把当前整帧复制到 PSRAM 作为底板，每次
// 重绘先恢复底板再画不透明面板，用 ui_send_buffer() 发送（不更新 ui_commit
// 的快照），关闭后编辑器按需重绘。
//
// 内存：底板只在 PSRAM 分配一次（约 15 KB），之后复用；**不回落内部 RAM**，
// 分配失败就不弹菜单，避免挤占最紧张的内部 RAM。菜单项是固定大小静态数组，
// 没有任何动态容器；查询接口只返回 bool，不复制正文。

#include "shortcut_menu.h"

#include "font_renderer.h"
#include "ime/IME.h"
#include "pjournal_app.h"
#include "screen_editor.h"
#include "settings_manager.h"
#include "ui_helpers.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include "u8g2.h"

extern u8g2_t *g_u8g2;

static const char *TAG = "ShortcutMenu";

struct MenuItem {
    ShortcutMenuAction action;
    const char *label;
    const char *shortcut;
};

static bool s_active = false;
static int s_sel = 0;
static int s_count = 0;
static MenuItem s_items[12];        // 最多 11 项，留一点余量
static uint8_t *s_base = nullptr;   // PSRAM 整帧底板
static size_t s_baseSize = 0;

static const int kMenuW = 340;

static void addItem(ShortcutMenuAction action, const char *label, const char *shortcut) {
    if (s_count >= (int)(sizeof(s_items) / sizeof(s_items[0]))) return;
    s_items[s_count].action = action;
    s_items[s_count].label = label;
    s_items[s_count].shortcut = shortcut;
    s_count++;
}

// 按当前模式构建可见项（打开时快照，菜单期间不再变化）。
static void buildItems() {
    s_count = 0;
    addItem(ShortcutMenuAction::Save, "保存", "Ctrl+S");
    addItem(ShortcutMenuAction::Clipboard, "剪贴板", "Alt+V");
    addItem(ShortcutMenuAction::Inspiration, "灵感", "Ctrl+I");
    addItem(ShortcutMenuAction::Search, "查找/替换", "Ctrl+/");

    // 版本历史只在开启且是日记/灵感会话时可用（快捷编辑/文件编辑里 Ctrl+Y
    // 是切换文件）。
    if (g_settings.versionHistory() && !app_editor_in_quick_file_session() &&
        !app_editor_in_file_edit_session()) {
        addItem(ShortcutMenuAction::History, "版本历史", "Ctrl+Y");
    }

    if (app_editor_prompt_mode()) {
        addItem(ShortcutMenuAction::Prompt, "AI 提示词", "Ctrl+P");
    }

    // 有选区润色选区，否则润色全文；空文档不显示。
    if (app_editor_has_selection()) {
        addItem(ShortcutMenuAction::PolishSelection, "AI 润色", "Ctrl+O");
    } else if (!app_editor_is_empty()) {
        addItem(ShortcutMenuAction::PolishAll, "AI 润色", "BOOT x2");
    }

    if (app_editor_in_file_edit_session()) {
        addItem(ShortcutMenuAction::FileManager, "文件管理器", "Ctrl+E");
    }

    addItem(ShortcutMenuAction::Voice, "语音听写", "USER x2");
    addItem(ShortcutMenuAction::Help, "帮助", "Ctrl+?");
    addItem(ShortcutMenuAction::Exit, "退出", "Ctrl+Q");
}

static void drawMenu() {
    if (!g_u8g2 || !s_base || s_count <= 0) return;

    const int prevSize = g_font.fontSize();
    g_font.setSize(18);
    const int asc = g_font.ascent();
    const int rowH = g_font.lineHeight() + 2;
    const int pad = 6;
    const int titleRowH = rowH + 2;
    const int hintRowH = rowH + 3;   // 提示行整体比普通行低 3px，框底随之外扩

    const int boxW = kMenuW;
    int boxH = pad + titleRowH + s_count * rowH + hintRowH + pad;
    if (boxH > SCREEN_H) boxH = SCREEN_H;
    const int boxX = (SCREEN_W - boxW) / 2;
    int boxY = (SCREEN_H - boxH) / 2;
    if (boxY < 0) boxY = 0;
    const int innerX = boxX + pad;
    const int innerW = boxW - 2 * pad;

    // 快捷键列宽按最长的一条算，功能列起点随之对齐。
    int scW = 0;
    for (int i = 0; i < s_count; i++) {
        const int w = g_font.textWidth(s_items[i].shortcut);
        if (w > scW) scW = w;
    }
    const int labelX = innerX + 8 + scW + 14;

    if (s_sel < 0) s_sel = 0;
    if (s_sel >= s_count) s_sel = s_count - 1;

    // 恢复底板，再在上面画不透明面板。
    memcpy(u8g2_GetBufferPtr(g_u8g2), s_base, s_baseSize);
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);

    g_font.drawText(innerX, boxY + pad + asc + 2, "快捷菜单");
    u8g2_DrawHLine(g_u8g2, innerX, boxY + pad + titleRowH - 2, innerW);

    const int listY = boxY + pad + titleRowH;
    for (int i = 0; i < s_count; i++) {
        const int rowTop = listY + i * rowH;
        const bool sel = (i == s_sel);
        if (sel) {
            u8g2_SetDrawColor(g_u8g2, 0);
            u8g2_DrawBox(g_u8g2, innerX, rowTop, innerW, rowH);
            u8g2_SetDrawColor(g_u8g2, 1);
        }
        const int ty = rowTop + 1 + asc;
        g_font.drawText(innerX + 4, ty, s_items[i].shortcut);
        g_font.drawText(labelX, ty, s_items[i].label);
        if (sel) u8g2_SetDrawColor(g_u8g2, 0);
    }

    // 选择/执行/关闭提示行上方画一条横线，与菜单列表分开。
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, innerX, listY + s_count * rowH + 1, innerW);

    const char *hint = "↑↓ 选择  Enter 执行  Esc 关闭";
    const int hintY = boxY + boxH - pad - g_font.descent() - 2;
    g_font.drawText(boxX + (boxW - g_font.textWidth(hint)) / 2, hintY, hint);

    g_font.setSize(prevSize);
    ui_send_buffer();
}

bool shortcut_menu_active() { return s_active; }

bool shortcut_menu_open() {
    if (s_active) return true;
    if (!g_u8g2) return false;

    const size_t size = u8g2_GetBufferSize(g_u8g2);
    if (size == 0) return false;

    if (!s_base) {
        // 只允许 PSRAM：内部 RAM 是最紧的资源，宁可没有菜单。
        s_base = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_base) {
            ESP_LOGE(TAG, "PSRAM frame copy alloc failed (%u bytes), menu unavailable",
                     (unsigned)size);
            return false;
        }
        s_baseSize = size;
    } else if (s_baseSize != size) {
        return false;  // 缓冲区尺寸变化不应发生；保守放弃
    }

    memcpy(s_base, u8g2_GetBufferPtr(g_u8g2), size);
    g_ime.cancelComposition();  // 只丢未上屏的拼音组合，保留输入法开关状态
    buildItems();
    s_sel = 0;
    s_active = true;
    drawMenu();
    return true;
}

void shortcut_menu_close(bool restore) {
    if (!s_active) return;
    s_active = false;
    if (restore && s_base && g_u8g2 && s_baseSize == u8g2_GetBufferSize(g_u8g2)) {
        memcpy(u8g2_GetBufferPtr(g_u8g2), s_base, s_baseSize);
        ui_send_buffer();
    }
    // 让编辑器下个空闲节拍重绘：既清掉打开时取消输入法组合留下的候选条
    // 残影，也保证面板下面的画面是最新的。
    screen_editor_reset_drawn();
}

ShortcutMenuAction shortcut_menu_handle(int key) {
    if (!s_active) return ShortcutMenuAction::None;

    if (key == KEY_UP || key == 'k') {
        if (s_sel > 0) s_sel--;
        drawMenu();
        return ShortcutMenuAction::None;
    }
    if (key == KEY_DOWN || key == 'j') {
        if (s_sel + 1 < s_count) s_sel++;
        drawMenu();
        return ShortcutMenuAction::None;
    }
    if (key == 0x1B || key == 'q' || key == 'Q' || key == KEY_MOD_TAP_LGUI) {
        shortcut_menu_close(true);
        return ShortcutMenuAction::None;
    }
    if (key == 0x0A || key == 0x0D) {
        ShortcutMenuAction act = ShortcutMenuAction::None;
        if (s_sel >= 0 && s_sel < s_count) act = s_items[s_sel].action;
        shortcut_menu_close(true);
        return act;
    }
    // 其余按键吞掉，避免菜单开着时触发底层界面的快捷键。
    return ShortcutMenuAction::None;
}
