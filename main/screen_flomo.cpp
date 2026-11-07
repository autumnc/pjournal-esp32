// Flomo 笔记界面。移植自 ../pico/pjournal-pico/main/screen_flomo.cpp，按本机
// （无触摸、默认字体）改造：
//   · 去掉全部浮动触摸按钮 / 长按菜单 / 触摸选字：新建 N、同步 S、编辑 E、
//     删除 D、搜索 /、帮助 ? 全部走快捷键；
//   · 正文编辑借用本仓写作编辑器（ctx.editContent / ctx.editFilename →
//     APP_EDITOR），回程读 /sdcard/pjournal/__flomo_* 临时文件落库；
//   · 绘制只用 g_font + ui_helpers，不碰 TTF/图标字体。
//
// 本地库是 /sdcard/flomo/memos.json（MemoDb，原子写）；新建/修改/删除只落本地并
// 打 dirty 标记，按 S 才连 WiFi 上传并分页拉取远端。

#include "screen_flomo.h"

#include "clipboard.h"
#include "flomo_api.h"
#include "flomo_db.h"
#include "font_renderer.h"
#include "ime/IME.h"
#include "pjournal_app.h"
#include "safe_file.h"
#include "screen_editor.h"
#include "settings_manager.h"
#include "ui_helpers.h"
#include "wifi_manager.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>   // xTaskCreateWithCaps: 同步任务栈放 PSRAM

extern u8g2_t *g_u8g2;
extern "C" {
    extern void u8g2_SetDrawColor(void *g_u8g2, int color);
    extern void u8g2_DrawBox(void *g_u8g2, int x, int y, int w, int h);
    extern void u8g2_DrawHLine(void *g_u8g2, int x, int y, int w);
    extern void u8g2_DrawFrame(void *g_u8g2, int x, int y, int w, int h);
}

#define FLOMO_DIR "/sdcard/flomo"
#define FLOMO_TMP_DIR "/sdcard/pjournal"

enum FlomoMode { FM_LIST, FM_DETAIL, FM_CONFIRM_DELETE, FM_SEARCH, FM_HELP, FM_TAGS };

// 标签树的一行。"全部笔记"是第 0 行（tag 为空，用来清掉筛选）。
struct FlomoTagEntry {
    std::string tag;
    int depth = 0;
    int count = 0;
};

static struct {
    FlomoMode mode = FM_LIST;
    Store store;
    std::vector<int> filtered;   // 可见笔记在 store.memos 里的下标

    int sel = 0;
    int scroll = 0;
    int detailScroll = 0;
    int helpScroll = 0;

    std::string query;
    std::string tag;            // 当前标签筛选（空 = 全部）
    std::string status;

    std::vector<FlomoTagEntry> tagList;   // 按 T 时从本地库重建
    int tagSel = 0;
    int tagScroll = 0;

    AppState returnTo = APP_MAIN;

    // 编辑器交接：pendingTemp 是 journal 目录下的临时文件名，
    // pendingSlug 为空 = 新建，否则是正在编辑的那条笔记的 slug。
    std::string pendingTemp;
    std::string pendingSlug;
    int editorSeq = 0;

    // 搜索输入
    std::string searchBuf;
    int searchCur = 0;
    bool searchIme = false;

    // 空转不重画：上一次按键那一拍已经画过与当前状态一致的画面。
    bool drawn = false;
} g;

// ── 小工具 ────────────────────────────────────────────────────────────────

static bool isLocalOnly(const Memo &m) {
    return m.pendingOp == "create" || m.slug.rfind("local-", 0) == 0;
}

static std::string nowStamp() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

// 读盘失败时保留内存里原有的库（不清空）：清空后再保存会把整库覆盖成空的。
static bool loadLocal() {
    MemoDb db(FLOMO_DIR);
    Store loaded;
    if (!db.load(loaded)) {
        g.status = "本地库读取失败（检查SD卡）";
        return false;
    }
    g.store = std::move(loaded);
    if (g.store.token.empty()) g.store.token = g_settings.flomoToken();
    return true;
}

static bool saveLocal() {
    MemoDb db(FLOMO_DIR);
    if (g.store.token.empty()) g.store.token = g_settings.flomoToken();
    return db.save(g.store);
}

// 标签是否命中筛选：精确匹配，或筛选的是父标签（前缀 + "/"，把子标签一起算）。
static bool tagMatches(const std::string &memoTag, const std::string &filter) {
    if (filter.empty()) return true;
    if (memoTag == filter) return true;
    return memoTag.size() > filter.size() &&
           memoTag.compare(0, filter.size(), filter) == 0 &&
           memoTag[filter.size()] == '/';
}

static void rebuildFilter() {
    g.filtered.clear();
    for (int i = 0; i < (int)g.store.memos.size(); ++i) {
        const Memo &m = g.store.memos[i];
        if (m.deleted) continue;
        if (!g.query.empty() && m.contentText.find(g.query) == std::string::npos) continue;
        if (!g.tag.empty()) {
            bool hit = false;
            for (const auto &t : m.tags) {
                if (tagMatches(t, g.tag)) { hit = true; break; }
            }
            if (!hit) continue;
        }
        g.filtered.push_back(i);
    }
    if (g.sel >= (int)g.filtered.size()) g.sel = std::max(0, (int)g.filtered.size() - 1);
    if (g.sel < 0) g.sel = 0;
}

// 从本地库重建标签树：按 "/" 分层缩进，父标签即使没有笔记也补一行（选中父标签
// 会把下面的子标签一起筛出来）。第 0 行固定是"全部笔记"。
static void buildTagTree() {
    g.tagList.clear();
    int memoCount = 0;
    std::map<std::string, int> counts;
    std::set<std::string> nodes;
    for (const auto &m : g.store.memos) {
        if (m.deleted) continue;
        memoCount++;
        std::set<std::string> memoNodes;   // 每篇笔记对每个标签节点只计一次数
        for (const auto &t : m.tags) {
            if (t.empty()) continue;
            // 补出所有父路径（工作/项目/子 → 工作、工作/项目、工作/项目/子）
            std::string p = t;
            while (!p.empty()) {
                memoNodes.insert(p);
                nodes.insert(p);
                size_t pos = p.rfind('/');
                if (pos == std::string::npos) break;
                p = p.substr(0, pos);
            }
        }
        for (const auto &n : memoNodes) counts[n]++;
    }
    g.tagList.push_back({"", 0, memoCount});
    for (const auto &tag : nodes) {   // std::set 已按字典序排好
        int depth = 0;
        for (char c : tag) if (c == '/') depth++;
        g.tagList.push_back({tag, depth, counts[tag]});
    }
}

static std::string firstLine(const std::string &s) {
    size_t p = s.find('\n');
    std::string l = p == std::string::npos ? s : s.substr(0, p);
    return l;
}

static size_t utf8Len(const std::string &s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t n = 1;
    if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    if (i + n > s.size()) n = 1;
    return n;
}

