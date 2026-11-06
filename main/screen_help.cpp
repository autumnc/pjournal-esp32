// 使用说明：主界面 [?] 进入。屏幕只有 400×300，整本手册拆成章节：
// 目录里上下选章节，Enter 进入；章节内上下/翻页滚动，左右或 h/l 换章。
// 正文按像素宽度逐字折行（与 Flomo 帮助同一套逻辑），中英文混排都不会溢出。

#include "screen_help.h"

#include "font_renderer.h"
#include "ui_helpers.h"

#include <cstdio>
#include <string>
#include <vector>

extern u8g2_t *g_u8g2;
extern "C" {
    extern void u8g2_SetDrawColor(void *g_u8g2, int color);
    extern void u8g2_DrawHLine(void *g_u8g2, int x, int y, int w);
}

struct HelpChapter {
    const char *title;
    const char *body;
};

static const HelpChapter CHAPTERS[] = {
    {"快速上手",
     "首次使用\n"
     "先到 [s] 设置 里填写 WiFi\n"
     "名称和密码，联网后同步时间。\n"
     "蓝牙键盘：长按 USER 键进入\n"
     "蓝牙管理，搜索并配对设备。\n"
     "\n"
     "键盘连上后回到主界面：\n"
     "左右方向键或 h/l 选择图标，\n"
     "Enter 进入；也可以直接按\n"
     "图标下方显示的字母键。\n"
     "\n"
     "马上写一篇：\n"
     "按 f 进入自由写作，\n"
     "写完按 Esc，选择保存即可。\n"
     "\n"
     "任何时候按 ? 可打开本说明。\n"
     "阅读时 Enter 进章节，Esc 回目录，\n"
     "再按 Esc 回到主界面。"},

    {"主界面",
     "主界面上方是日期与本周记录，\n"
     "中间一行是功能图标，\n"
     "下方显示当前选中项名称。\n"
     "设置里可切换周视图/月视图。\n"
     "\n"
     "快捷键：\n"
     "p  提示写作\n"
     "f  自由写作\n"
     "v  查看过往日记\n"
     "w  同步 WebDAV\n"
     "t  GTD 任务管理\n"
     "o  大纲写作\n"
     "n  Flomo 笔记\n"
     "s  设置\n"
     "?  使用说明\n"
     "\n"
     "图标较多时用左右键横向滚动，\n"
     "两侧的 < > 表示还有内容。\n"
     "\n"
     "物理按键：\n"
     "长按 BOOT 进入休眠，\n"
     "任意物理键唤醒并恢复画面；\n"
     "长按 USER 进入蓝牙管理。\n"
     "Ctrl+I 可随时打开灵感面板。"},

    {"日记写作",
     "f 自由写作：新建空白日记。\n"
     "p 提示写作：随机给一句写作\n"
     "提示，围绕它记录。\n"
     "\n"
     "连接蓝牙键盘后直接打字。\n"
     "中文输入按 Ctrl+Space 切换，\n"
     "输入编码后用数字键选字，\n"
     "空格选第一个，左右键翻页，\n"
     "Esc 取消本次输入。\n"
     "\n"
     "保存与退出：\n"
     "Ctrl+S 立即保存；\n"
     "Esc 退出时若有未保存改动，\n"
     "会询问是否保存：\n"
     "Enter 保存并退出，n 放弃。\n"
     "\n"
     "设置里打开自动保存后，\n"
     "停笔 3 秒自动存储；\n"
     "打开恢复草稿后，意外断电\n"
     "下次开机可恢复未保存内容。"},

    {"编辑器快捷键",
     "Ctrl+S     保存\n"
     "Ctrl+Q     退出编辑\n"
     "Ctrl+Z     撤销\n"
     "Ctrl+R     重做\n"
     "Ctrl+Y     查看当前日记历史版本\n"
     "Ctrl+P     AI 生成写作提示\n"
     "Ctrl+F     发送正文到 Flomo\n"
     "Ctrl+O     AI 润色选中文本\n"
     "Ctrl+T     折叠/展开当前标题\n"
     "Ctrl+/     查找/替换\n"
     "Ctrl+?     编辑器快捷键帮助\n"
     "\n"
     "Ctrl+C/X/V 复制/剪切/粘贴\n"
     "Ctrl+A     全选\n"
     "Shift+方向键 选择文本\n"
     "Home/End   行首/行尾\n"
     "PgUp/PgDn  上一屏/下一屏\n"
     "\n"
     "双击 BOOT  全文 AI 润色\n"
     "双击 USER  语音听写\n"
     "\n"
     "AI 功能需要先在设置里填写\n"
     "Deepseek Key 并联网。"},

    {"Markdown 与排版",
     "编辑器实时渲染 Markdown，\n"
     "可在设置中关闭。\n"
     "\n"
     "# 标题        ###### 六级标题\n"
     "- 项目        1. 有序列表\n"
     "- [ ] 待办    - [x] 已完成\n"
     "> 引用        --- 分割线\n"
     "**加粗**      *斜体*\n"
     "`代码`        ~~删除~~\n"
     "==高亮==\n"
     "\n"
     "光标停在标题行，按 Ctrl+T 可\n"
     "折叠该标题下的内容，再按展开。\n"
     "\n"
     "设置里可以调整：\n"
     "文字方向（横排/竖排）、\n"
     "竖排参考线、首行缩进、\n"
     "打字机模式、打字音效、\n"
     "字体大小、显示行号、Vi 模式。"},

    {"过往日记与历史",
     "v 查看过往日记：\n"
     "j/k 或上下键选择，\n"
     "Enter 打开详情，e 编辑，\n"
     "h 查看历史版本，d 删除，\n"
     "q 或 Esc 返回主界面。\n"
     "\n"
     "历史版本（编辑器 Ctrl+Y，\n"
     "列表或详情里按 h 也可进入）：\n"
     "j/k      选择\n"
     "Enter    预览\n"
     "r        恢复所选版本\n"
     "d        删除所选版本\n"
     "q/Esc    返回\n"
     "\n"
     "需要先在设置里打开版本历史，\n"
     "之后每次保存自动生成快照，\n"
     "每篇保留最近 10 个版本。"},

    {"GTD 任务管理",
     "t 进入 GTD 任务管理：\n"
     "j/k      上下移动\n"
     "h/l      提高/降低层级\n"
     "a        添加任务\n"
     "i        添加子任务\n"
     "r        重命名\n"
     "Enter    任务详情（字段、\n"
     "         优先级、截止日期）\n"
     "Space    切换完成状态\n"
     "Shift+上下 连续多选\n"
     "d        删除（多选时批量）\n"
     "z/Z      折叠/全部折叠展开\n"
     "Tab      切换标签\n"
     "/        筛选\n"
     "A        归档管理\n"
     "s        任务摘要\n"
     "c        情境管理\n"
     "t        标签管理\n"
     "n        新建项目"},

    {"大纲写作",
     "o 进入大纲写作：\n"
     "a        添加标题\n"
     "i        添加子标题\n"
     "r        重命名\n"
     "Enter    详情面板\n"
     "f        关联文件\n"
     "s        摘要\n"
     "d        删除标题\n"
     "Tab      切换项目\n"
     "/        筛选\n"
     "t        标签管理\n"
     "j/k      上下移动\n"
     "h/l      提高/降低层级"},

    {"灵感面板",
     "Ctrl+I 在任意界面打开灵感面板，\n"
     "随手记下一闪而过的想法。\n"
     "\n"
     "a        添加灵感\n"
     "d        删除灵感\n"
     "Enter    编辑内容\n"
     "k        编辑关键词\n"
     "/        检索内容或关键词\n"
     "c        复制到粘贴板\n"
     "q/Esc    返回\n"
     "\n"
     "灵感保存在 SD 卡的\n"
     "outline/inspiration.json，\n"
     "与大纲写作共用目录。"},

    {"Flomo 笔记",
     "n 打开 Flomo 笔记列表。\n"
     "快捷键区分大小写：\n"
     "N        新建笔记\n"
     "Enter    查看详情\n"
     "E        编辑当前笔记\n"
     "C        复制正文到粘贴板\n"
     "D        删除当前笔记\n"
     "S        手动同步（需要 WiFi）\n"
     "T        标签树/按标签筛选\n"
     "/        搜索\n"
     "?        本页帮助\n"
     "Esc      返回主界面\n"
     "\n"
     "本地优先：新建和修改只写 SD 卡，\n"
     "列表里带 * 表示尚未同步。\n"
     "按 S 才会上传本地改动，\n"
     "并按游标增量拉取远端新内容。\n"
     "\n"
     "标签写在正文里（#标签），\n"
     "支持父/子层级；按 T 打开标签树，\n"
     "Enter 筛选，选第一行清除筛选。\n"
     "令牌过期时同步会自动重新登录。"},

    {"WebDAV 同步",
     "w 开始同步 WebDAV。\n"
     "\n"
     "先在 s 设置 的账号相关里\n"
     "填写 WebDAV URL、用户名、密码，\n"
     "在连接管理里填好 WiFi。\n"
     "\n"
     "进入同步界面后自动上传/下载，\n"
     "屏幕会显示联网与同步进度，\n"
     "完成后短暂显示结果再返回。\n"
     "\n"
     "同步使用网络，耗时几秒到\n"
     "几十秒不等；期间请保持开机。\n"
     "若失败，检查网络与 WebDAV\n"
     "地址后重试。"},

    {"设置",
     "s 进入设置。设置项分七组，\n"
     "组标题可折叠/展开：\n"
     "模式设置、文本编辑器、\n"
     "输入法设置、文件管理、\n"
     "连接管理、账号相关、个人信息。\n"
     "\n"
     "j/k 或上下键选择，\n"
     "Enter 修改开关或展开选项，\n"
     "左右键调整数值，\n"
     "Esc 保存并返回。\n"
     "\n"
     "常用项：\n"
     "WiFi SSID/密码、时区、\n"
     "网络同步时间、字体大小、\n"
     "自动保存、版本历史、\n"
     "自动休眠、词库管理、\n"
     "U 盘模式、蓝牙设备管理。\n"
     "\n"
     "工作模式切换为快捷编辑后\n"
     "需要重启生效。"},

    {"输入法",
     "中文输入：\n"
     "Ctrl+Space   中/英切换\n"
     "Shift+Space  全角/半角\n"
     "Ctrl+Shift+F 繁体/简体\n"
     "Ctrl+D       删除动态用户词\n"
     "左 Shift 单击 临时英文模式\n"
     "\n"
     "拼音候选：数字键 1-9 选字，\n"
     "空格选第一个，左右键翻页，\n"
     "Esc 取消本次输入。\n"
     "\n"
     "v 模式快捷输入：\n"
     "裸 v 出颜文字候选；\n"
     "v/kaixin 或 v/kx 搜颜文字；\n"
     "v/bd 搜标点；\n"
     "v/t/ v/d/ v/w/ 分别出时间、\n"
     "日期、星期候选。\n"
     "\n"
     "英文模式下输入字母出单词联想。\n"
     "设置里的词库管理可查看、\n"
     "搜索、删除和添加词条，\n"
     "按 i/e 导入/导出词库文件。"},

    {"蓝牙与休眠",
     "蓝牙键盘：\n"
     "长按 USER 进入蓝牙管理，\n"
     "上下键选择设备，\n"
     "长按连接，双击添加或删除，\n"
     "按提示操作即可。\n"
     "\n"
     "已配对设备在开机或休眠唤醒后\n"
     "自动重连：60 秒内每 5 秒重试，\n"
     "期间按一下键盘任意键帮助唤醒。\n"
     "状态栏会显示连接状态与电量。\n"
     "\n"
     "休眠：\n"
     "长按 BOOT 进入休眠，\n"
     "按任意物理按键唤醒，\n"
     "唤醒后立即恢复休眠前画面。\n"
     "设置里可开启自动休眠：\n"
     "空闲 10 分钟后自动进入；\n"
     "还可选择休眠时是否保留画面。\n"
     "\n"
     "蓝牙管理/GTD 列表里：\n"
     "USER 单击=上，BOOT 单击=下，\n"
     "双击在两个按键上有额外功能\n"
     "（润色、语音听写、蓝牙增删等）。"},

    {"文件管理与 U 盘",
     "文件管理：\n"
     "设置里的文件管理可浏览 SD 卡。\n"
     "快捷编辑/文件编辑模式下\n"
     "按 Ctrl+E 打开本地文件管理器：\n"
     "a 新建、r 改名、d 删除、\n"
     "Enter 打开文件或进入目录，\n"
     "Esc 关闭。\n"
     "\n"
     "Web 文件管理：\n"
     "开启后在同一网络的浏览器里\n"
     "访问设备地址即可管理 SD 卡；\n"
     "可设置访问令牌保护。\n"
     "\n"
     "U 盘模式：\n"
     "设置 → 文件管理 → U 盘模式，\n"
     "设备会作为 U 盘挂载到电脑。\n"
     "期间串口会断开，退出后恢复；\n"
     "请先在电脑上安全弹出再退出。\n"
     "\n"
     "快捷编辑：\n"
     "SD 卡根目录的 0.txt-9.txt，\n"
     "Ctrl+0-9 切换文件。"},

    {"常见问题",
     "同步提示写盘失败/内存不足：\n"
     "多为 SD 卡或内部内存紧张。\n"
     "确认 SD 卡插好、格式为 FAT32，\n"
     "重启后先不要同时跑蓝牙、\n"
     "联网和大量输入，再试一次。\n"
     "\n"
     "Flomo 提示令牌过期：\n"
     "检查设置里的 Flomo 邮箱/密码，\n"
     "同步时会自动重新登录；\n"
     "也可手动重新生成 Token。\n"
     "\n"
     "连不上 WiFi：\n"
     "确认 SSID/密码正确，\n"
     "使用 2.4GHz 网络，\n"
     "信号弱时靠近路由器重试。\n"
     "\n"
     "输入法不出候选：\n"
     "按 Ctrl+Space 确认处于中文态。\n"
     "\n"
     "时间不对：\n"
     "设置时区（如 CST-8），\n"
     "联网后执行网络同步时间。\n"
     "\n"
     "屏幕卡住：长按 BOOT 休眠，\n"
     "再按任意键唤醒。"}
};
static const int CHAPTER_COUNT = (int)(sizeof(CHAPTERS) / sizeof(CHAPTERS[0]));