// 单行列表项按屏幕宽度截断（第一行可能很长；不截断的话反色选中框会算出
// 超过 u8g2_uint_t 的宽度，画出来是一块乱色）。
static std::string clampLine(const std::string &s, int maxW) {
    if (g_font.textWidth(s.c_str()) <= maxW) return s;
    const char *ell = "…";
    const int ew = g_font.textWidth(ell);
    size_t i = 0;
    while (i < s.size()) {
        size_t n = utf8Len(s, i);
        if (g_font.textWidth(s.substr(0, i + n).c_str()) + ew > maxW) break;
        i += n;
    }
    return s.substr(0, i) + ell;
}

// 按像素宽度折行（中文没有词边界，逐字符量宽；ASCII 空格作为优先断点）。
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
                // 行尾有空格就在空格处断开，英文单词不被拦腰截断。
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

// 详情页折行缓存：正文 + 宽度 + 当前字号不变就复用。
// 放在文件作用域而不是函数内 static，是为了退屏时能显式释放（见 releaseDetailCache）。
static std::string s_detailSrc;
static int s_detailW = -1;
static int s_detailPx = -1;
static std::vector<std::string> s_detailLines;

static const std::vector<std::string> &detailLines(const std::string &text) {
    const int w = SCREEN_W - 16;
    const int px = g_font.fontSize();
    if (s_detailW != w || s_detailPx != px || s_detailSrc != text) {
        s_detailSrc = text;
        s_detailW = w;
        s_detailPx = px;
        s_detailLines = wrapText(s_detailSrc, w);
    }
    return s_detailLines;
}

static void releaseDetailCache() {
    std::string().swap(s_detailSrc);
    std::vector<std::string>().swap(s_detailLines);
    s_detailW = -1;
    s_detailPx = -1;
}

// ── 同步：跑在后台任务里 ──────────────────────────────────────────────────
//
// 一趟同步要连 WiFi（最多 10 秒轮询）、可能还要登录，然后逐条推本地改动、逐页拉
// 远端 —— 整趟最坏几十秒。主任务只负责把 worker 报上来的进度画出来。
// 两个任务之间不共享库：worker 自己从磁盘 load 一份 Store，推拉完整体原子落盘；
// 主任务在"结束"那一拍才把库重新读一遍。
enum FlomoSyncState { FSYNC_IDLE = 0, FSYNC_RUNNING, FSYNC_DONE };
static std::atomic<int> s_syncState{FSYNC_IDLE};
static SemaphoreHandle_t s_syncMux = nullptr;
static std::string s_syncMsg;      // 进度串（worker 写、主任务读来画）
static std::string s_syncDrawnMsg; // 上次画出来的进度串（相同则跳过整屏重画）
static std::string s_syncResult;   // 收尾文案：失败原因是它；成功时留空
static bool s_syncOk = false;
// 登录/重登换到的新令牌。worker 不直接写 g_settings（设置缓存不是线程安全的，
// 用户可能已经 Esc 出去在设置页操作），收工那一拍由主任务落回去。
static std::string s_syncNewToken;

static void ensureSyncMux() {
    if (!s_syncMux) s_syncMux = xSemaphoreCreateMutex();
}
static void syncLock()   { ensureSyncMux(); if (s_syncMux) xSemaphoreTake(s_syncMux, portMAX_DELAY); }
static void syncUnlock() { if (s_syncMux) xSemaphoreGive(s_syncMux); }

static void syncPublish(const std::string &msg) {
    syncLock();
    s_syncMsg = msg;
    syncUnlock();
}

// 连 WiFi（worker 侧版：进度自己报；失败写 status）。
static bool ensureWifiWork(std::string &status) {
    if (g_wifi.isConnected()) return true;
    std::string ssid = g_settings.wifiSsid();
    std::string pass = g_settings.wifiPassword();
    if (ssid.empty()) { status = "未配置 WiFi（先到设置填写）"; return false; }
    g_wifi.begin();
    if (!g_wifi.connect(ssid.c_str(), pass.c_str())) { status = "WiFi 连接失败"; return false; }
    for (int i = 0; i < 100; i++) {
        if (g_wifi.isConnected()) return true;
        static const char *dots[] = {"...", "....", ".....", ".."};
        syncPublish(std::string("正在连接 WiFi") + dots[i % 4]);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    status = "WiFi 连接超时";
    return false;
}

// 从响应里挑出服务端返回的 memo（Flomo 不同接口包的层不一样）。
static Memo memoFromResult(const ApiResult &r) {
    Memo m = memoFromJson(r.data);
    if (!m.slug.empty()) return m;
    m = memoFromJson(r.data["memo"]);
    if (!m.slug.empty()) return m;
    return memoFromJson(r.data["data"]);
}

static void removeMemoBySlug(std::vector<Memo> &memos, const std::string &slug) {
    if (slug.empty()) return;
    memos.erase(std::remove_if(memos.begin(), memos.end(),
                               [&](const Memo &m) { return m.slug == slug; }),
                memos.end());
}

// memo/updated/ 的 data 可能直接是数组，也可能套一层 memos/memo_list/items/list/result。
static const JsonValue &memoList(const JsonValue &data) {
    if (data.isArray()) return data;
    static const char *keys[] = {"memos", "memo_list", "items", "list", "result", "data"};
    for (const char *k : keys) {
        const JsonValue &v = data[k];
        if (v.isArray()) return v;
    }
    return data;
}

// "YYYY-MM-DD HH:MM:SS"（Flomo 的 UTC+8 本地时间）→ Unix 秒。也接受已经是数字
// 的秒/毫秒时间戳。解析失败返回 -1。
static long long updatedAtToEpoch(const JsonValue &v) {
    if (v.isNumber()) {
        long long n = (long long)v.asNumber();
        return n > 1000000000000LL ? n / 1000 : n;
    }
    std::string s = v.asString();
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (s.size() < 19 || sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
        return -1;
    // Howard Hinnant 的 days_from_civil，避免依赖 timegm/不同 libc 的时区处理。
    int yy = y - (mo <= 2 ? 1 : 0);
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long long days = era * 146097LL + (long long)doe - 719468LL;
    return days * 86400LL + h * 3600LL + mi * 60LL + se - 8 * 3600LL;
}

// 手动同步的实际工作：先把本地 dirty 的改动推上去，再逐页拉取远端较新的笔记。
// 只碰传进来的 work、设置和磁盘 —— 不画屏、不碰 g.store。
static bool flomoSyncWork(Store &work, std::string &status, std::string &newToken) {
    if (!ensureWifiWork(status)) return false;

    // 凭证一次读出来：worker 跑着的时候用户可能已经 Esc 出去改设置，后面重登时
    // 不要再跨任务读同一份设置（g_settings 不是线程安全的）。
    const std::string email = g_settings.flomoEmail();
    const std::string pass = g_settings.flomoPassword();

    std::string token = g_settings.flomoToken();
    if (token.empty()) {
        if (email.empty() || pass.empty()) {
            status = "未配置 Flomo 账号（进设置填写）";
            return false;
        }
        syncPublish("正在登录 Flomo…");
        ApiResult r = FlomoApi::login(email, pass);
        if (!r.ok) { status = "登录失败: " + r.message; return false; }
        token = r.data["access_token"].asString();
        if (token.empty()) { status = "登录未返回令牌"; return false; }
        newToken = token;
        work.token = token;
    }

    FlomoApi api(token);

    // 令牌过期（业务码 -10/-20）时用设置里的邮箱密码自动重登一次。Flomo token
    // 会过期，以前只会报"刷新失败"，用户还得回设置手动重新生成。
    bool relogged = false;
    auto reloginIfExpired = [&](const ApiResult &r) -> bool {
        if (r.ok || relogged || (r.code != -10 && r.code != -20)) return false;
        if (email.empty() || pass.empty()) return false;
        relogged = true;
        syncPublish("登录已过期，正在重新登录…");
        ApiResult lr = FlomoApi::login(email, pass);
        if (!lr.ok) return false;
        std::string t = lr.data["access_token"].asString();
        if (t.empty()) return false;
        newToken = t;
        work.token = t;
        api = FlomoApi(t);
        return true;
    };

    // ① 推送本地改动
    int dirtyTotal = 0;
    for (auto &m : work.memos) if (m.dirty) dirtyTotal++;
    int dirtyDone = 0;
    int pushed = 0;
    for (auto &m : work.memos) {
        if (!m.dirty) continue;
        dirtyDone++;
        std::string opName;
        if (m.pendingOp == "create") opName = "上传";
        else if (m.pendingOp == "update") opName = "更新";
        else if (m.pendingOp == "delete") {
            if (isLocalOnly(m)) { m.dirty = false; m.pendingOp.clear(); continue; }
            opName = "删除";
        } else { continue; }

        auto runOp = [&]() -> ApiResult {
            if (m.pendingOp == "create") return api.createMemo(m.contentText);
            if (m.pendingOp == "update") return api.updateMemo(m.slug, m.contentText);
            return api.deleteMemo(m.slug);
        };

        char prog[64];
        snprintf(prog, sizeof(prog), "%s本地改动 %d/%d…", opName.c_str(), dirtyDone, dirtyTotal);
        syncPublish(prog);

        ApiResult r = runOp();
        if (!r.ok && reloginIfExpired(r)) r = runOp();
        if (!r.ok) {
            status = opName + "失败: " + r.message;
            return false;
        }
        pushed++;
        if (m.pendingOp == "create") {
            Memo remote = memoFromResult(r);
            if (remote.slug.empty()) {
                // 服务端没回 memo：本地这条已提交，保留内容清掉标记即可。
                m.dirty = false;
                m.pendingOp.clear();
            } else {
                m = remote;
            }
        } else if (m.pendingOp == "update") {
            Memo remote = memoFromResult(r);
            if (!remote.slug.empty()) m = remote;
            else { m.dirty = false; m.pendingOp.clear(); }
        } else {
            m.dirty = false;
            m.pendingOp.clear();
        }
    }

    // 已删且没有未同步标记的本地条目真正丢掉。
    work.memos.erase(std::remove_if(work.memos.begin(), work.memos.end(),
                                    [](const Memo &m) { return m.deleted && !m.dirty; }),
                     work.memos.end());

    // ② 增量拉取（memo/updated/ 游标分页，最多 50 页兜底）。从上一次成功拉到的
    //    最后一条继续，只取之后更新的（含服务端软删除）；本地库为空时从头拉，
    //    否则旧笔记永远补不回来。游标 = slug + updated_at 的 Unix 秒。
    if (work.memos.empty()) {
        work.syncUpdatedAt = 0;
        work.syncSlug.clear();
    }
    std::string slug = work.syncSlug;
    long long updated = work.syncUpdatedAt;
    int pulled = 0;
    int emptySlug = 0;
    for (int page = 1; page <= 50; ++page) {
        char prog[48];
        snprintf(prog, sizeof(prog), "拉取笔记 第 %d 页…", page);
        syncPublish(prog);
        ApiResult r = api.listPage(slug, updated);
        if (!r.ok && reloginIfExpired(r)) r = api.listPage(slug, updated);
        if (!r.ok) {
            status = "刷新失败: " + r.message;
            return false;
        }
        const JsonValue &list = memoList(r.data);
        if (page == 1)
            ESP_LOGI("FlomoSync", "第1页 data=%s,%s size=%u cursor=%lld/%s",
                     r.data.isArray() ? "array" : "object",
                     list.isArray() ? "memos-array" : "non-array",
                     (unsigned)list.size(), updated, slug.c_str());
        if (!list.isArray() || list.size() == 0) break;

        long long pageCursor = -1;
        std::string pageSlug;
        for (size_t i = 0; i < list.size(); ++i) {
            const JsonValue &item = list[i];
            Memo m = memoFromJson(item);
            if (m.slug.empty()) emptySlug++;
            if (m.deleted) removeMemoBySlug(work.memos, m.slug);
            else upsertMemo(work.memos, m);
            pulled++;
            if (i + 1 == list.size()) {   // 游标取原始条目的最后一条（含已删除的）
                pageCursor = updatedAtToEpoch(item["updated_at"]);
                pageSlug = m.slug;
            }
        }
        // 游标必须前进，否则服务端会一直回同一页。先在最后一页也把游标推进，
        // 这样下次增量从真正的位置继续，而不是重拉最后一页。
        if (pageCursor <= 0 || pageSlug.empty() ||
            (pageCursor == updated && pageSlug == slug)) {
            ESP_LOGW("FlomoSync", "翻页游标无效，提前结束: slug=%s ts=%lld",
                     pageSlug.c_str(), pageCursor);
            break;
        }
        slug = pageSlug;
        updated = pageCursor;
        if ((int)list.size() < FLOMO_PAGE_SIZE) break;
    }
    // 只有真正拉到东西才推进已保存的游标（失败/空页保持原值）。
    if (pulled > 0) {
        work.syncSlug = slug;
        work.syncUpdatedAt = updated;
    }
    ESP_LOGI("FlomoSync", "远端增量 %d 条（空 slug %d），本地库共 %u 条，游标 %lld/%s",
             pulled, emptySlug, (unsigned)work.memos.size(), work.syncUpdatedAt,
             work.syncSlug.c_str());

    work.lastSync = nowStamp();
    sortMemos(work.memos);
    // 成功时 status 兼作收尾文案：让用户看到真正增量拉了几条，而不是列表总数。
    char summary[80];
    snprintf(summary, sizeof(summary), "同步完成：上传 %d 条，拉取 %d 条", pushed, pulled);
    status = summary;
    return true;
}

// 落盘要重试：WiFi/TLS 活着时内部 RAM 很紧，SD 卡的 DMA 缓冲（要连续一块内部
// 内存）会分配失败，日志里是 sdmmc_cmd allocate_dma_buf err=0x101 + EIO。等
// WiFi 断开释放内存后通常就好了，所以恢复 WiFi 状态之后再写，失败再等等重试。
static bool saveWorkWithRetry(Store &work) {
    const char *TAG = "FlomoSync";
    for (int attempt = 1; attempt <= 6; ++attempt) {
        ESP_LOGI(TAG, "落盘尝试 %d: 内部 free %u largest %u DMA largest %u", attempt,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        MemoDb db(FLOMO_DIR);
        if (db.save(work)) {
            if (attempt > 1) ESP_LOGI(TAG, "落盘第 %d 次成功", attempt);
            return true;
        }
        ESP_LOGW(TAG, "落盘失败 第%d次: 内部 largest %u DMA largest %u", attempt,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return false;
}

// worker 任务：自己 load 一份库 → 干活 → 落盘 → 发结果。全程不画屏。
// 真活干在 body 里：任务以 vTaskDelete 收尾，留在任务函数里的 C++ 对象不会析构，
// 所以整库 Store 和 status 都放在 body 的栈帧里，返回时正常销毁。
static void flomoSyncBody() {
    const char *TAG = "FlomoSync";
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "同步开始");

    Store work;
    {
        MemoDb db(FLOMO_DIR);
        if (!db.load(work)) {
            // 读不出来就整趟作废：空库继续同步，收工写盘会把用户的整库覆盖掉。
            syncLock();
            s_syncResult = "本地库读取失败（检查SD卡）";
            s_syncOk = false;
            syncUnlock();
            s_syncState.store(FSYNC_DONE, std::memory_order_release);
            return;
        }
        if (work.token.empty()) work.token = g_settings.flomoToken();
    }
    syncPublish("正在准备…");

    const bool wifiWasConnected = g_wifi.isConnected();
    std::string status;
    std::string newToken;
    bool ok = flomoSyncWork(work, status, newToken);
    // 与 WebDAV / 发送到 Flomo 同一套：本来没连 WiFi 就把这次临时连上的断掉。
    // 落盘放在断开之后：网络栈还没吐出来的内部内存，正是 SD DMA 缓冲要的那块。
    restore_wifi_state(wifiWasConnected);
    if (!saveWorkWithRetry(work)) {
        if (ok) { ok = false; status = "写盘失败(SD 内存不足)"; }
        else if (!status.empty()) status += "；本地库也写盘失败";
    }
    ESP_LOGI(TAG, "同步结束 ok=%d 耗时 %d ms 栈余 %u %s", (int)ok,
             (int)((esp_timer_get_time() - t0) / 1000),
             (unsigned)uxTaskGetStackHighWaterMark(nullptr), status.c_str());

    syncLock();
    s_syncResult = status;
    s_syncOk = ok;
    s_syncNewToken = newToken;
    syncUnlock();
    s_syncState.store(FSYNC_DONE, std::memory_order_release);
}

static void flomoSyncTask(void *arg) {
    (void)arg;
    flomoSyncBody();
    vTaskDeleteWithCaps(nullptr);   // 栈是 xTaskCreateWithCaps 分配的
}

// 手动同步的入口（S 键）：只起任务，这一拍就回来。
static void flomoTriggerSync() {
    if (s_syncState.load(std::memory_order_acquire) != FSYNC_IDLE) return;   // 已经在跑了
    ensureSyncMux();
    syncLock();
    s_syncMsg.clear();
    s_syncResult.clear();
    s_syncNewToken.clear();
    s_syncOk = false;
    syncUnlock();
    s_syncDrawnMsg.clear();
    s_syncState.store(FSYNC_RUNNING, std::memory_order_release);

    TaskHandle_t h = nullptr;
    // 优先级 1（与 flomo_send 一致）；栈 8192 与 flomo_send 相同（同样走 HTTPS）。
    // 本任务多出来的是整库 load/save，而 flomo_json 是堆上 DOM、递归只有嵌套层数。
    // 栈放 PSRAM：同步本身已经在搬整库，内部 RAM 正是最紧的时候，别再要一块连续内部内存。
    if (xTaskCreateWithCaps(flomoSyncTask, "flomo_sync", 8192, nullptr, 1,
                            &h, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE("Flomo", "flomo_sync create failed: internal largest=%u psram largest=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        syncLock();
        s_syncResult = "系统繁忙,请重试";
        s_syncOk = false;
        syncUnlock();
        s_syncState.store(FSYNC_DONE, std::memory_order_release);
    }
}

// ── 绘制 ──────────────────────────────────────────────────────────────────

static int listVisible() {
    const int top = FONT_H + 4 + LINE_SPACING;
    int v = (STATUS_BAR_Y - top) / LINE_SPACING + 1;
    return v < 1 ? 1 : v;
}

static int tagsVisible() {
    const int top = FONT_H + 4 + LINE_SPACING;
    int v = (STATUS_BAR_Y - top) / LINE_SPACING + 1;
    return v < 1 ? 1 : v;
}

static int detailVisible() {
    const int top = FONT_H + 4 + LINE_SPACING;
    int v = (STATUS_BAR_Y - top) / LINE_SPACING + 1;
    return v < 1 ? 1 : v;
}

static int searchVisible() {
    const bool composing = g.searchIme && g_ime.composing();
    const int listY = FONT_H + 4 + LINE_SPACING;
    const int listMaxY = composing ? imeFullscreenPanelTopY() - LINE_SPACING : SCREEN_H;
    int v = (listMaxY - listY) / LINE_SPACING + 1;
    return v < 1 ? 1 : v;
}

static int helpVisible() {
    const int contentY = 28 + g_font.descent() + 12;
    int v = (STATUS_Y - contentY) / LINE_SPACING;
    return v < 1 ? 1 : v;
}

static void followList(int vis) {
    const int n = (int)g.filtered.size();
    if (g.sel < 0) g.sel = 0;
    if (n > 0 && g.sel >= n) g.sel = n - 1;
    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;
    if (g.scroll > n - vis) g.scroll = n - vis;
    if (g.scroll < 0) g.scroll = 0;
}

static void followTags(int vis) {
    const int n = (int)g.tagList.size();
    if (g.tagSel < 0) g.tagSel = 0;
    if (n > 0 && g.tagSel >= n) g.tagSel = n - 1;
    if (g.tagSel < g.tagScroll) g.tagScroll = g.tagSel;
    if (g.tagSel >= g.tagScroll + vis) g.tagScroll = g.tagSel - vis + 1;
    if (g.tagScroll > n - vis) g.tagScroll = n - vis;
    if (g.tagScroll < 0) g.tagScroll = 0;
}

static std::string listStatusLeft() {
    if (!g.status.empty()) return g.status;
    if (g.store.token.empty() && g.filtered.empty()) return "N新建 S同步 T标签 /搜索 ?帮助";
    return "Enter详情 N新建 T标签 S同步";
}

// 列表的画面本体（不含提交）：确认框要盖在详情上，详情页要用它兜底。
static void drawListBody() {
    ui_clear();
    ui_draw_text(4, g_font.ascent(), "Flomo", false, true);
    {
        std::string t = std::to_string((int)g.filtered.size()) + " 条";
        if (!g.query.empty()) t = "/" + g.query + "  " + t;
        else if (!g.tag.empty()) t = "#" + g.tag + "  " + t;
        t = clampLine(t, SCREEN_W - 60);   // 别盖住左边的标题
        int w = g_font.textWidth(t.c_str());
        ui_draw_text(SCREEN_W - w - 4, g_font.ascent(), t.c_str());
    }
    const int sepY = FONT_H + 4;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    const int top = sepY + LINE_SPACING;
    const int vis = listVisible();
    followList(vis);

    for (int i = 0; i < vis && g.scroll + i < (int)g.filtered.size(); i++) {
        const int fi = g.scroll + i;
        const Memo &m = g.store.memos[g.filtered[fi]];
        std::string date = m.updatedAt.size() >= 10 ? m.updatedAt.substr(5, 5) : "--";
        std::string line = clampLine((m.dirty ? "*" : " ") + date + "  " +
                                        firstLine(m.contentText), SCREEN_W - 16);
        bool s = (fi == g.sel);
        ui_draw_text(8, top + i * LINE_SPACING, line.c_str(), s);
    }
    if (g.filtered.empty()) {
        const char *msg = !g.query.empty() ? "无匹配"
                          : (g.store.memos.empty() ? "暂无笔记 — 按 N 新建" : "无可见笔记");
        ui_draw_text(8, top, msg);
    }

    ui_draw_status(listStatusLeft().c_str(), "");
}

static void drawList() {
    drawListBody();
    ui_commit();
}

static void drawTags() {
    ui_clear();
    ui_draw_text(4, g_font.ascent(), "标签筛选", false, true);
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d 个", (int)g.tagList.size() - 1);
        int w = g_font.textWidth(buf);
        ui_draw_text(SCREEN_W - w - 4, g_font.ascent(), buf);
    }
    const int sepY = FONT_H + 4;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    const int top = sepY + LINE_SPACING;
    const int vis = tagsVisible();
    followTags(vis);

    for (int i = 0; i < vis && g.tagScroll + i < (int)g.tagList.size(); i++) {
        const int ti = g.tagScroll + i;
        const FlomoTagEntry &e = g.tagList[ti];
        std::string line;
        if (ti == 0) {
            line = "全部笔记";
        } else {
            line.assign((size_t)e.depth * 2, ' ');
            line += e.tag;
        }
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "  %d", e.count);
        line += cnt;
        line = clampLine(line, SCREEN_W - 16);
        ui_draw_text(8, top + i * LINE_SPACING, line.c_str(), ti == g.tagSel);
    }

    std::string cur = g.tag.empty() ? "当前:全部" : ("当前:#" + g.tag);
    ui_draw_status("Enter筛选 Esc返回", cur.c_str());
    ui_commit();
}

// 详情的画面本体（不含提交）：确认框要盖在它上面。
static void drawDetailBody() {
    ui_clear();
    if (g.filtered.empty()) { drawListBody(); return; }
    const Memo &m = g.store.memos[g.filtered[g.sel]];

    ui_draw_text(4, g_font.ascent(), m.updatedAt.c_str());
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d/%d", g.sel + 1, (int)g.filtered.size());
        int w = g_font.textWidth(buf);
        ui_draw_text(SCREEN_W - w - 4, g_font.ascent(), buf);
    }
    const int sepY = FONT_H + 4;
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    const std::vector<std::string> &lines = detailLines(m.contentText);
    const int top = sepY + LINE_SPACING;
    int vis = detailVisible();
    int maxScroll = (int)lines.size() - vis;
    if (maxScroll < 0) maxScroll = 0;
    if (g.detailScroll > maxScroll) g.detailScroll = maxScroll;
    if (g.detailScroll < 0) g.detailScroll = 0;

    for (int i = 0; i < vis && g.detailScroll + i < (int)lines.size(); i++) {
        ui_draw_text(8, top + i * LINE_SPACING, lines[g.detailScroll + i].c_str());
    }
    if (lines.empty()) ui_draw_text(8, top, "（空）");

    const char *st = g.status.empty()
                         ? (m.dirty ? "未同步·E编辑 D删除 Esc返回" : "E编辑 C复制 D删除 Esc返回")
                         : g.status.c_str();
    ui_draw_status(st, "");
}

static void drawDetail() {
    drawDetailBody();
    ui_commit();
}

static void drawConfirmDelete() {
    drawDetailBody();
    const int w = 260;
    const int h = 2 * LINE_SPACING + 20;
    const int bx = (SCREEN_W - w) / 2;
    const int by = (SCREEN_H - h) / 2;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, bx, by, w, h);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, bx, by, w, h);
    ui_draw_text_centered(by + 10 + g_font.ascent(), "确认删除这条笔记？", false, true);
    ui_draw_text_centered(by + 10 + g_font.ascent() + LINE_SPACING, "Enter确认  Esc取消");
    ui_commit();
}

static void drawSearch() {
    ui_clear();
    ui_draw_text(4, g_font.ascent(), "检索:", false, true);
    std::string display = g.searchBuf.empty() ? " " : g.searchBuf;
    const int inputX = g_font.textWidth("检索:") + 8;
    const int inputY = g_font.ascent();
    g_font.drawText(inputX, inputY, display.c_str());
    int cx = inputX + g_font.textWidth(g.searchBuf.substr(0, g.searchCur).c_str());
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawBox(g_u8g2, cx, inputY + 4, 8, 3);

    const int sepY = FONT_H + 4;
    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    const bool composing = g.searchIme && g_ime.composing();
    const int listY = sepY + LINE_SPACING;
    const int vis = searchVisible();
    followList(vis);

    for (int i = 0; i < vis && g.scroll + i < (int)g.filtered.size(); i++) {
        const Memo &m = g.store.memos[g.filtered[g.scroll + i]];
        std::string date = m.updatedAt.size() >= 10 ? m.updatedAt.substr(5, 5) : "--";
        std::string line = clampLine(date + "  " + firstLine(m.contentText), SCREEN_W - 16);
        ui_draw_text(8, listY + i * LINE_SPACING, line.c_str(), g.scroll + i == g.sel);
    }
    if (g.filtered.empty())
        ui_draw_text(8, listY, g.searchBuf.empty() ? "输入关键词检索" : "无匹配");

    if (composing) drawIMEUIFullscreen();
    else ui_draw_status("Enter打开 Esc返回", imeStatusLabel(g.searchIme).c_str());
    ui_commit();
}

static const char *HELP_LINES[] = {
    "── Flomo 笔记 ──",
    "N     新建笔记",
    "Enter 查看详情",
    "E     编辑当前笔记",
    "C     复制正文到粘贴板",
    "D     删除当前笔记",
    "S     手动同步（连 WiFi）",
    "T     标签树 / 按标签筛选",
    "/     搜索",
    "?     帮助",
    "Esc   返回",
    "",
    "同步说明：",
    "新增/修改只存本地，",
    "带 * 的条目表示还没同步；",
    "按 S 才会上传并拉取远端。",
    "正文编辑借用写作编辑器，",
    "保存后回到本列表。",
};
static const int HELP_LINE_COUNT = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);