static bool s_reading = false;   // false = 目录, true = 阅读章节
static bool s_drawn = false;     // 进入屏幕后第一拍必须画一次
static int s_sel = 0;            // 目录选中项
static int s_tocScroll = 0;
static int s_chapter = 0;
static int s_scroll = 0;

static size_t utf8Len(const std::string &s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

// 按像素宽度折行（与 screen_flomo.cpp 的 wrapText 同款）。
static std::vector<std::string> wrapText(const std::string &text, int maxW) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (true) {
        size_t nl = text.find('\n', pos);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string para = text.substr(pos, end - pos);
        if (para.empty()) {
            out.push_back("");
        } else {
            size_t i = 0;
            while (i < para.size()) {
                std::string line;
                size_t j = i;
                size_t lastSpace = std::string::npos;
                while (j < para.size()) {
                    size_t n = utf8Len(para, j);
                    std::string cand = line + para.substr(j, n);
                    if (!line.empty() && g_font.textWidth(cand.c_str()) > maxW) break;
                    if (para[j] == ' ') lastSpace = line.size();
                    line = cand;
                    j += n;
                }
                if (j >= para.size()) { out.push_back(line); break; }
                size_t cut = (lastSpace != std::string::npos && lastSpace > 0)
                                 ? lastSpace : line.size();
                out.push_back(para.substr(i, cut));
                i += cut;
                while (i < para.size() && para[i] == ' ') i++;
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

// 章节折行结果缓存：章节、宽度、字号不变就复用。
static const std::vector<std::string> &chapterLines(int idx) {
    static int cachedIdx = -1;
    static int cachedW = -1;
    static int cachedPx = -1;
    static std::vector<std::string> lines;
    const int w = SCREEN_W - 16;
    const int px = g_font.fontSize();
    if (cachedIdx != idx || cachedW != w || cachedPx != px) {
        cachedIdx = idx;
        cachedW = w;
        cachedPx = px;
        lines = wrapText(CHAPTERS[idx].body, w);
    }
    return lines;
}

static int contentTop() { return 28 + g_font.descent() + 12; }

static int bodyVisible() {
    int v = (STATUS_Y - contentTop()) / LINE_SPACING;
    return v < 1 ? 1 : v;
}

static int tocVisible() { return bodyVisible(); }

static void drawHeader(const char *title) {
    ui_clear();
    ui_draw_text_centered(28, title, false, true);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, 28 + g_font.descent() + 4, SCREEN_W);
}

static void drawToc() {
    const int vis = tocVisible();
    if (s_sel < 0) s_sel = 0;
    if (s_sel >= CHAPTER_COUNT) s_sel = CHAPTER_COUNT - 1;
    if (s_sel < s_tocScroll) s_tocScroll = s_sel;
    if (s_sel >= s_tocScroll + vis) s_tocScroll = s_sel - vis + 1;
    if (s_tocScroll > CHAPTER_COUNT - vis) s_tocScroll = CHAPTER_COUNT - vis;
    if (s_tocScroll < 0) s_tocScroll = 0;

    drawHeader("使用说明");
    const int y0 = contentTop();
    for (int i = 0; i < vis && s_tocScroll + i < CHAPTER_COUNT; i++) {
        int idx = s_tocScroll + i;
        char buf[64];
        snprintf(buf, sizeof(buf), "%2d %s", idx + 1, CHAPTERS[idx].title);
        ui_draw_text(12, y0 + i * LINE_SPACING + g_font.ascent(), buf,
                     idx == s_sel, idx == s_sel);
    }
    char right[24];
    snprintf(right, sizeof(right), "%d/%d", s_sel + 1, CHAPTER_COUNT);
    ui_draw_status("Enter打开 Esc返回", right);
    ui_commit();
}

static void drawChapter() {
    const std::vector<std::string> &lines = chapterLines(s_chapter);
    const int n = (int)lines.size();
    const int vis = bodyVisible();
    int maxScroll = n - vis;
    if (maxScroll < 0) maxScroll = 0;
    if (s_scroll > maxScroll) s_scroll = maxScroll;
    if (s_scroll < 0) s_scroll = 0;

    drawHeader(CHAPTERS[s_chapter].title);
    const int y0 = contentTop();
    for (int i = 0; i < vis && s_scroll + i < n; i++) {
        ui_draw_text(8, y0 + i * LINE_SPACING + g_font.ascent(),
                     lines[s_scroll + i].c_str());
    }
    char right[24];
    snprintf(right, sizeof(right), "%d/%d", s_chapter + 1, CHAPTER_COUNT);
    ui_draw_status("Esc目录 上下滚动", right);
    ui_commit();
}

void screen_help_init() {
    s_reading = false;
    s_drawn = false;
    s_sel = 0;
    s_tocScroll = 0;
    s_chapter = 0;
    s_scroll = 0;
}

AppState screen_help_handle(int key, ScreenContext &ctx) {
    ctx.nextState = APP_HELP;
    if (!s_drawn) {
        s_drawn = true;
        if (s_reading) drawChapter();
        else drawToc();
        return APP_HELP;
    }
    if (key == 0) return APP_HELP;

    if (!s_reading) {
        if (key == 0x1B || key == 'q' || key == 'Q') {
            ctx.nextState = APP_MAIN;
            return APP_MAIN;
        }
        const int vis = tocVisible();
        if (key == KEY_UP || key == 'k') {
            if (s_sel > 0) s_sel--;
        } else if (key == KEY_DOWN || key == 'j') {
            if (s_sel + 1 < CHAPTER_COUNT) s_sel++;
        } else if (key == KEY_PAGE_UP) {
            s_sel -= vis;
            if (s_sel < 0) s_sel = 0;
        } else if (key == KEY_PAGE_DOWN || key == ' ') {
            s_sel += vis;
            if (s_sel >= CHAPTER_COUNT) s_sel = CHAPTER_COUNT - 1;
        } else if (key == KEY_HOME) {
            s_sel = 0;
        } else if (key == KEY_END) {
            s_sel = CHAPTER_COUNT - 1;
        } else if (key == 0x0A || key == 0x0D || key == KEY_RIGHT || key == 'l') {
            s_chapter = s_sel;
            s_scroll = 0;
            s_reading = true;
            drawChapter();
            return APP_HELP;
        }
        drawToc();
        return APP_HELP;
    }

    // 章节阅读
    if (key == 0x1B || key == 'q' || key == 'Q') {
        s_reading = false;
        drawToc();
        return APP_HELP;
    }
    const int vis = bodyVisible();
    const int n = (int)chapterLines(s_chapter).size();
    int maxScroll = n - vis;
    if (maxScroll < 0) maxScroll = 0;
    if (key == KEY_UP || key == 'k') {
        if (s_scroll > 0) s_scroll--;
    } else if (key == KEY_DOWN || key == 'j') {
        if (s_scroll < maxScroll) s_scroll++;
    } else if (key == KEY_PAGE_UP) {
        s_scroll -= vis;
        if (s_scroll < 0) s_scroll = 0;
    } else if (key == KEY_PAGE_DOWN || key == ' ') {
        s_scroll += vis;
        if (s_scroll > maxScroll) s_scroll = maxScroll;
    } else if (key == KEY_HOME) {
        s_scroll = 0;
    } else if (key == KEY_END) {
        s_scroll = maxScroll;
    } else if (key == KEY_LEFT || key == 'h') {
        if (s_chapter > 0) s_chapter--;
        s_scroll = 0;
    } else if (key == KEY_RIGHT || key == 'l') {
        if (s_chapter + 1 < CHAPTER_COUNT) s_chapter++;
        s_scroll = 0;
    }
    drawChapter();
    return APP_HELP;
}