static void drawHelp() {
    ui_clear();
    ui_draw_text_centered(28, "Flomo 帮助", false, true);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawHLine(g_u8g2, 0, 28 + g_font.descent() + 4, SCREEN_W);
    const int contentY = 28 + g_font.descent() + 12;
    const int maxVis = helpVisible();
    int maxScroll = HELP_LINE_COUNT - maxVis;
    if (maxScroll < 0) maxScroll = 0;
    if (g.helpScroll > maxScroll) g.helpScroll = maxScroll;
    if (g.helpScroll < 0) g.helpScroll = 0;
    for (int i = 0; i < maxVis && g.helpScroll + i < HELP_LINE_COUNT; i++) {
        ui_draw_text(12, contentY + i * LINE_SPACING + g_font.ascent(),
                     HELP_LINES[g.helpScroll + i], false,
                     HELP_LINES[g.helpScroll + i][0] == '-' && HELP_LINES[g.helpScroll + i][1] == '-');
    }
    ui_draw_status("Esc返回", "");
    ui_commit();
}

// 进度界面（主任务侧）：居中一行提示，每换一段就重画一次。
static void drawSyncShared() {
    std::string msg;
    syncLock();
    msg = s_syncMsg;
    syncUnlock();
    // 进度没变就不重画。同步里最长的一段是等 HTTPS（一次最多 30 秒），
    // 逐拍整屏刷新既闪又白费。
    if (msg == s_syncDrawnMsg) return;
    s_syncDrawnMsg = msg;
    ui_clear();
    int base = (SCREEN_H - FONT_H) / 2;
    ui_draw_text_centered(base, "Flomo 同步", false, true);
    ui_draw_text_centered(base + FONT_H + 6, msg.c_str());
    ui_draw_text_centered(STATUS_Y, "请稍候…");
    ui_commit();
}

// ── 借写作编辑器改正文 ────────────────────────────────────────────────────
static AppState openEditorFor(ScreenContext &ctx, const std::string &slug, const std::string &content) {
    char name[48];
    snprintf(name, sizeof(name), "__flomo_%d_%lld", g.editorSeq++, (long long)std::time(nullptr));
    g.pendingTemp = name;
    g.pendingSlug = slug;

    g_ime.setActive(false);
    g.searchIme = false;
    ctx.editContent = content;
    ctx.editFilename = name;
    ctx.promptMode = false;
    ctx.promptText.clear();
    ctx.editorTitle = "Flomo";
    ctx.prevState = APP_FLOMO;
    ctx.nextState = APP_EDITOR;
    app_editor_request_reinit();
    return APP_EDITOR;
}

// 「从编辑器回来」那一拍：把临时文件里的正文落进库（新建一条，或改一条）。
// 两个调用点：进屏那一刻，以及后台同步收工那一拍（同步期间不能碰库）。
static bool applyPendingEdit() {
    if (g.pendingTemp.empty()) return false;

    std::string path = std::string(FLOMO_TMP_DIR) + "/" + g.pendingTemp;
    std::string content = readWholeFile(path);
    remove(path.c_str());
    std::string slug = g.pendingSlug;
    g.pendingTemp.clear();
    g.pendingSlug.clear();

    if (content.empty()) return false;
    std::string body = extractBody(content);
    if (body.empty()) body = content;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
    if (body.empty()) return false;

    if (slug.empty()) {
        // 新建
        Memo m;
        m.slug = "local-" + std::to_string((long long)std::time(nullptr)) +
                 "-" + std::to_string(g.editorSeq);   // 同一秒连建两条也不撞键
        m.createdAt = nowStamp();
        m.updatedAt = m.createdAt;
        m.contentText = body;
        m.contentHtml = textToHtml(body);
        m.tags = extractTags(body);
        m.dirty = true;
        m.pendingOp = "create";
        g.store.memos.push_back(m);
        sortMemos(g.store.memos);
    } else {
        for (auto &m : g.store.memos) {
            if (m.slug != slug) continue;
            m.contentText = body;
            m.contentHtml = textToHtml(body);
            m.tags = extractTags(body);
            m.updatedAt = nowStamp();
            m.dirty = true;
            m.pendingOp = isLocalOnly(m) ? "create" : "update";
            break;
        }
        sortMemos(g.store.memos);
    }
    // 写盘失败不能报"已保存"——重启后这条改动就没了（内存里还在，仍可再试）。
    bool saved = saveLocal();
    rebuildFilter();
    g.status = saved ? "已保存到本地（按 S 同步）" : "改动未写盘（检查SD卡）";
    return true;
}

// ── 初始化 / 事件 ─────────────────────────────────────────────────────────

void screen_flomo_init(AppState returnTo) {
    g.returnTo = returnTo;
    g.drawn = false;
    s_syncDrawnMsg.clear();   // 重新进屏：就算进度没变也要重画一次
    g_ime.setActive(false);
    g.searchIme = false;

    // 后台同步正在跑：worker 手里那份库快照是它开始那一刻的，这会儿读库/写库都会
    // 和它抢。这一拍什么都不碰，全攒到它收工那一拍再补。
    if (s_syncState.load(std::memory_order_acquire) == FSYNC_RUNNING) {
        g.mode = FM_LIST;   // 上次可能从详情/帮助里跑掉；收工后从列表看结果
        return;
    }

    const bool reloaded = loadLocal();

    // 从编辑器回来：读临时文件、落库，并保留列表位置。
    // 库没读出来就先别落：applyPendingEdit 会保存内存里的旧库，可能把盘上的覆盖掉。
    if (!g.pendingTemp.empty()) {
        if (reloaded) {
            applyPendingEdit();
            return;
        }
        g.mode = FM_LIST;
        return;
    }

    // 从主菜单进屏：总是从干净列表开始（mode 可能残留上次的状态）。
    g.mode = FM_LIST;
    g.sel = 0;
    g.scroll = 0;
    rebuildFilter();
}

// 离开 Flomo 屏时释放整库。整库在内存里是几百上千条 Memo：每条 slug/时间戳都是
// 16~30 字符的 std::string，按 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512 这类小分配
// 全落在内部 RAM。以千条计就是上百 KB、几千个 32~64B 的堆块 —— 退出后不还给系统，
// 紧接着开 WebDAV 连一个 12KB 的内部连续任务栈都建不出来（"系统繁忙"）。
//
// 下次进屏 screen_flomo_init 会从 SD 卡重新 load，所以这里可以整个丢掉。
// pendingTemp/pendingSlug 是往返写作编辑器的路条，必须保留；后台同步任务用的是
// 自己 load 的库快照（见 flomoSyncBody），不碰 g.store，也可以照常释放。
void screen_flomo_release() {
    g.store = Store();
    std::vector<int>().swap(g.filtered);
    std::vector<FlomoTagEntry>().swap(g.tagList);
    releaseDetailCache();
    ESP_LOGI("Flomo", "退屏释放整库: 内部 free %u largest %u | PSRAM free %u largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static void deleteCurrent() {
    if (g.filtered.empty()) return;
    int idx = g.filtered[g.sel];
    Memo &m = g.store.memos[idx];
    if (isLocalOnly(m)) {
        // 还没上传过的新笔记，直接丢掉，不用给服务端发删除。
        g.store.memos.erase(g.store.memos.begin() + idx);
    } else {
        m.deleted = true;
        m.dirty = true;
        m.pendingOp = "delete";
    }
    bool saved = saveLocal();
    rebuildFilter();
    g.status = saved ? "已删除（按 S 同步）" : "已删除但写盘失败（检查SD卡）";
}

// 列表/搜索共用的光标移动。
static bool listNavKey(int key, int vis) {
    if (key == KEY_UP || key == 'k') {
        if (g.sel > 0) g.sel--;
    } else if (key == KEY_DOWN || key == 'j') {
        if (g.sel + 1 < (int)g.filtered.size()) g.sel++;
    } else if (key == KEY_PAGE_UP) {
        g.sel -= vis;
        if (g.sel < 0) g.sel = 0;
    } else if (key == KEY_PAGE_DOWN) {
        g.sel += vis;
        if (g.sel >= (int)g.filtered.size()) g.sel = (int)g.filtered.size() - 1;
        if (g.sel < 0) g.sel = 0;
    } else if (key == KEY_HOME) {
        g.sel = 0;
    } else if (key == KEY_END) {
        g.sel = (int)g.filtered.size() - 1;
        if (g.sel < 0) g.sel = 0;
    } else {
        return false;
    }
    g.status.clear();
    return true;
}

AppState screen_flomo_handle(int key, ScreenContext &ctx) {
    // ── 后台同步的两个状态 ────────────────────────────────────────────────
    const int syncNow = s_syncState.load(std::memory_order_acquire);
    if (syncNow == FSYNC_RUNNING) {
        // Esc 放行：worker 写的是磁盘，不依赖这一屏 —— 走开。回来看到的照样是新库。
        if (key == 0x1B) {
            ctx.nextState = g.returnTo;
            return g.returnTo;
        }
        drawSyncShared();
        return APP_FLOMO;
    }
    if (syncNow == FSYNC_DONE) {   // worker 收工：把库和结果收进来
        s_syncState.store(FSYNC_IDLE, std::memory_order_release);
        syncLock();
        const std::string res = s_syncResult;
        const bool ok = s_syncOk;
        const std::string newToken = s_syncNewToken;
        s_syncResult.clear();
        s_syncNewToken.clear();
        syncUnlock();
        // 登录/重登换到的新令牌由主任务写回设置（见 s_syncNewToken 的注释）。
        if (!newToken.empty()) g_settings.setFlomoToken(newToken);

        const bool reloaded = loadLocal();   // worker 只碰了磁盘，这里读回来
        // 同步期间在编辑器里写的那条（进屏那一拍被攒下了，见 screen_flomo_init）：
        // 现在库安静了，补进这份刚读回来的库。库没读出来就留着临时文件，等下次进屏。
        if (reloaded && !g.pendingTemp.empty()) applyPendingEdit();
        rebuildFilter();
        if (g.sel >= (int)g.filtered.size()) g.sel = (int)g.filtered.size() - 1;
        if (g.sel < 0) g.sel = 0;
        if (g.mode == FM_TAGS) {
            buildTagTree();
            if (g.tagSel >= (int)g.tagList.size()) g.tagSel = (int)g.tagList.size() - 1;
            if (g.tagSel < 0) g.tagSel = 0;
        }
        if (!reloaded) {
            g.status = ok ? "同步完成，但本地库读取失败（检查SD卡）"
                          : (res.empty() ? "同步失败" : res);
        } else if (ok) {
            g.status = res.empty() ? "同步完成" : res;
        } else {
            g.status = res.empty() ? "同步失败" : res;
        }
        g.drawn = false;   // 内容换了：这一拍照画
    }

    // ── 空转不重画 ────────────────────────────────────────────────────────
    // 本屏每一处状态变化都发生在按键那一拍，而且那一拍自己就画了；空转重画永远
    // 和上一幅一样，只会白烧绘制。例外：刚收工/刚进屏时 drawn 为 false。
    if (key == 0 && g.drawn) return APP_FLOMO;
    g.drawn = true;

    // 列表和长文按屏翻；其余子状态（确认框）保持单步语义。
    if (key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        const bool pageMode = (g.mode == FM_LIST || g.mode == FM_SEARCH ||
                               g.mode == FM_DETAIL || g.mode == FM_HELP ||
                               g.mode == FM_TAGS);
        if (!pageMode) key = (key == KEY_PAGE_UP) ? KEY_UP : KEY_DOWN;
    }

    switch (g.mode) {
    // ── 帮助 ──
    case FM_HELP:
        if (key == 0x1B || key == 'q' || key == 'Q' || key == 0x0A || key == 0x0D) {
            g.mode = FM_LIST;
            drawList();   // 回列表要画列表，不能把帮助再画一遍（空转不再重画）
            return APP_FLOMO;
        }
        if (key == KEY_UP && g.helpScroll > 0) {
            g.helpScroll--;
        } else if (key == KEY_DOWN) {
            g.helpScroll++;
        } else if (key == KEY_PAGE_UP) {
            g.helpScroll -= helpVisible();
            if (g.helpScroll < 0) g.helpScroll = 0;
        } else if (key == KEY_PAGE_DOWN) {
            g.helpScroll += helpVisible();  // 上限在画的时候夹住
        }
        drawHelp();
        return APP_FLOMO;

    // ── 删除确认 ──
    case FM_CONFIRM_DELETE:
        if (key == 0x0A || key == 0x0D || key == 'y' || key == 'Y') {
            deleteCurrent();
            g.mode = FM_LIST;
            drawList();
        } else if (key == 0x1B || key == 'n' || key == 'N') {
            g.mode = FM_LIST;
            drawList();
        } else {
            drawConfirmDelete();
        }
        return APP_FLOMO;

    // ── 搜索 ──
    case FM_SEARCH: {
        if (g.searchIme && key != 0) {
            std::string imeOut;
            if (g_ime.handleKey(key, imeOut)) {
                if (!imeOut.empty()) {
                    g.searchBuf.insert(g.searchCur, imeOut);
                    g.searchCur += (int)imeOut.length();
                    g.query = g.searchBuf;
                    rebuildFilter();
                    g.sel = 0; g.scroll = 0;
                }
                drawSearch();
                return APP_FLOMO;
            }
        }
        if (key == KEY_IME_TOGGLE) {
            g.searchIme = !g.searchIme;
            g_ime.setActive(g.searchIme);
            drawSearch();
            return APP_FLOMO;
        }
        if (key == KEY_FULLWIDTH_TOGGLE) { g_ime.toggleFullwidth(); drawSearch(); return APP_FLOMO; }
        if (key == 0x1B) {
            g.searchIme = false;
            g_ime.setActive(false);
            g.mode = FM_LIST;
            drawList();
            return APP_FLOMO;
        }
        if (key == 0x0A || key == 0x0D) {
            if (!g.filtered.empty()) {
                g.searchIme = false;
                g_ime.setActive(false);
                g.detailScroll = 0;
                g.mode = FM_DETAIL;
                drawDetail();   // 打开详情，不是把搜索页再画一遍
                return APP_FLOMO;
            }
            drawSearch();
            return APP_FLOMO;
        }
        if (key == KEY_LEFT) {
            if (g.searchCur > 0) {
                int prev = g.searchCur - 1;
                while (prev > 0 && ((unsigned char)g.searchBuf[prev] & 0xC0) == 0x80) prev--;
                g.searchCur = prev;
            }
        } else if (key == KEY_RIGHT) {
            if (g.searchCur < (int)g.searchBuf.size()) {
                int next = g.searchCur + 1;
                while (next < (int)g.searchBuf.size() &&
                       ((unsigned char)g.searchBuf[next] & 0xC0) == 0x80) next++;
                g.searchCur = next;
            }
        } else if (key == 0x7F || key == 0x08) {
            if (g.searchCur > 0) {
                int prev = g.searchCur - 1;
                while (prev > 0 && ((unsigned char)g.searchBuf[prev] & 0xC0) == 0x80) prev--;
                g.searchBuf.erase(prev, g.searchCur - prev);
                g.searchCur = prev;
                g.query = g.searchBuf;
                rebuildFilter();
                g.sel = 0; g.scroll = 0;
            }
        } else if (key >= 0x20 && key <= 0x7E) {
            g.searchBuf.insert(g.searchCur, 1, (char)key);
            g.searchCur++;
            g.query = g.searchBuf;
            rebuildFilter();
            g.sel = 0; g.scroll = 0;
        } else if (listNavKey(key, searchVisible())) {
            // 结果区上下移动
        }
        drawSearch();
        return APP_FLOMO;
    }

    // ── 标签树 / 筛选 ──
    case FM_TAGS: {
        const int vis = tagsVisible();
        if (key == 0x1B || key == 'q' || key == 'Q') {
            g.mode = FM_LIST;
            drawList();
            return APP_FLOMO;
        }
        if (key == 0x0A || key == 0x0D) {
            if (g.tagSel >= 0 && g.tagSel < (int)g.tagList.size())
                g.tag = g.tagList[g.tagSel].tag;
            g.sel = 0;
            g.scroll = 0;
            g.status.clear();
            rebuildFilter();
            g.mode = FM_LIST;
            drawList();
            return APP_FLOMO;
        }
        if (key == KEY_UP) {
            g.tagSel--;
        } else if (key == KEY_DOWN) {
            g.tagSel++;
        } else if (key == KEY_HOME) {
            g.tagSel = 0;
        } else if (key == KEY_END) {
            g.tagSel = (int)g.tagList.size() - 1;
        } else if (key == KEY_PAGE_UP) {
            g.tagSel -= vis;
        } else if (key == KEY_PAGE_DOWN) {
            g.tagSel += vis;
        }
        if (g.tagSel < 0) g.tagSel = 0;
        if (g.tagSel >= (int)g.tagList.size()) g.tagSel = (int)g.tagList.size() - 1;
        drawTags();
        return APP_FLOMO;
    }

    // ── 详情 ──
    case FM_DETAIL: {
        if (key == 0x1B) { g.mode = FM_LIST; drawList(); return APP_FLOMO; }
        if (key == 'e' || key == 'E') {
            if (!g.filtered.empty()) {
                const Memo &m = g.store.memos[g.filtered[g.sel]];
                return openEditorFor(ctx, m.slug, m.contentText);
            }
        }
        if (key == 'c' || key == 'C') {
            if (!g.filtered.empty()) {
                g_clipboard = g.store.memos[g.filtered[g.sel]].contentText;
                g.status = "已复制到粘贴板";
                drawDetail();
                return APP_FLOMO;
            }
        }
        if (key == 'd' || key == 'D') {
            g.mode = FM_CONFIRM_DELETE;
            drawConfirmDelete();
            return APP_FLOMO;
        }
        if (key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN ||
            key == KEY_HOME || key == KEY_END) {
            if (!g.filtered.empty()) {
                const Memo &m = g.store.memos[g.filtered[g.sel]];
                const int n = (int)detailLines(m.contentText).size();
                const int vis = detailVisible();
                int maxScroll = n - vis;
                if (maxScroll < 0) maxScroll = 0;
                if (key == KEY_UP) {
                    if (g.detailScroll > 0) g.detailScroll--;
                } else if (key == KEY_DOWN) {
                    if (g.detailScroll < maxScroll) g.detailScroll++;
                } else if (key == KEY_PAGE_UP) {
                    g.detailScroll -= vis;
                    if (g.detailScroll < 0) g.detailScroll = 0;
                } else if (key == KEY_PAGE_DOWN) {
                    g.detailScroll += vis;
                    if (g.detailScroll > maxScroll) g.detailScroll = maxScroll;
                } else if (key == KEY_HOME) {
                    g.detailScroll = 0;
                } else {
                    g.detailScroll = maxScroll;
                }
            }
        }
        drawDetail();
        return APP_FLOMO;
    }

    // ── 列表 ──
    default: break;
    }

    if (key == 0x1B || key == 'q' || key == 'Q') {
        ctx.nextState = g.returnTo;
        return g.returnTo;
    }
    if (listNavKey(key, listVisible())) {
        drawList();
        return APP_FLOMO;
    }
    if (key == 'n' || key == 'N') {
        return openEditorFor(ctx, "", "");
    }
    if (key == 'e' || key == 'E') {
        if (!g.filtered.empty()) {
            const Memo &m = g.store.memos[g.filtered[g.sel]];
            return openEditorFor(ctx, m.slug, m.contentText);
        }
    } else if (key == 'c' || key == 'C') {
        if (!g.filtered.empty()) {
            g_clipboard = g.store.memos[g.filtered[g.sel]].contentText;
            g.status = "已复制到粘贴板";
        }
    } else if (key == 'd' || key == 'D') {
        if (!g.filtered.empty()) {
            g.mode = FM_CONFIRM_DELETE;
            drawConfirmDelete();
            return APP_FLOMO;
        }
    } else if ((key == 0x0A || key == 0x0D) && !g.filtered.empty()) {
        g.detailScroll = 0;
        g.mode = FM_DETAIL;
        drawDetail();
        return APP_FLOMO;
    } else if (key == 's' || key == 'S') {
        g.mode = FM_LIST;
        g.status.clear();
        flomoTriggerSync();
        drawSyncShared();   // 立刻把进度画出来，别先在列表上闪一下
        return APP_FLOMO;
    } else if (key == 't' || key == 'T') {
        buildTagTree();
        g.tagSel = 0;
        for (int i = 1; i < (int)g.tagList.size(); ++i) {
            if (g.tagList[i].tag == g.tag) { g.tagSel = i; break; }
        }
        g.tagScroll = 0;
        g.mode = FM_TAGS;
        drawTags();
        return APP_FLOMO;
    } else if (key == '/') {
        g.searchBuf = g.query;
        g.searchCur = (int)g.searchBuf.size();
        g.searchIme = true;
        g_ime.setActive(true);
        g.sel = 0;
        g.scroll = 0;
        g.mode = FM_SEARCH;
        drawSearch();
        return APP_FLOMO;
    } else if (key == '?') {
        g.helpScroll = 0;
        g.mode = FM_HELP;
        drawHelp();
        return APP_FLOMO;
    }

    drawList();
    return APP_FLOMO;
}
