#include "screen_gtd.h"

#include "font_renderer.h"

#include "json_parser.h"

#include "journal_storage.h"

#include "ui_helpers.h"

#include "ime/IME.h"

#include <cstdio>

#include <cstring>

#include <ctime>

#include <algorithm>

#include <set>

#include <map>

#include <sys/stat.h>

#include <dirent.h>



extern u8g2_t *g_u8g2;

extern "C" {

    extern void u8g2_SetDrawColor(void *u8g2, int color);

    extern void u8g2_DrawBox(void *u8g2, int x, int y, int w, int h);

    extern void u8g2_DrawHLine(void *u8g2, int x, int y, int w);

    extern void u8g2_DrawFrame(void *u8g2, int x, int y, int w, int h);

}



// ── Constants ────────────────────────────────────────────────────────────

#define DATA_DIR  "/sdcard/gtd"

#define DATA_FILE DATA_DIR "/gtd.json"

#define ARCHIVE_DIR DATA_DIR "/archive"



static const char *VIEW_LABELS[] = {"收集箱", "下一步", "等待", "项目", "已完成"};

enum View { V_INBOX, V_NEXT, V_WAITING, V_PROJECT, V_COMPLETED, V_CONCAT };

// V_CONCAT used as count, after V_COMPLETED

static const int V_COUNT = 5;



static const char *STATUS_LABELS[] = {"todo", "doing", "done", "waiting"};

static const char *STATUS_DISPLAY[] = {"待办", "进行中", "已完成", "等待中"};

static const char *PRIORITY_LABELS[] = {"A", "B", "C"};

static const char *PRIORITY_DISPLAY[] = {"A 高", "B 中", "C 低"};



enum Mode { M_BROWSE, M_ADD, M_DETAIL, M_EDIT_FIELD, M_FILTER, M_RENAME, M_CONFIRM, M_ADD_PROJECT, M_RENAME_PROJECT, M_PICKER, M_CALENDAR, M_HELP, M_EDIT_NOTE, M_CONTEXT_MGR, M_TAG_MGR, M_ADD_CONTEXT, M_ADD_TAG, M_RENAME_CONTEXT, M_RENAME_TAG, M_SUMMARY, M_ARCHIVE };



// Picker option for popup panel

struct PickerOpt {

    std::string value;   // actual JSON value

    std::string display; // display text

};



// ── State ────────────────────────────────────────────────────────────────

static struct {

    int view = 0;

    int sel = 0;

    int scroll = 0;

    int mode = M_BROWSE;

    JsonValue data;           // full task data from file

    std::vector<int> filtered; // indices into data["tasks"] array



    // detail / edit

    int detailTaskIdx = -1;   // index in data["tasks"]

    int detailField = 0;



    // text editing

    std::string editBuf;

    int editCur = 0;

    bool imeActive = false;

    std::string pendingParent;

    std::string pendingProject;  // auto-set project for new task in project view



    // filter

    std::string filterText;



    // Project drill-down

    std::vector<std::string> projectList;

    int projectDrillIdx = -1;  // selected project index in projectList, -1 = show list



    // Insertion position for a/i key (index in tasks array, -1 = append)

    int insertAfter = -1;



    // project selector index (for 'j' type field editing)

    int projectSelIdx = -1;



    // picker popup

    std::vector<PickerOpt> pickerOpts;

    int pickerSel = 0;

    int pickerField = -1;  // which detail field the picker is for



    // calendar popup

    int calYear = 2026;

    int calMonth = 1;

    int calDay = 1;

    int calSelDay = 1;



    // help dialog

    int helpScroll = 0;

    int helpPrevMode = M_BROWSE;



    // note editor

    std::vector<std::string> noteLines;

    int noteRow = 0;    // cursor line

    int noteCol = 0;    // cursor byte offset in line

    int noteScroll = 0;

    std::vector<VRow> noteVrows;

    bool noteVrowsDirty = true;



    // context/tag management

    std::vector<std::string> contextList;

    std::vector<std::string> tagList;

    std::string filterContext;

    std::vector<std::string> filterTags;

    int ctxMgrSel = 0;

    int tagMgrSel = 0;

    std::set<int> pickerToggled;  // for multi-select tag picker

    std::string renameTargetContext;

    std::string renameTargetTag;



    // summary dialog

    int summaryScroll = 0;
    int summaryPrevMode = M_BROWSE;



    // confirm dialog

    std::string confirmMsg;

    int confirmIdx = -1;

    std::vector<int> confirmIdxs;  // 批量删除确认的任务下标(M_CONFIRM 用)



    // multi-select (Shift+↑↓ 连续多选)

    std::set<std::string> multiSel;  // 选中的任务 id 集合

    int multiAnchor = -1;            // 选择锚点(g.filtered 显示下标), -1=未激活



    // project rename target

    std::string renameTargetProject;



    // fold state (indices into g_gtdTree that are collapsed)

    std::set<int> foldedNodes;



    // archive manager

    std::vector<std::string> archiveMonths;  // "YYYY-MM" list

    std::vector<int> archiveCounts;          // task count per month

    std::vector<JsonValue> archiveTasks;     // tasks of selected month (read-only browse)

    int archiveSel = 0;

    int archiveScroll = 0;

    int archiveViewSel = 0;   // index in archiveTasks when browsing

    int archiveViewScroll = 0;

    bool archiveBrowsing = false;  // false = month list, true = task list

    std::string archiveViewMonth;  // currently viewed month

} g;

// 仅列表浏览模式启用物理按键导航快捷键,避免与文本输入冲突
bool screen_gtd_accept_physical_buttons() { return g.mode == M_BROWSE; }

// 项目标签顶层(项目选择列表)
bool screen_gtd_in_project_list() {
    return g.mode == M_BROWSE && g.view == V_PROJECT && g.projectDrillIdx < 0;
}

// 进度百分比(0-100) → 图标(U+E004=0%, U+E005..U+E00C=1/8..8/8)
static const char *progressIconStr(int pct) {
    static const char *icons[9] = { "\xEE\x80\x84", "\xEE\x80\x85", "\xEE\x80\x86", "\xEE\x80\x87",
                                    "\xEE\x80\x88", "\xEE\x80\x89", "\xEE\x80\x8A", "\xEE\x80\x8B",
                                    "\xEE\x80\x8C" };
    if (pct <= 0) return icons[0];
    int lvl = (pct * 2 + 24) / 25;
    if (lvl > 8) lvl = 8;
    return icons[lvl];
}

// GTD 状态栏右侧: 分隔符图标 + 当前日期(月-日) + 设备电量 (日期左边始终有分隔符)
static void gtdStatusRight(char *buf, size_t sz) {
    time_t now; time(&now);
    struct tm *tm = localtime(&now);
    char date[8];
    strftime(date, sizeof(date), "%m-%d", tm);
    std::string bt = battery_text();
    if (!bt.empty())
        snprintf(buf, sz, "\xEE\x80\x83%s %s", date, bt.c_str());
    else
        snprintf(buf, sz, "\xEE\x80\x83%s", date);
}



// ── Tree view (for Project tab drill-down) ──────────────────────────────

struct GtdTreeItem {

    int taskIdx;

    int depth;

    bool isLast;

    std::vector<bool> ancLast; // ancestor-is-last at each level 0..depth-1

};

static std::vector<GtdTreeItem> g_gtdTree;
static std::vector<int> g_visibleTreeIdx; // indices into g_gtdTree, after fold filtering



static void gtdTreeAddChildren(const JsonValue &tasks, int startFi,

    int depth, const std::vector<bool> &ancLast,

    const std::vector<int> &scope, std::vector<bool> &used)

{

    if (depth > 50) return;

    std::string parentId = tasks[scope[startFi]]["id"].asString();



    std::vector<int> childFis;

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        if (used[fi]) continue;

        if (tasks[scope[fi]]["parent"].asString() == parentId)

            childFis.push_back(fi);

    }



    for (size_t ci = 0; ci < childFis.size(); ci++) {

        int fi = childFis[ci];

        used[fi] = true;



        GtdTreeItem item;

        item.taskIdx = scope[fi];

        item.depth = depth;

        item.isLast = (ci == childFis.size() - 1);

        item.ancLast = ancLast;

        g_gtdTree.push_back(item);



        std::vector<bool> childAnc = ancLast;

        childAnc.push_back(item.isLast);

        gtdTreeAddChildren(tasks, fi, depth + 1, childAnc, scope, used);

    }

}



static void buildGtdTree() {

    g_gtdTree.clear();

    if (g.view != V_PROJECT || g.projectDrillIdx < 0) return;

    auto &tasks = g.data["tasks"];

    if (!tasks.isArray() || g.filtered.empty()) return;



    // Collect IDs in the project scope for parent-exists checks

    std::set<std::string> idsInScope;

    for (int fi : g.filtered) {

        idsInScope.insert(tasks[fi]["id"].asString());

    }



    // Copy filtered list as a scope for tree building

    std::vector<int> scope = g.filtered;

    std::vector<bool> used(scope.size(), false);



    // Pass 1: add roots (empty parent, or parent not in scope)

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        if (used[fi]) continue;

        std::string pid = tasks[scope[fi]]["parent"].asString();

        if (pid.empty() || idsInScope.find(pid) == idsInScope.end()) {

            used[fi] = true;

            GtdTreeItem item;

            item.taskIdx = scope[fi];

            item.depth = 0;

            item.isLast = true; // tentative

            g_gtdTree.push_back(item);

        }

    }



    // Pass 2: add children recursively for each root

    // Re-walk the tree to fix isLast and add children properly

    // (Simpler: rebuild from scratch using the recursive approach)

    g_gtdTree.clear();



    // Rebuild properly with recursive children

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        if (used[fi]) { // still true from Pass 1 for those we marked

            // These are roots — process them

        }

    }



    // Start fresh

    std::vector<bool> used2(scope.size(), false);

    std::vector<int> roots;

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        std::string pid = tasks[scope[fi]]["parent"].asString();

        if (pid.empty() || idsInScope.find(pid) == idsInScope.end()) {

            roots.push_back(fi);

        }

    }

    // Determine isLast for each root

    for (size_t ri = 0; ri < roots.size(); ri++) {

        int fi = roots[ri];

        used2[fi] = true;

        GtdTreeItem item;

        item.taskIdx = scope[fi];

        item.depth = 0;

        item.isLast = (ri == roots.size() - 1);

        g_gtdTree.push_back(item);



        std::vector<bool> childAnc;

        childAnc.push_back(item.isLast);

        gtdTreeAddChildren(tasks, fi, 1, childAnc, scope, used2);

    }



    // Pass 3: any remaining orphans (circular refs etc.) as roots

    for (int fi = 0; fi < (int)scope.size(); fi++) {

        if (used2[fi]) continue;

        used2[fi] = true;

        GtdTreeItem item;

        item.taskIdx = scope[fi];

        item.depth = 0;

        item.isLast = true;

        g_gtdTree.push_back(item);

    }

}



// ── Filter helpers ───────────────────────────────────────────────────────

static bool taskMatchesView(const JsonValue &task, int view) {

    std::string status = task["status"].asString("todo");

    if (view == V_INBOX)     return status == "todo";

    if (view == V_NEXT)      return status == "doing";

    if (view == V_WAITING)   return status == "waiting";

    if (view == V_PROJECT)   return !task["project"].asString().empty();

    if (view == V_COMPLETED) return status == "done";

    return false;

}



static bool isInProjectList() { return g.view == V_PROJECT && g.projectDrillIdx < 0; }

// ── Multi-select (Shift+↑↓) ────────────────────────────────────────────
static bool hasMultiSel() { return !g.multiSel.empty(); }

static void clearMultiSel() {
    g.multiSel.clear();
    g.multiAnchor = -1;
}

// 由锚点..光标的连续区间从 g.filtered 重建选中任务 id 集合
static void rebuildMultiSel() {
    g.multiSel.clear();
    if (g.multiAnchor < 0) return;
    int a = std::min(g.multiAnchor, g.sel);
    int b = std::max(g.multiAnchor, g.sel);
    auto &tasks = g.data["tasks"];
    for (int i = a; i <= b && i < (int)g.filtered.size(); i++) {
        g.multiSel.insert(tasks[g.filtered[i]]["id"].asString());
    }
}

// displayIdx 位置的条目是否处于多选中
static bool isMultiSelected(int displayIdx) {
    if (g.multiSel.empty()) return false;
    auto &tasks = g.data["tasks"];
    if (displayIdx < 0 || displayIdx >= (int)g.filtered.size()) return false;
    return g.multiSel.count(tasks[g.filtered[displayIdx]]["id"].asString()) > 0;
}

static void rebuildFilter() {

    g.filtered.clear();

    auto &tasks = g.data["tasks"];

    if (!tasks.isArray()) return;

    for (int i = 0; i < (int)tasks.size(); i++) {

        auto &t = tasks[i];

        if (!taskMatchesView(t, g.view)) continue;

        if (g.view == V_PROJECT && g.projectDrillIdx >= 0) {

            std::string proj = t["project"].asString();

            if (proj.empty() || proj != g.projectList[g.projectDrillIdx])

                continue;

        }

        if (!g.filterText.empty()) {

            std::string title = t["title"].asString();

            std::string note  = t["note"].asString();

            if (title.find(g.filterText) == std::string::npos &&

                note.find(g.filterText) == std::string::npos)

                continue;

        }

        if (!g.filterContext.empty()) {

            if (t["context"].asString() != g.filterContext)

                continue;

        }

        if (!g.filterTags.empty()) {

            auto &tt = t["tags"];

            bool hasAll = true;

            for (auto &ft : g.filterTags) {

                bool found = false;

                if (tt.isArray()) {

                    for (int j = 0; j < (int)tt.size(); j++) {

                        if (tt[j].asString() == ft) { found = true; break; }

                    }

                }

                if (!found) { hasAll = false; break; }

            }

            if (!hasAll) continue;

        }

        g.filtered.push_back(i);

    }

    if (g.sel >= (int)g.filtered.size()) g.sel = (int)g.filtered.size() - 1;

    if (g.sel < 0) g.sel = 0;



    // Build tree view and reorder filtered to tree order for project drill-down

    if (g.view == V_PROJECT && g.projectDrillIdx >= 0) {

        buildGtdTree();

        // Apply fold: build visible tree index list (skipping children of folded nodes)
        g_visibleTreeIdx.clear();
        std::set<int> hiddenByFold;
        for (int fi : g.foldedNodes) {
            if (fi < 0 || fi >= (int)g_gtdTree.size()) continue;
            int foldDepth = g_gtdTree[fi].depth;
            for (int j = fi + 1; j < (int)g_gtdTree.size(); j++) {
                if (g_gtdTree[j].depth <= foldDepth) break;
                hiddenByFold.insert(j);
            }
        }
        for (int i = 0; i < (int)g_gtdTree.size(); i++) {
            if (!hiddenByFold.count(i))
                g_visibleTreeIdx.push_back(i);
        }

        g.filtered.clear();
        for (int vi : g_visibleTreeIdx)
            g.filtered.push_back(g_gtdTree[vi].taskIdx);

        if (g.sel >= (int)g.filtered.size()) g.sel = (int)g.filtered.size() - 1;

        if (g.sel < 0) g.sel = 0;

    }

}



static void buildProjectList() {

    g.projectList.clear();

    // Add projects from stored "projects" array

    auto &projs = g.data["projects"];

    if (projs.isArray()) {

        for (int i = 0; i < (int)projs.size(); i++) {

            std::string name = projs[i].asString();

            if (name.empty()) continue;

            bool dup = false;

            for (auto &p : g.projectList) if (p == name) { dup = true; break; }

            if (!dup) g.projectList.push_back(name);

        }

    }

    // Add projects derived from tasks

    auto &tasks = g.data["tasks"];

    if (tasks.isArray()) {

        for (int i = 0; i < (int)tasks.size(); i++) {

            std::string proj = tasks[i]["project"].asString();

            if (proj.empty()) continue;

            bool dup = false;

            for (auto &p : g.projectList) if (p == proj) { dup = true; break; }

            if (!dup) g.projectList.push_back(proj);

        }

    }

    std::sort(g.projectList.begin(), g.projectList.end());

}

// 物理按键双击 BOOT: 项目树内返回项目选择菜单; 其他视图无动作
void screen_gtd_physical_double_boot() {
    if (g.mode != M_BROWSE) return;
    if (g.view == V_PROJECT && g.projectDrillIdx >= 0) {
        g.projectDrillIdx = -1;
        g.sel = 0; g.scroll = 0;
        g.foldedNodes.clear();
        clearMultiSel();
        buildProjectList();
        rebuildFilter();
    }
}



static void buildContextList() {

    g.contextList.clear();

    auto &ctxs = g.data["contexts"];

    if (ctxs.isArray()) {

        for (int i = 0; i < (int)ctxs.size(); i++) {

            std::string name = ctxs[i].asString();

            if (name.empty()) continue;

            bool dup = false;

            for (auto &c : g.contextList) if (c == name) { dup = true; break; }

            if (!dup) g.contextList.push_back(name);

        }

    }

    auto &tasks = g.data["tasks"];

    if (tasks.isArray()) {

        for (int i = 0; i < (int)tasks.size(); i++) {

            std::string ctx = tasks[i]["context"].asString();

            if (ctx.empty()) continue;

            bool dup = false;

            for (auto &c : g.contextList) if (c == ctx) { dup = true; break; }

            if (!dup) g.contextList.push_back(ctx);

        }

    }

    std::sort(g.contextList.begin(), g.contextList.end());

}



static void buildTagList() {

    g.tagList.clear();

    auto &tags = g.data["tags"];

    if (tags.isArray()) {

        for (int i = 0; i < (int)tags.size(); i++) {

            std::string name = tags[i].asString();

            if (name.empty()) continue;

            bool dup = false;

            for (auto &t : g.tagList) if (t == name) { dup = true; break; }

            if (!dup) g.tagList.push_back(name);

        }

    }

    auto &tasks = g.data["tasks"];

    if (tasks.isArray()) {

        for (int i = 0; i < (int)tasks.size(); i++) {

            auto &tt = tasks[i]["tags"];

            if (tt.isArray()) {

                for (int j = 0; j < (int)tt.size(); j++) {

                    std::string name = tt[j].asString();

                    if (name.empty()) continue;

                    bool dup = false;

                    for (auto &t : g.tagList) if (t == name) { dup = true; break; }

                    if (!dup) g.tagList.push_back(name);

                }

            }

        }

    }

    std::sort(g.tagList.begin(), g.tagList.end());

}



// ── Data I/O ─────────────────────────────────────────────────────────────

static void loadData() {

    auto v = JsonValue::loadFromFile(DATA_FILE);

    if (v.isNull() || !v.has("tasks") || !v["tasks"].isArray()) {

        g.data = JsonValue::object();

        g.data.set("tasks", JsonValue::array());

    } else {

        g.data = v;

        // Purge null-type tasks from old bug

        auto &tasks = g.data["tasks"];

        int write = 0;

        for (int i = 0; i < (int)tasks.size(); i++) {

            if (!tasks[i].isNull())

                tasks.elements[write++] = tasks[i];

        }

        tasks.elements.resize(write);

    }

    if (!g.data.has("projects") || !g.data["projects"].isArray())

        g.data.set("projects", JsonValue::array());

    if (!g.data.has("contexts") || !g.data["contexts"].isArray())

        g.data.set("contexts", JsonValue::array());

    if (!g.data.has("tags") || !g.data["tags"].isArray())

        g.data.set("tags", JsonValue::array());

    g.view = 0; g.sel = 0; g.scroll = 0;

    rebuildFilter();

}



static void saveData() {

    if (!JsonValue::saveToFile(DATA_FILE, g.data)) {

        // Save failed — try ensuring directory exists and retry once

        mkdir(DATA_DIR, 0755);

        JsonValue::saveToFile(DATA_FILE, g.data);

    }

}



// ── Auto-archive ─────────────────────────────────────────────────────────

static std::string currentMonthStr() {
    time_t now; time(&now); struct tm *tm = localtime(&now);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d", tm->tm_year + 1900, tm->tm_mon + 1);
    return buf;
}

static void autoArchive() {
    std::string curMonth = currentMonthStr();
    std::string lastArchive = g.data["lastArchive"].asString();

    // Already archived this month
    if (lastArchive == curMonth) return;

    mkdir(ARCHIVE_DIR, 0755);

    auto &tasks = g.data["tasks"];
    if (!tasks.isArray()) return;

    // Group completed tasks by their completed-month
    // key = "YYYY-MM", value = array of task indices (in reverse for safe erase)
    std::map<std::string, std::vector<int>> byMonth;
    for (int i = 0; i < (int)tasks.size(); i++) {
        if (tasks[i]["status"].asString("todo") != "done") continue;
        std::string completed = tasks[i]["completed"].asString();
        if (completed.empty()) continue;
        // completed format: "YYYY-MM-DD"
        if (completed.length() < 7) continue;
        std::string month = completed.substr(0, 7);
        // Only archive tasks from previous months or earlier
        if (month >= curMonth) continue;
        byMonth[month].push_back(i);
    }

    bool anyArchived = false;
    for (auto &kv : byMonth) {
        std::string month = kv.first;
        std::vector<int> &indices = kv.second;

        // Load existing archive file (if any) and merge
        std::string arcPath = std::string(ARCHIVE_DIR) + "/" + month + ".json";
        JsonValue arcData = JsonValue::loadFromFile(arcPath);
        if (arcData.isNull() || !arcData.has("tasks") || !arcData["tasks"].isArray()) {
            arcData = JsonValue::object();
            arcData.set("tasks", JsonValue::array());
            arcData.set("archiveDate", month);
        }
        auto &arcTasks = arcData["tasks"];

        // Append archived tasks
        for (int idx : indices)
            arcTasks.pushBack(tasks[idx]);

        JsonValue::saveToFile(arcPath, arcData);
        anyArchived = true;
    }

    // Remove archived tasks from main data (erase from back to front)
    std::vector<int> allToRemove;
    for (auto &kv : byMonth)
        for (int idx : kv.second)
            allToRemove.push_back(idx);
    std::sort(allToRemove.rbegin(), allToRemove.rend());
    for (int idx : allToRemove)
        tasks.elements.erase(tasks.elements.begin() + idx);

    g.data.set("lastArchive", curMonth);
    if (anyArchived) saveData();
}



// ── ID generator ─────────────────────────────────────────────────────────

static std::string makeId() {

    time_t now; time(&now); struct tm *tm = localtime(&now);

    char buf[32];

    static int seq = 0;

    snprintf(buf, sizeof(buf), "%02d%02d%02d_%02d%02d_%d",

             tm->tm_year % 100, tm->tm_mon + 1, tm->tm_mday,

             tm->tm_hour, tm->tm_min, seq++);

    return buf;

}

// ── Export markdown ──────────────────────────────────────────────────────

static std::string exportMD() {

    std::string md = "# GTD 任务列表\n\n";

    auto &tasks = g.data["tasks"];

    if (!tasks.isArray()) return md;

    for (int i = 0; i < (int)tasks.size(); i++) {

        auto &t = tasks[i];

        std::string status = t["status"].asString("todo");

        std::string pri = t["priority"].asString();

        std::string title = t["title"].asString();



        const char *mark = "[ ]";

        if (status == "doing") mark = "[→]";

        else if (status == "done") mark = "[✓]";

        else if (status == "waiting") mark = "[~]";



        md += "- " + std::string(mark) + " " + title;

        if (!pri.empty()) md += " **" + pri + "**";

        md += "\n";

    }

    return md;

}



// ── UI drawing ───────────────────────────────────────────────────────────



#define IME_CODE_Y (STATUS_Y - 2*FONT_H + g_font.ascent())

#define IME_CAND_Y (STATUS_Y - FONT_H + g_font.ascent() - 3)



static void drawIMEStatus() {

    if (!g_ime.composing()) return;

    std::string code = g_ime.displayCode();

    int pageSize = g_ime.pageSize();

    int curPage = g_ime.currentPage();

    int totalPages = g_ime.totalPages();

    if (totalPages < 1) totalPages = 1;

    char pageInfo[32];

    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);

    int sepY = IME_CODE_Y - 4;

    int codeBaseline = sepY - 7;

    {

        int cw = g_font.textWidth(code.c_str()) + 8;

        u8g2_SetDrawColor(g_u8g2, 1);

        u8g2_DrawBox(g_u8g2, 4, codeBaseline - g_font.ascent(), cw, FONT_H);

        u8g2_SetDrawColor(g_u8g2, 0);

        g_font.drawText(4, codeBaseline, code.c_str(), false);

        u8g2_SetDrawColor(g_u8g2, 1);

    }

    {

        int tw = g_font.textWidth(pageInfo);

        int pw = tw + 8;

        int px = SCREEN_W - pw - 4;

        u8g2_SetDrawColor(g_u8g2, 1);

        u8g2_DrawBox(g_u8g2, px, codeBaseline - g_font.ascent(), pw, FONT_H);

        u8g2_SetDrawColor(g_u8g2, 0);

        g_font.drawText(px + 4, codeBaseline, pageInfo, false);

        u8g2_SetDrawColor(g_u8g2, 1);

    }

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

    u8g2_SetDrawColor(g_u8g2, 1);

    auto &cands = g_ime.candidates();

    std::string candLine;

    for (int i = 0; i < (int)cands.size(); i++) {

        char idx[16];

        snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);

        std::string part = std::string(" ") + idx + cands[i];

        int curW = g_font.textWidth(candLine.c_str());

        int partW = g_font.textWidth(part.c_str());

        if (curW + partW + 8 > SCREEN_W) break;

        candLine += part;

    }

    {

        int cw = g_font.textWidth(candLine.c_str()) + 8;

        u8g2_SetDrawColor(g_u8g2, 1);

        u8g2_DrawBox(g_u8g2, 4, IME_CAND_Y - g_font.ascent(), cw, FONT_H);

        u8g2_SetDrawColor(g_u8g2, 0);

        g_font.drawText(4, IME_CAND_Y, candLine.c_str(), false);

        u8g2_SetDrawColor(g_u8g2, 1);

    }

}



static void drawTabBar() {

    int x = 0;

    int tw = SCREEN_W / V_COUNT;

    for (int v = 0; v < V_COUNT; v++) {

        bool act = (v == g.view);

        if (act) {

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, x, 0, tw, FONT_H + 2);

            u8g2_SetDrawColor(g_u8g2, 1);

            g_font.drawText(x + (tw - g_font.textWidth(VIEW_LABELS[v])) / 2,

                            g_font.ascent(), VIEW_LABELS[v], false);

        } else {

            u8g2_SetDrawColor(g_u8g2, 0);

            g_font.drawText(x + (tw - g_font.textWidth(VIEW_LABELS[v])) / 2,

                            g_font.ascent(), VIEW_LABELS[v], false);

        }

        x += tw;

    }

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 2, SCREEN_W);

}



static const char *statusIcon(const std::string &s) {

    if (s == "doing")   return "[→]";

    if (s == "done")    return "[✓]";

    if (s == "waiting") return "[~]";

    return "[ ]";

}



static void drawDetail();  // forward declaration

static void drawList();    // forward declaration

static void drawHelp();    // forward declaration



// ── Archive manager ──────────────────────────────────────────────────────

static void loadArchiveMonths() {
    g.archiveMonths.clear();
    g.archiveCounts.clear();
    DIR *dir = opendir(ARCHIVE_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() > 5 && name.substr(name.size() - 5) == ".json") {
            std::string month = name.substr(0, name.size() - 5);
            // Count tasks
            std::string path = std::string(ARCHIVE_DIR) + "/" + name;
            JsonValue data = JsonValue::loadFromFile(path);
            int count = 0;
            if (data.has("tasks") && data["tasks"].isArray()) count = (int)data["tasks"].size();
            g.archiveMonths.push_back(month);
            g.archiveCounts.push_back(count);
        }
    }
    closedir(dir);
    // Sort both vectors together (newest first)
    std::vector<int> order(g.archiveMonths.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [](int a, int b) {
        return g.archiveMonths[a] > g.archiveMonths[b];
    });
    std::vector<std::string> sm = g.archiveMonths;
    std::vector<int> sc = g.archiveCounts;
    for (size_t i = 0; i < order.size(); i++) {
        g.archiveMonths[i] = sm[order[i]];
        g.archiveCounts[i] = sc[order[i]];
    }
}

static void loadArchiveMonthTasks(const std::string &month) {
    g.archiveTasks.clear();
    std::string path = std::string(ARCHIVE_DIR) + "/" + month + ".json";
    JsonValue data = JsonValue::loadFromFile(path);
    if (!data.isNull() && data.has("tasks") && data["tasks"].isArray()) {
        auto &tasks = data["tasks"];
        for (int i = 0; i < (int)tasks.size(); i++)
            g.archiveTasks.push_back(tasks[i]);
    }
}

static void drawArchiveMgr() {
    ui_clear();

    if (!g.archiveBrowsing) {
        // Month list
        ui_draw_text(4, g_font.ascent(), "归档管理", false, true);
        u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

        int y = FONT_H + 8 + LINE_SPACING;
        int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
        if (vis < 1) vis = 1;
        if (g.archiveSel < g.archiveScroll) g.archiveScroll = g.archiveSel;
        if (g.archiveSel >= g.archiveScroll + vis) g.archiveScroll = g.archiveSel - vis + 1;

        if (g.archiveMonths.empty()) {
            ui_draw_text(8, y, "暂无归档");
        }
        for (int i = 0; i < vis && (g.archiveScroll + i) < (int)g.archiveMonths.size(); i++) {
            int mi = g.archiveScroll + i;
            bool sel = (mi == g.archiveSel);
            std::string month = g.archiveMonths[mi];
            int count = g.archiveCounts[mi];

            char buf[64];
            snprintf(buf, sizeof(buf), "%s  (%d项)", month.c_str(), count);
            ui_draw_text(8, y + i * LINE_SPACING, buf, sel);
        }

        char rbuf[24]; gtdStatusRight(rbuf, sizeof(rbuf));

        ui_draw_status("?:帮助|Enter展开 d:删除 Esc返回", rbuf);
    } else {
        // Task list for selected month
        char title[48];
        snprintf(title, sizeof(title), "归档: %s", g.archiveViewMonth.c_str());
        ui_draw_text(4, g_font.ascent(), title, false, true);
        u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

        int y = FONT_H + 8 + LINE_SPACING;
        int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;
        if (vis < 1) vis = 1;
        if (g.archiveViewSel < g.archiveViewScroll) g.archiveViewScroll = g.archiveViewSel;
        if (g.archiveViewSel >= g.archiveViewScroll + vis) g.archiveViewScroll = g.archiveViewSel - vis + 1;

        for (int i = 0; i < vis && (g.archiveViewScroll + i) < (int)g.archiveTasks.size(); i++) {
            int ti = g.archiveViewScroll + i;
            bool sel = (ti == g.archiveViewSel);
            auto &t = g.archiveTasks[ti];
            std::string status = t["status"].asString("todo");
            std::string ttl = t["title"].asString();
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %s", statusIcon(status), ttl.c_str());
            ui_draw_text(8, y + i * LINE_SPACING, buf, sel);
        }

        if (g.archiveTasks.empty()) ui_draw_text(8, y, "(空)");

        char sl[48];
        snprintf(sl, sizeof(sl), "?:帮助|%d项 Esc返回", (int)g.archiveTasks.size());
        char rbuf[24]; gtdStatusRight(rbuf, sizeof(rbuf));
        ui_draw_status(sl, rbuf);
    }

    drawIMEStatus(); ui_commit();
}



static void drawList() {

    ui_clear();

    drawTabBar();



    int y = FONT_H + 6 + LINE_SPACING;



    // Project list (top level)

    if (g.view == V_PROJECT && g.projectDrillIdx < 0) {

        buildProjectList();

        int vis = (STATUS_Y - y + LINE_SPACING - 1) / LINE_SPACING;

        if (vis < 1) vis = 1;

        if (g.sel < g.scroll) g.scroll = g.sel;

        if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;

        for (int i = 0; i < vis && (g.scroll + i) < (int)g.projectList.size(); i++) {

            bool sel = (g.scroll + i == g.sel);

            char buf[64];

            snprintf(buf, sizeof(buf), "◆ %s", g.projectList[g.scroll + i].c_str());

            ui_draw_text(4, y + i * LINE_SPACING, buf, sel);

        }

        if (g.projectList.empty()) {

            ui_draw_text(4, y, "暂无项目 — 按n新建");

        }

        char sl[32];
        snprintf(sl, sizeof(sl), "?:帮助|%d个项目", (int)g.projectList.size());
        char rbuf[24]; gtdStatusRight(rbuf, sizeof(rbuf));
        ui_draw_status(sl, rbuf);

        drawIMEStatus();

        return;

    }



    // Project tree view (drill-down)

    if (g.view == V_PROJECT && g.projectDrillIdx >= 0) {

        if (g_gtdTree.empty()) {

            ui_draw_text(8, y, "此项目暂无任务 — 按a添加");

            char sl[96];

            snprintf(sl, sizeof(sl), "a:添加 Tab:切换");

            char rbuf[24]; gtdStatusRight(rbuf, sizeof(rbuf));

            ui_draw_status(sl, rbuf);

            drawIMEStatus();

            return;

        }

        auto &tasks = g.data["tasks"];

        int maxY = g_ime.composing() ? (IME_CODE_Y - 4) : STATUS_Y;

        int vis = (maxY - y + LINE_SPACING - 1) / LINE_SPACING;

        if (vis < 1) vis = 1;

        if (g.sel < g.scroll) g.scroll = g.sel;

        if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;



        for (int i = 0; i < vis && (g.scroll + i) < (int)g_visibleTreeIdx.size(); i++) {

            int treeIdx = g_visibleTreeIdx[g.scroll + i];
            auto &ti = g_gtdTree[treeIdx];

            bool sel = (g.scroll + i == g.sel);

            auto &t = tasks[ti.taskIdx];

            std::string title = t["title"].asString();

            std::string status = t["status"].asString("todo");

            std::string pri = t["priority"].asString();



            // Check if this node has children in the tree
            bool hasChildren = (treeIdx + 1 < (int)g_gtdTree.size() &&
                                g_gtdTree[treeIdx + 1].depth > ti.depth);
            bool isFolded = g.foldedNodes.count(treeIdx) > 0;

            // Build tree prefix using ├─ └─ │ symbols

            std::string prefix;

            for (int a = 0; a < ti.depth; a++)

                prefix += ti.ancLast[a] ? "  " : "│ ";

            // 折叠项:三角在缩进线位置(替换 ◆/├);无折叠项:根用 ◆,子项用 ├─/└─
            if (hasChildren) prefix += isFolded ? "▸ " : "▾ ";
            else if (ti.depth > 0) prefix += ti.isLast ? "└─ " : "├─ ";
            else prefix += "◆ ";



            char line[96];

            snprintf(line, sizeof(line), "%s%s %s", prefix.c_str(), statusIcon(status), title.c_str());

            ui_draw_text(4, y + i * LINE_SPACING, line, sel);



            // Right-side info: progress + priority badge

            int pri_w = 0, pri_x = SCREEN_W;

            if (!pri.empty()) {

                pri_w = g_font.textWidth(pri.c_str()) + 4;

                pri_x = SCREEN_W - pri_w - 4;

            }



            // Progress before priority

            int pct = (int)t["progress"].asNumber(0);

            if (pct > 0) {

                const char *buf = progressIconStr(pct);

                int info_w = g_font.textWidth(buf);

                int info_x = pri_x - 6 - info_w;

                g_font.drawText(info_x, y + i * LINE_SPACING, buf, false);

            }



            if (!pri.empty()) {

                u8g2_SetDrawColor(g_u8g2, 0);

                u8g2_DrawBox(g_u8g2, pri_x, y + i * LINE_SPACING - g_font.ascent(), pri_w, FONT_H);

                u8g2_SetDrawColor(g_u8g2, 1);

                g_font.drawText(pri_x + 2, y + i * LINE_SPACING, pri.c_str(), true);

                u8g2_SetDrawColor(g_u8g2, 0);

            }

        }



        // Status bar: left=help hint | selected task info
        char statusLine[128];
        statusLine[0] = '\0';
        if (g.sel >= 0 && g.sel < (int)g_visibleTreeIdx.size()) {
            auto &st = tasks[g_gtdTree[g_visibleTreeIdx[g.sel]].taskIdx];
            std::string parts;
            std::string sCtx = st["context"].asString();
            if (!sCtx.empty()) parts += "@" + sCtx + " ";
            auto &stt = st["tags"];
            if (stt.isArray()) {
                for (int j = 0; j < (int)stt.size(); j++) {
                    std::string tn = stt[j].asString();
                    if (!tn.empty()) parts += "#" + tn + " ";
                }
            }
            std::string sDue = st["due"].asString();
            if (!sDue.empty()) {
                size_t nd = 0;
                for (char c : sDue) if (c == '-') nd++;
                std::string dd = (nd >= 2 && sDue.length() >= 5) ? sDue.substr(sDue.length() - 5) : sDue;
                if (!dd.empty()) parts += dd + " ";
            }
            while (!parts.empty() && parts.back() == ' ') parts.pop_back();
            if (!parts.empty())
                snprintf(statusLine, sizeof(statusLine), "?:帮助|%s", parts.c_str());
        }
        if (statusLine[0] == '\0')
            snprintf(statusLine, sizeof(statusLine), "?:帮助");
        char rbuf[24]; gtdStatusRight(rbuf, sizeof(rbuf));
        ui_draw_status(statusLine, rbuf);

        drawIMEStatus();

        return;

    }



    int maxY = g_ime.composing() ? (IME_CODE_Y - 4) : STATUS_Y;

    int vis = (maxY - y + LINE_SPACING - 1) / LINE_SPACING;

    if (vis < 1) vis = 1;



    if (g.sel < g.scroll) g.scroll = g.sel;

    if (g.sel >= g.scroll + vis) g.scroll = g.sel - vis + 1;



    auto &tasks = g.data["tasks"];

    for (int i = 0; i < vis && (g.scroll + i) < (int)g.filtered.size(); i++) {

        int ti = g.filtered[g.scroll + i];

        auto &t = tasks[ti];

        bool sel = (g.scroll + i == g.sel);

        bool msel = (!sel && isMultiSelected(g.scroll + i));

        std::string title = t["title"].asString();

        std::string status = t["status"].asString("todo");

        std::string pri = t["priority"].asString();

        std::string parent = t["parent"].asString();



        char line[160];

        snprintf(line, sizeof(line), "%s%s %s", msel ? "✓ " : "", statusIcon(status), title.c_str());



        int indent = 0;
        if (g.view == V_PROJECT && !parent.empty()) indent = 1;

        int lx = 8 + indent * 12;



        ui_draw_text(lx, y + i * LINE_SPACING, line, sel);



        // Right-side info: progress + priority badge

        int pri_w = 0, pri_x = SCREEN_W;

        if (!pri.empty()) {

            pri_w = g_font.textWidth(pri.c_str()) + 4;

            pri_x = SCREEN_W - pri_w - 4;

        }



        // Progress before priority

        int pct = (int)t["progress"].asNumber(0);

        if (pct > 0) {

            const char *buf = progressIconStr(pct);

            int info_w = g_font.textWidth(buf);

            int info_x = pri_x - 6 - info_w;

            g_font.drawText(info_x, y + i * LINE_SPACING, buf, false);

        }



        if (!pri.empty()) {

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, pri_x, y + i * LINE_SPACING - g_font.ascent(), pri_w, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 1);

            g_font.drawText(pri_x + 2, y + i * LINE_SPACING, pri.c_str(), true);

            u8g2_SetDrawColor(g_u8g2, 0);

        }

    }



    if (!g.filterText.empty() && !g_ime.composing()) {

        char fb[64];

        snprintf(fb, sizeof(fb), "筛选: %s", g.filterText.c_str());

        ui_draw_text(4, STATUS_Y - LINE_SPACING + 2, fb, true);

    }



    // Show active context/tag filter

    if (!g.filterContext.empty() || !g.filterTags.empty()) {

        char fb[96];

        std::string ftxt;

        if (!g.filterContext.empty()) ftxt += "@" + g.filterContext;

        for (auto &ft : g.filterTags) {

            if (!ftxt.empty()) ftxt += " ";

            ftxt += "#" + ft;

        }

        snprintf(fb, sizeof(fb), "过滤: %s", ftxt.c_str());

        ui_draw_text(4, STATUS_Y - LINE_SPACING + 2 - (g.filterText.empty() ? 0 : LINE_SPACING), fb, true);

    }



    // Status bar: left=help hint | selected task info
    char statusLine[128];
    statusLine[0] = '\0';
    if (g.sel >= 0 && g.sel < (int)g.filtered.size()) {
        auto &st = tasks[g.filtered[g.sel]];
        std::string parts;
        std::string sCtx = st["context"].asString();
        if (!sCtx.empty()) parts += "@" + sCtx + " ";
        auto &stt = st["tags"];
        if (stt.isArray()) {
            for (int j = 0; j < (int)stt.size(); j++) {
                std::string tn = stt[j].asString();
                if (!tn.empty()) parts += "#" + tn + " ";
            }
        }
        std::string sDue = st["due"].asString();
        if (!sDue.empty()) {
            size_t nd = 0;
            for (char c : sDue) if (c == '-') nd++;
            std::string dd = (nd >= 2 && sDue.length() >= 5) ? sDue.substr(sDue.length() - 5) : sDue;
            if (!dd.empty()) parts += dd + " ";
        }
        while (!parts.empty() && parts.back() == ' ') parts.pop_back();
        if (!parts.empty())
            snprintf(statusLine, sizeof(statusLine), "?:帮助|%s", parts.c_str());
    }
    if (hasMultiSel()) {
        // 多选时状态栏显示选中数量(替换任务详情信息)
        snprintf(statusLine, sizeof(statusLine), "已选%d项|?:帮助", (int)g.multiSel.size());
    }
    if (statusLine[0] == '\0')
        snprintf(statusLine, sizeof(statusLine), "?:帮助");
    char rbuf[24]; gtdStatusRight(rbuf, sizeof(rbuf));
    ui_draw_status(statusLine, rbuf);



    drawIMEStatus();

}



// ── Draw add-task input ──────────────────────────────────────────────────

static void drawAdd() {

    ui_clear();

    const char *addTitle = "添加新任务";

    if (g.mode == M_RENAME) addTitle = "重命名任务";

    else if (g.mode == M_ADD_PROJECT) addTitle = "新建项目";

    else if (g.mode == M_RENAME_PROJECT) addTitle = "重命名项目";

    else if (g.mode == M_ADD_CONTEXT) addTitle = "添加情境";

    else if (g.mode == M_ADD_TAG) addTitle = "添加标签";

    else if (g.mode == M_RENAME_CONTEXT) addTitle = "重命名情境";

    else if (g.mode == M_RENAME_TAG) addTitle = "重命名标签";

    ui_draw_text_centered(28, addTitle, false, true);

    u8g2_DrawHLine(g_u8g2, 0, 28 + g_font.descent() + 4, SCREEN_W);



    // Hint for context/tag modes

    if (g.mode == M_ADD_CONTEXT || g.mode == M_RENAME_CONTEXT)

        ui_draw_text_centered(28 + FONT_H + 4, "以@开头或直接输入");

    else if (g.mode == M_ADD_TAG || g.mode == M_RENAME_TAG)

        ui_draw_text_centered(28 + FONT_H + 4, "以#开头或直接输入");



    std::string display = g.editBuf.empty() ? " " : g.editBuf;

    int ty;

    if (g.mode == M_ADD_CONTEXT || g.mode == M_ADD_TAG || g.mode == M_RENAME_CONTEXT || g.mode == M_RENAME_TAG)

        ty = 28 + FONT_H * 2 + 8 + g_font.ascent();

    else

        ty = 28 + g_font.descent() + 12 + g_font.ascent();

    ui_draw_text(4, ty, display.c_str());

    // cursor

    int cx = g_font.textWidth(g.editBuf.substr(0, g.editCur).c_str());

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawBox(g_u8g2, 4 + cx, ty + 4, 8, 3);

    u8g2_SetDrawColor(g_u8g2, 1);



    // IME at bottom of screen, no status bar reservation

    if (g_ime.composing()) {

        std::string code = g_ime.displayCode();

        int pageSize = g_ime.pageSize();

        int curPage = g_ime.currentPage();

        int totalPages = g_ime.totalPages();

        if (totalPages < 1) totalPages = 1;

        char pageInfo[32];

        snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);



        int candBaseline = SCREEN_H - 9;

        int sepY = candBaseline - FONT_H - 4;

        int codeBaseline = sepY - 7;



        // Code line

        {

            int cw = g_font.textWidth(code.c_str()) + 8;

            u8g2_SetDrawColor(g_u8g2, 1);

            u8g2_DrawBox(g_u8g2, 4, codeBaseline - g_font.ascent(), cw, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 0);

            g_font.drawText(4, codeBaseline, code.c_str(), false);

            u8g2_SetDrawColor(g_u8g2, 1);

        }

        // Page info

        {

            int tw = g_font.textWidth(pageInfo);

            int pw = tw + 8;

            int px = SCREEN_W - pw - 4;

            u8g2_SetDrawColor(g_u8g2, 1);

            u8g2_DrawBox(g_u8g2, px, codeBaseline - g_font.ascent(), pw, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 0);

            g_font.drawText(px + 4, codeBaseline, pageInfo, false);

            u8g2_SetDrawColor(g_u8g2, 1);

        }

        // Separator

        u8g2_SetDrawColor(g_u8g2, 0);

        u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

        u8g2_SetDrawColor(g_u8g2, 1);

        // Candidates

        auto &cands = g_ime.candidates();

        std::string candLine;

        for (int i = 0; i < (int)cands.size(); i++) {

            char idx[16];

            snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);

            std::string part = std::string(" ") + idx + cands[i];

            int curW = g_font.textWidth(candLine.c_str());

            int partW = g_font.textWidth(part.c_str());

            if (curW + partW + 8 > SCREEN_W) break;

            candLine += part;

        }

        {

            int cw = g_font.textWidth(candLine.c_str()) + 8;

            u8g2_SetDrawColor(g_u8g2, 1);

            u8g2_DrawBox(g_u8g2, 4, candBaseline - g_font.ascent(), cw, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 0);

            g_font.drawText(4, candBaseline, candLine.c_str(), false);

            u8g2_SetDrawColor(g_u8g2, 1);

        }

    }

    u8g2_SetDrawColor(g_u8g2, 0);

    ui_commit();

}



static const char *HELP_LINES[] = {

    "── 列表视图 ──",

    "USER键  上选(双击切换状态)",

    "BOOT键  下选",

    "USER长按 切换标签",

    "",

    "↓     下移",

    "↑     上移",

    "j     上移任务",

    "k     下移任务",

    "h     提高层级",

    "l     降低层级",

    "z     折叠/展开",

    "Z     全部折叠/展开",

    "A     归档管理",

    "a     添加任务",

    "i     添加子任务",

    "r     重命名",

    "Enter 详情",

    "Space 切换状态",

    "Shift+↑↓ 多选",

    "d     删除(批量)",

    "Tab   切换标签",

    "/     筛选",

    "r     重命名项目",

    "n     新建项目",

    "",

    "── 任务详情 ──",

    "j/↓   下一字段",

    "k/↑   上一字段",

    "Enter 编辑字段",

    "Esc   返回",

    "优先级/状态/项目: 弹出选择",

    "截止日期: 日历选择",

    "Backspace 清除日期",

    "s     任务摘要",

    "",

    "── 日历对话框 ──",

    "←→    逐日选择",

    "↑↓    按周跳转",

    "h     上月",

    "l     下月",

    "Enter 确认",

    "Esc   取消",

    "",

    "── 添加/重命名 ──",

    "Ctrl+Space 切换输入法",

    "Enter 确认",

    "Esc   取消",

    "",

    "── 备注编辑 ──",

    "Enter 换行",

    "Tab   保存",

    "Esc   取消",

    "",

    "── 通用 ──",

    "?     显示帮助",

    "c     情境管理",

    "t     标签管理",

    "q/Esc 返回",

};

static const int HELP_LINE_COUNT = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);



static void drawSummary() {
    // Box 300x250 centered
    int boxX = (SCREEN_W - 300) / 2;
    int boxY = (SCREEN_H - 250) / 2;
    int boxW = 300, boxH = 250;
    u8g2_SetDrawColor(g_u8g2, 1);
    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);
    u8g2_SetDrawColor(g_u8g2, 0);
    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);
    ui_draw_text_centered(boxY + FONT_H, "任务摘要", false, true);

    auto &task = g.data["tasks"][g.detailTaskIdx];
    int textX = boxX + 8;
    int y = boxY + FONT_H + 12 + 15;
    int contentW = boxW - 16;

    // Context
    std::string ctx = task["context"].asString();
    char line[128];
    snprintf(line, sizeof(line), "情境: %s", ctx.empty() ? "(无)" : ("@" + ctx).c_str());
    ui_draw_text(textX, y, line, false);
    y += LINE_SPACING;

    // Tags
    auto &tt = task["tags"];
    std::string tagStr;
    if (tt.isArray() && tt.size() > 0) {
        for (int j = 0; j < (int)tt.size(); j++) {
            if (j > 0) tagStr += " ";
            tagStr += "#" + tt[j].asString();
        }
    } else {
        tagStr = "(无)";
    }
    snprintf(line, sizeof(line), "标签: %s", tagStr.c_str());
    ui_draw_text(textX, y, line, false);
    y += LINE_SPACING;

    // Progress
    int pct = (int)task["progress"].asNumber(0);
    snprintf(line, sizeof(line), "进度: %s", progressIconStr(pct));
    ui_draw_text(textX, y, line, false);
    y += LINE_SPACING + 4;

    // Separator
    u8g2_DrawHLine(g_u8g2, boxX + 4, y - 14, boxW - 8);
    y += LINE_SPACING + 4 - 12;

    // Notes - inline word-wrap
    std::string note = task["note"].asString();
    if (!note.empty()) {
        std::vector<std::string> noteLines;
        std::string curLine;
        for (size_t k = 0; k < note.size(); k++) {
            if (note[k] == '\n') {
                noteLines.push_back(curLine);
                curLine.clear();
            } else {
                curLine += note[k];
            }
        }
        if (!curLine.empty() || noteLines.empty()) noteLines.push_back(curLine);
        std::vector<std::string> wrapped;
        for (auto &nl : noteLines) {
            int pos = 0;
            int len = (int)nl.length();
            while (pos < len) {
                int end = pos;
                int lastBreak = -1;
                while (end < len) {
                    std::string sub = nl.substr(pos, end - pos + 1);
                    if (g_font.textWidth(sub.c_str()) > contentW) break;
                    if (nl[end] == ' ') lastBreak = end + 1;
                    end++;
                }
                if (end >= len) {
                    wrapped.push_back(nl.substr(pos));
                    break;
                }
                if (lastBreak > pos) {
                    wrapped.push_back(nl.substr(pos, lastBreak - pos));
                    pos = lastBreak;
                    while (pos < len && nl[pos] == ' ') pos++;
                } else if (end > pos) {
                    wrapped.push_back(nl.substr(pos, end - pos));
                    pos = end;
                } else {
                    // Single character wider than contentW, force include it
                    wrapped.push_back(nl.substr(pos, 1));
                    pos++;
                }
            }
        }
        int textAreaH = boxY + boxH - 16 - y;
        int maxVis = textAreaH / LINE_SPACING;
        if (maxVis < 1) maxVis = 1;
        if (g.summaryScroll > (int)wrapped.size() - maxVis)
            g.summaryScroll = (int)wrapped.size() - maxVis;
        if (g.summaryScroll < 0) g.summaryScroll = 0;
        for (int i = 0; i < maxVis && (g.summaryScroll + i) < (int)wrapped.size(); i++)
            g_font.drawText(textX, y + i * LINE_SPACING, wrapped[g.summaryScroll + i].c_str(), false);
    } else {
        ui_draw_text(textX, y, "(无备注)", false);
    }

    ui_draw_status("\xe2\x86\x91\xe2\x86\x93\xe6\xbb\x9a\xe5\x8a\xa8 Esc\xe8\xbf\x94\xe5\x9b\x9e", "");
    u8g2_SetDrawColor(g_u8g2, 0);
    ui_commit();
}static void drawHelp() {

    // Draw underlying screen first

    if (g.mode == M_HELP && g.helpPrevMode == M_BROWSE) drawList();

    else drawDetail();



    int boxW = 300;

    int boxH = 250;

    int boxX = (SCREEN_W - boxW) / 2;

    int boxY = (SCREEN_H - boxH) / 2;



    // Opaque white background

    u8g2_SetDrawColor(g_u8g2, 1);

    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);



    // Title

    int titleY = boxY + 8 + g_font.ascent();

    g_font.drawText(boxX + (boxW - g_font.textWidth("快捷键帮助")) / 2, titleY, "快捷键帮助", false);



    // Separator

    u8g2_DrawHLine(g_u8g2, boxX + 4, titleY + g_font.descent() + 4, boxW - 8);



    // Content area

    int contentY = titleY + g_font.descent() + 10;

    int contentMaxY = boxY + boxH - 8;

    int maxVis = (contentMaxY - contentY) / LINE_SPACING;

    if (maxVis < 1) maxVis = 1;



    int maxScroll = HELP_LINE_COUNT - maxVis;

    if (maxScroll < 0) maxScroll = 0;

    if (g.helpScroll > maxScroll) g.helpScroll = maxScroll;

    if (g.helpScroll < 0) g.helpScroll = 0;



    for (int i = 0; i < maxVis && (g.helpScroll + i) < HELP_LINE_COUNT; i++) {

        int ly = contentY + i * LINE_SPACING;

        const char *line = HELP_LINES[g.helpScroll + i];

        // Section headers start with ─ (0xE2 in UTF-8)

        bool isHeader = ((unsigned char)line[0] == 0xE2);

        ui_draw_text(boxX + 12, ly + g_font.ascent(), line, false, isHeader);

    }

}



struct DetailField {

    const char *label;  // display label

    const char *key;    // actual JSON key

    char type; // 's'=string, 'p'=priority toggle, 't'=status toggle, 'n'=number, 'j'=project selector

};



static const DetailField DETAIL_FIELDS[] = {

    {"标题",     "title",    's'},

    {"优先级",   "priority", 'p'},

    {"状态",     "status",   't'},

    {"项目",     "project",  'j'},

    {"截止日期", "due",      'd'},

    {"情境",     "context",  'c'},

    {"标签",     "tags",     'g'},

    {"进度",     "progress", 'n'},

    {"备注",     "note",     'm'},

};

static const int NUM_DETAIL_FIELDS = sizeof(DETAIL_FIELDS) / sizeof(DETAIL_FIELDS[0]);



static void openPicker(int fieldIdx) {

    auto &df = DETAIL_FIELDS[fieldIdx];

    auto &task = g.data["tasks"][g.detailTaskIdx];

    g.pickerOpts.clear();

    g.pickerField = fieldIdx;



    if (df.type == 'p') {

        for (int i = 0; i < 3; i++)

            g.pickerOpts.push_back({PRIORITY_LABELS[i], PRIORITY_DISPLAY[i]});

        std::string cur = task["priority"].asString();

        if (cur.empty()) cur = "B";

        g.pickerSel = 0;

        for (int i = 0; i < 3; i++) if (PRIORITY_LABELS[i] == cur) g.pickerSel = i;

    } else if (df.type == 't') {

        for (int i = 0; i < 4; i++)

            g.pickerOpts.push_back({STATUS_LABELS[i], STATUS_DISPLAY[i]});

        std::string cur = task["status"].asString("todo");

        g.pickerSel = 0;

        for (int i = 0; i < 4; i++) if (STATUS_LABELS[i] == cur) g.pickerSel = i;

    } else if (df.type == 'j') {

        buildProjectList();

        g.pickerOpts.push_back({"", "(无)"});

        for (auto &p : g.projectList)

            g.pickerOpts.push_back({p, p});

        std::string cur = task["project"].asString();

        g.pickerSel = 0;

        for (int i = 0; i < (int)g.pickerOpts.size(); i++)

            if (g.pickerOpts[i].value == cur) { g.pickerSel = i; break; }

    } else if (df.type == 'c') {

        buildContextList();

        g.pickerOpts.push_back({"", "(无)"});

        for (auto &c : g.contextList)

            g.pickerOpts.push_back({c, "@" + c});

        std::string cur = task["context"].asString();

        g.pickerSel = 0;

        for (int i = 0; i < (int)g.pickerOpts.size(); i++)

            if (g.pickerOpts[i].value == cur) { g.pickerSel = i; break; }

    } else if (df.type == 'g') {

        buildTagList();

        for (auto &t : g.tagList)

            g.pickerOpts.push_back({t, "#" + t});

        g.pickerSel = 0;

        g.pickerToggled.clear();

        auto &tt = task["tags"];

        if (tt.isArray()) {

            for (int i = 0; i < (int)g.pickerOpts.size(); i++) {

                for (int j = 0; j < (int)tt.size(); j++) {

                    if (g.pickerOpts[i].value == tt[j].asString()) {

                        g.pickerToggled.insert(i);

                        break;

                    }

                }

            }

        }

    } else if (df.type == 'n') {

        static const int PROG_VALS[] = {0, 12, 25, 37, 50, 62, 75, 87, 100};

        for (int i = 0; i < 9; i++) {

            char label[16];

            snprintf(label, sizeof(label), " %d%%", PROG_VALS[i]);

            g.pickerOpts.push_back({std::to_string(PROG_VALS[i]),
                                    std::string(progressIconStr(PROG_VALS[i])) + label});

        }

        int cur = task["progress"].asInt(0);

        g.pickerSel = 0;

        int best = 0, bestDist = 100000;

        for (int i = 0; i < 9; i++) {

            int d = PROG_VALS[i] - cur;

            if (d < 0) d = -d;

            if (d < bestDist) { bestDist = d; best = i; }

        }

        g.pickerSel = best;

    }



    g.mode = M_PICKER;

}



static void drawPicker() {

    // Draw detail underneath first

    drawDetail();



    int n = (int)g.pickerOpts.size();

    if (n == 0) return;



    int maxVis = 6;

    int boxW = 250;

    int boxH = maxVis * LINE_SPACING + 24 + 15;

    int boxX = (SCREEN_W - boxW) / 2;

    int boxY = (SCREEN_H - boxH) / 2;



    // Scroll to keep selection visible

    int scroll = 0;

    if (n > maxVis) {

        scroll = g.pickerSel - maxVis / 2;

        if (scroll < 0) scroll = 0;

        if (scroll + maxVis > n) scroll = n - maxVis;

    }



    // Opaque white background

    u8g2_SetDrawColor(g_u8g2, 1);

    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);

    // Black border frame

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);



    // Draw options

    int vis = n > maxVis ? maxVis : n;

    bool isTagPicker = (g.pickerField >= 0 && g.pickerField < NUM_DETAIL_FIELDS && DETAIL_FIELDS[g.pickerField].type == 'g');

    for (int i = 0; i < vis; i++) {

        int oi = scroll + i;

        int iy = boxY + 27 + i * LINE_SPACING;

        bool sel = (oi == g.pickerSel);

        bool toggled = isTagPicker && g.pickerToggled.count(oi);

        // For tag picker: show [✓] or [ ] prefix

        std::string display;

        if (isTagPicker) display = toggled ? "[✓]" : "[ ]";

        display += g.pickerOpts[oi].display;

        if (sel) {

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, boxX + 4, iy - g_font.ascent(), boxW - 8, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 1);

            g_font.drawText(boxX + 8, iy, display.c_str(), false);

            u8g2_SetDrawColor(g_u8g2, 0);

        } else {

            g_font.drawText(boxX + 8, iy, display.c_str(), false);

        }

    }



    // Scroll indicators

    if (n > maxVis) {

        if (scroll > 0) {

            char si[16]; snprintf(si, sizeof(si), "▲%d", scroll);

            g_font.drawText(boxX + boxW - g_font.textWidth(si) - 6, boxY + 27 + g_font.ascent(), si, false);

        }

        if (scroll + maxVis < n) {

            char si[16]; snprintf(si, sizeof(si), "▼%d", n - scroll - maxVis);

            g_font.drawText(boxX + boxW - g_font.textWidth(si) - 6, boxY + 27 + (vis - 1) * LINE_SPACING + g_font.ascent(), si, false);

        }

    }

}



static int daysInMonth(int year, int month) {

    static const int dim[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 30;

    int d = dim[month];

    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) d = 29;

    return d;

}



// day of week: 0=Mon .. 6=Sun

static int dayOfWeek(int year, int month, int day) {

    struct tm t = {};

    t.tm_year = year - 1900;

    t.tm_mon = month - 1;

    t.tm_mday = day;

    mktime(&t);

    int w = t.tm_wday;  // 0=Sun

    return w == 0 ? 6 : w - 1;

}



static void openCalendar() {

    auto &task = g.data["tasks"][g.detailTaskIdx];

    std::string due = task["due"].asString();

    time_t now_t; time(&now_t); struct tm *now_tm = localtime(&now_t);



    if (due.length() >= 10) {

        g.calYear = atoi(due.substr(0, 4).c_str());

        g.calMonth = atoi(due.substr(5, 2).c_str());

        g.calDay = atoi(due.substr(8, 2).c_str());

    } else {

        g.calYear = now_tm->tm_year + 1900;

        g.calMonth = now_tm->tm_mon + 1;

        g.calDay = now_tm->tm_mday;

    }

    g.calSelDay = g.calDay;

    // 防御 due 字段里非法月份/日期(如 13 月 32 日)导致的越界访问。
    if (g.calMonth < 1) g.calMonth = 1;
    if (g.calMonth > 12) g.calMonth = 12;
    if (g.calDay < 1) g.calDay = 1;
    int dim = daysInMonth(g.calYear, g.calMonth);
    if (g.calDay > dim) g.calDay = dim;
    g.calSelDay = g.calDay;

    g.mode = M_CALENDAR;

}



static void drawCalendar() {

    drawDetail();



    int boxW = 280;

    int boxH = 220;

    int boxX = (SCREEN_W - boxW) / 2;

    int boxY = (SCREEN_H - boxH) / 2;



    // Opaque white background

    u8g2_SetDrawColor(g_u8g2, 1);

    u8g2_DrawBox(g_u8g2, boxX, boxY, boxW, boxH);

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawFrame(g_u8g2, boxX, boxY, boxW, boxH);



    // Month/year header

    char hdr[32];

    snprintf(hdr, sizeof(hdr), "%d年%d月", g.calYear, g.calMonth);

    int hdrW = g_font.textWidth(hdr);

    g_font.drawText(boxX + (boxW - hdrW) / 2, boxY + 12 + g_font.ascent(), hdr, false);



    // Navigation hints: h=prev month, l=next month

    g_font.drawText(boxX + 8, boxY + 12 + g_font.ascent(), "h", false);

    g_font.drawText(boxX + boxW - 8 - g_font.textWidth("l"), boxY + 12 + g_font.ascent(), "l", false);



    // Weekday header

    const char *wdnames[] = {"一","二","三","四","五","六","日"};

    int colW = boxW / 7;

    int hdrY = boxY + 12 + FONT_H + 4;

    for (int i = 0; i < 7; i++) {

        int cx = boxX + i * colW + (colW - g_font.textWidth(wdnames[i])) / 2;

        g_font.drawText(cx, hdrY + g_font.ascent(), wdnames[i], false);

    }



    // Separator line

    u8g2_SetDrawColor(g_u8g2, 0);

    u8g2_DrawHLine(g_u8g2, boxX + 4, hdrY + FONT_H + 2, boxW - 8);



    // Day grid

    int firstDow = dayOfWeek(g.calYear, g.calMonth, 1);

    int dim = daysInMonth(g.calYear, g.calMonth);

    int gridY = hdrY + FONT_H + 21;



    time_t now_t; time(&now_t); struct tm *now_tm = localtime(&now_t);

    int todayDay = (now_tm->tm_year + 1900 == g.calYear && now_tm->tm_mon + 1 == g.calMonth)

                   ? now_tm->tm_mday : 0;



    for (int d = 1; d <= dim; d++) {

        int dow = (firstDow + d - 1) % 7;

        int row = (firstDow + d - 1) / 7;

        int cx = boxX + dow * colW;

        int cy = gridY + row * LINE_SPACING;



        bool sel = (d == g.calSelDay);

        bool today = (d == todayDay);



        char ds[16];

        snprintf(ds, sizeof(ds), "%d", d);

        int tw = g_font.textWidth(ds);

        int dx = cx + (colW - tw) / 2;



        if (sel) {

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, cx + 2, cy - g_font.ascent(), colW - 4, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 1);

            g_font.drawText(dx, cy, ds, true);

            u8g2_SetDrawColor(g_u8g2, 0);

        } else {

            if (today) {

                // Underline today

                g_font.drawText(dx, cy, ds, false);

                u8g2_DrawHLine(g_u8g2, dx, cy + 3, tw);

            } else {

                g_font.drawText(dx, cy, ds, false);

            }

        }

    }

}



static void rebuildNoteVrows() {

    if (g.noteVrowsDirty) {

        g.noteVrows = buildVrows(g.noteLines);

        g.noteVrowsDirty = false;

    }

}



static void openNoteEditor() {

    auto &task = g.data["tasks"][g.detailTaskIdx];

    std::string note = task["note"].asString();

    g.noteLines.clear();

    size_t pos = 0;

    while (pos < note.length()) {

        size_t nl = note.find('\n', pos);

        g.noteLines.push_back((nl == std::string::npos) ? note.substr(pos) : note.substr(pos, nl - pos));

        if (nl == std::string::npos) break;

        pos = nl + 1;

    }

    if (g.noteLines.empty()) g.noteLines.push_back("");

    g.noteRow = 0;

    g.noteCol = (int)g.noteLines[0].length();

    g.noteScroll = 0;

    g.noteVrowsDirty = true;

    g.imeActive = true;

    g_ime.setActive(true);

    g.mode = M_EDIT_NOTE;

}



static void drawNoteEditor() {

    ui_clear();

    ui_draw_text(4, g_font.ascent(), "备注编辑", false, true);

    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);



    rebuildNoteVrows();



    int contentY = FONT_H + 8 + LINE_SPACING;

    int maxY = g_ime.composing() ? (STATUS_Y - 2 * LINE_SPACING) : STATUS_Y;

    int vis = (maxY - contentY) / LINE_SPACING;

    if (vis < 1) vis = 1;



    // Find cursor vrow

    int cursorVrow = 0;

    for (int i = 0; i < (int)g.noteVrows.size(); i++) {

        if (g.noteVrows[i].lineIdx == g.noteRow &&

            g.noteCol >= g.noteVrows[i].start &&

            g.noteCol <= g.noteVrows[i].end) {

            cursorVrow = i;

            break;

        }

    }



    // Scroll

    if (cursorVrow < g.noteScroll) g.noteScroll = cursorVrow;

    if (cursorVrow >= g.noteScroll + vis) g.noteScroll = cursorVrow - vis + 1;



    // Draw vrows

    for (int i = 0; i < vis && (g.noteScroll + i) < (int)g.noteVrows.size(); i++) {

        auto &vr = g.noteVrows[g.noteScroll + i];

        int ly = contentY + i * LINE_SPACING;

        std::string text = g.noteLines[vr.lineIdx].substr(vr.start, vr.end - vr.start);

        ui_draw_text(4 + vr.indentCells * g_font.halfAdvance(), ly, text.c_str(), false);

    }



    // Draw cursor

    {

        auto &vr = g.noteVrows[cursorVrow];

        std::string before = g.noteLines[vr.lineIdx].substr(vr.start, g.noteCol - vr.start);

        int cx = 4 + vr.indentCells * g_font.halfAdvance() + g_font.textWidth(before.c_str());

        int cy = contentY + (cursorVrow - g.noteScroll) * LINE_SPACING;

        u8g2_SetDrawColor(g_u8g2, 0);

        u8g2_DrawBox(g_u8g2, cx, cy + 4, 8, 3);

        u8g2_SetDrawColor(g_u8g2, 1);

    }

    u8g2_SetDrawColor(g_u8g2, 0);



    // IME at bottom

    if (g_ime.composing()) {

        std::string code = g_ime.displayCode();

        int pageSize = g_ime.pageSize();

        int curPage = g_ime.currentPage();

        int totalPages = g_ime.totalPages();

        if (totalPages < 1) totalPages = 1;

        char pageInfo[32];

        snprintf(pageInfo, sizeof(pageInfo), "%d/%d", curPage, totalPages);



        int candBaseline = STATUS_Y - 9;

        int sepY = candBaseline - FONT_H - 4;

        int codeBaseline = sepY - 7;

        {

            int cw = g_font.textWidth(code.c_str()) + 8;

            u8g2_SetDrawColor(g_u8g2, 1);

            u8g2_DrawBox(g_u8g2, 4, codeBaseline - g_font.ascent(), cw, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 0);

            g_font.drawText(4, codeBaseline, code.c_str(), false);

            u8g2_SetDrawColor(g_u8g2, 1);

        }

        {

            int tw = g_font.textWidth(pageInfo);

            int pw = tw + 8;

            int px = SCREEN_W - pw - 4;

            u8g2_SetDrawColor(g_u8g2, 1);

            u8g2_DrawBox(g_u8g2, px, codeBaseline - g_font.ascent(), pw, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 0);

            g_font.drawText(px + 4, codeBaseline, pageInfo, false);

            u8g2_SetDrawColor(g_u8g2, 1);

        }

        u8g2_SetDrawColor(g_u8g2, 0);

        u8g2_DrawHLine(g_u8g2, 0, sepY, SCREEN_W);

        u8g2_SetDrawColor(g_u8g2, 1);

        auto &cands = g_ime.candidates();

        std::string candLine;

        for (int i = 0; i < (int)cands.size(); i++) {

            char idx[16];

            snprintf(idx, sizeof(idx), "%d.", (i % pageSize) + 1);

            std::string part = std::string(" ") + idx + cands[i];

            int curW = g_font.textWidth(candLine.c_str());

            int partW = g_font.textWidth(part.c_str());

            if (curW + partW + 8 > SCREEN_W) break;

            candLine += part;

        }

        {

            int cw = g_font.textWidth(candLine.c_str()) + 8;

            u8g2_SetDrawColor(g_u8g2, 1);

            u8g2_DrawBox(g_u8g2, 4, candBaseline - g_font.ascent(), cw, FONT_H);

            u8g2_SetDrawColor(g_u8g2, 0);

            g_font.drawText(4, candBaseline, candLine.c_str(), false);

            u8g2_SetDrawColor(g_u8g2, 1);

        }

    }

    u8g2_SetDrawColor(g_u8g2, 0);

    ui_draw_status("Enter换行 Ctrl+S保存 ESC取消", "");

    ui_commit();

}



static void drawDetail() {

    auto &tasks = g.data["tasks"];

    if (!tasks.isArray() || g.detailTaskIdx < 0 || g.detailTaskIdx >= (int)tasks.size()) return;

    auto &task = tasks[g.detailTaskIdx];



    ui_clear();

    ui_draw_text(4, g_font.ascent(), "任务详情", false, true);

    u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);



    int y = FONT_H + 8 + LINE_SPACING;

    int maxY = (g.mode == M_EDIT_FIELD && g_ime.composing()) ? (IME_CODE_Y - 4) : STATUS_Y;

    int vis = (maxY - y + LINE_SPACING - 1) / LINE_SPACING;

    if (vis < 1) vis = 1;



    int fScroll = (g.detailField / vis) * vis;

    for (int i = 0; i < vis && (fScroll + i) < NUM_DETAIL_FIELDS; i++) {

        int fi = fScroll + i;

        auto &df = DETAIL_FIELDS[fi];

        bool sel = (fi == g.detailField);

        bool editing = (g.mode == M_EDIT_FIELD && fi == g.detailField);



        std::string val;

        if (editing) {

            // Show live edit buffer

            val = g.editBuf;

            if (val.empty()) val = " ";

        } else {

            switch (df.type) {

            case 's': val = task[df.key].asString(); break;

            case 'j': {

                val = task["project"].asString();

                if (val.empty()) val = "(无)";

                break;

            }

            case 'p': {

                std::string pv = task["priority"].asString();

                if (pv.empty()) pv = "B";

                for (int k = 0; k < 3; k++) if (PRIORITY_LABELS[k] == pv) { val = PRIORITY_DISPLAY[k]; break; }

                if (val.empty()) val = pv;

                break;

            }

            case 't': {

                std::string sv = task["status"].asString("todo");

                for (int k = 0; k < 4; k++) if (STATUS_LABELS[k] == sv) { val = STATUS_DISPLAY[k]; break; }

                if (val.empty()) val = sv;

                break;

            }

            case 'n': {
                int pct = task["progress"].asInt(0);
                std::string icon = progressIconStr(pct);
                val = icon + " " + std::to_string(pct) + "%";
                break;
            }

            case 'd': {

                val = task["due"].asString();

                if (val.empty()) val = "(未设)";

                break;

            }

            case 'm': {

                val = task[df.key].asString();

                if (val.empty()) val = "(空)";

                else {

                    size_t nl = val.find('\n');

                    if (nl != std::string::npos) val = val.substr(0, nl) + "…";

                }

                break;

            }

            case 'c': {

                val = task["context"].asString();

                if (val.empty()) val = "(无)";

                else val = "@" + val;

                break;

            }

            case 'g': {

                auto &tt = task["tags"];

                if (tt.isArray() && tt.size() > 0) {

                    for (int j = 0; j < (int)tt.size(); j++) {

                        if (j > 0) val += " ";

                        val += "#" + tt[j].asString();

                    }

                } else val = "(无)";

                break;

            }

            }

        }

        if (val.empty()) val = "-";



        char buf[80];

        snprintf(buf, sizeof(buf), "%s: %s", df.label, val.c_str());

        ui_draw_text(8, y + i * LINE_SPACING, buf, sel);



        // Draw cursor when editing string/number/multiline fields

        if (editing && (df.type == 's' || df.type == 'm')) {

            std::string prefix = std::string(df.label) + ": ";

            int px = 8 + g_font.textWidth(prefix.c_str());

            int cx = px + g_font.textWidth(g.editBuf.substr(0, g.editCur).c_str());

            u8g2_SetDrawColor(g_u8g2, 0);

            u8g2_DrawBox(g_u8g2, cx, y + i * LINE_SPACING + 4, 8, 3);

            u8g2_SetDrawColor(g_u8g2, 1);

        }

    }



    // Show sub-task hierarchy below fields

    {

        std::string taskId = task["id"].asString();

        std::string parentId = task["parent"].asString();

        int ySub = y + vis * LINE_SPACING + 4;



        // Show parent task

        if (!parentId.empty()) {

            for (int k = 0; k < (int)tasks.size(); k++) {

                if (tasks[k]["id"].asString() == parentId) {

                    std::string pStatus = tasks[k]["status"].asString("todo");

                    char pbuf[80];

                    snprintf(pbuf, sizeof(pbuf), "↑ %s %s", statusIcon(pStatus), tasks[k]["title"].asString().c_str());

                    if (ySub + LINE_SPACING <= STATUS_Y)

                        ui_draw_text(8, ySub, pbuf, false);

                    ySub += LINE_SPACING;

                    break;

                }

            }

        }



        // Show child tasks

        int childCount = 0;

        for (int k = 0; k < (int)tasks.size(); k++) {

            if (tasks[k]["parent"].asString() == taskId) {

                childCount++;

                if (ySub + LINE_SPACING <= STATUS_Y) {

                    std::string cStatus = tasks[k]["status"].asString("todo");

                    char cbuf[80];

                    snprintf(cbuf, sizeof(cbuf), "├─ %s %s", statusIcon(cStatus), tasks[k]["title"].asString().c_str());

                    ui_draw_text(8, ySub, cbuf, false);

                }

                ySub += LINE_SPACING;

            }

        }

        // Fix last child connector

        if (childCount > 0) {

            // Redraw last child with └─

            int lastY = ySub - LINE_SPACING;

            if (lastY + LINE_SPACING <= STATUS_Y + LINE_SPACING) {

                // Find last child again

                int cc = 0;

                for (int k = 0; k < (int)tasks.size(); k++) {

                    if (tasks[k]["parent"].asString() == taskId) {

                        cc++;

                        if (cc == childCount) {

                            std::string cStatus = tasks[k]["status"].asString("todo");

                            char cbuf[80];

                            snprintf(cbuf, sizeof(cbuf), "└─ %s %s", statusIcon(cStatus), tasks[k]["title"].asString().c_str());

                            if (lastY + LINE_SPACING <= STATUS_Y)

                                ui_draw_text(8, lastY, cbuf, false);

                            break;

                        }

                    }

                }

            }

        }

    }

    {

        bool editingM = (g.mode == M_EDIT_FIELD && DETAIL_FIELDS[g.detailField].type == 'm');

        if (editingM)

            ui_draw_status("Enter换行 Tab保存 ↑↓选择 ESC返回", "");

        else

            ui_draw_status("Enter编辑 ↑↓选择 ESC返回 ?:帮助", "");

    }

    drawIMEStatus();

}



// ── Screen entry ─────────────────────────────────────────────────────────

void screen_gtd_init() {

    mkdir(DATA_DIR, 0755);

    g.mode = M_BROWSE;

    g.view = 0;

    g.sel = 0;

    g.scroll = 0;

    g.detailTaskIdx = -1;

    g.editBuf.clear();

    g.editCur = 0;

    g.imeActive = false;

    g.pendingParent.clear();

    g.filterText.clear();

    g.projectDrillIdx = -1;

    g.insertAfter = -1;

    g.confirmIdx = -1;

    g.confirmIdxs.clear();

    clearMultiSel();

    g_ime.setActive(false);

    loadData();

    autoArchive();

}



// ── Main handle ──────────────────────────────────────────────────────────

AppState screen_gtd_handle(int key, ScreenContext &ctx) {

    auto &tasks = g.data["tasks"];



    // ── M_ADD: adding new task ──────────────────────────────────────

    if (g.mode == M_ADD) {

        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (g_ime.handleKey(key, imeOut)) {

                if (!imeOut.empty()) {

                    g.editBuf.insert(g.editCur, imeOut);

                    g.editCur += (int)imeOut.length();

                }

                drawAdd();

                return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) {

            g.imeActive = !g.imeActive;

            g_ime.setActive(g.imeActive);

            drawAdd();

            return APP_GTD;

        }

        if (key == 0x1B) {

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

            g.insertAfter = -1;

            g.pendingParent.clear();

        } else if (key == 0x0A || key == 0x0D) {

            if (!g.editBuf.empty()) {

                JsonValue t;

                t.set("id", makeId());

                t.set("title", g.editBuf);

                t.set("priority", "B");

                t.set("status", "todo");

                t.set("due", "");

                t.set("progress", 0);

                t.set("note", "");

                t.set("context", "");

                t.set("tags", JsonValue::array());

                t.set("project", g.pendingProject.empty() ? std::string("") : g.pendingProject);

                t.set("parent", g.pendingParent);

                t.set("created", [](){

                    time_t n; time(&n); struct tm *tm = localtime(&n);

                    char b[16]; strftime(b, sizeof(b), "%Y-%m-%d", tm); return std::string(b);

                }());

                t.set("completed", "");

                if (g.insertAfter >= 0 && g.insertAfter < (int)tasks.size())

                    tasks.elements.insert(tasks.elements.begin() + g.insertAfter + 1, t);

                else

                    tasks.pushBack(t);

                saveData();

                rebuildFilter();

                g.pendingParent.clear();

                g.pendingProject.clear();

                g.insertAfter = -1;

            }

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            if (g.editCur > 0) {

                int prev = g.editCur - 1;

                while (prev > 0 && ((unsigned char)g.editBuf[prev] & 0xC0) == 0x80) prev--;

                g.editBuf.erase(prev, g.editCur - prev);

                g.editCur = prev;

            }

        } else if (key == KEY_LEFT) {

            if (g.editCur > 0) {

                g.editCur--;

                while (g.editCur > 0 && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur--;

            }

        } else if (key == KEY_RIGHT) {

            if (g.editCur < (int)g.editBuf.length()) {

                g.editCur++;

                while (g.editCur < (int)g.editBuf.length() && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur++;

            }

        } else if (key >= 0x20 && key <= 0x7E) {

            g.editBuf.insert(g.editCur, 1, (char)key);

            g.editCur++;

        }

        drawAdd();

        return APP_GTD;

    }



    // ── M_RENAME: quick rename task title ───────────────────────────────

    if (g.mode == M_RENAME) {

        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (g_ime.handleKey(key, imeOut)) {

                if (!imeOut.empty()) {

                    g.editBuf.insert(g.editCur, imeOut);

                    g.editCur += (int)imeOut.length();

                }

                drawAdd();

                return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) {

            g.imeActive = !g.imeActive;

            g_ime.setActive(g.imeActive);

            drawAdd();

            return APP_GTD;

        }

        if (key == 0x1B) {

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x0A || key == 0x0D) {

            if (!g.editBuf.empty() && g.detailTaskIdx >= 0 && g.detailTaskIdx < (int)tasks.size()) {

                tasks[g.detailTaskIdx].set("title", g.editBuf);

                saveData();

                rebuildFilter();

            }

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            if (g.editCur > 0) {

                int prev = g.editCur - 1;

                while (prev > 0 && ((unsigned char)g.editBuf[prev] & 0xC0) == 0x80) prev--;

                g.editBuf.erase(prev, g.editCur - prev);

                g.editCur = prev;

            }

        } else if (key == KEY_LEFT) {

            if (g.editCur > 0) {

                g.editCur--;

                while (g.editCur > 0 && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur--;

            }

        } else if (key == KEY_RIGHT) {

            if (g.editCur < (int)g.editBuf.length()) {

                g.editCur++;

                while (g.editCur < (int)g.editBuf.length() && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur++;

            }

        } else if (key >= 0x20 && key <= 0x7E) {

            g.editBuf.insert(g.editCur, 1, (char)key);

            g.editCur++;

        }

        drawAdd();

        return APP_GTD;

    }



    // ── M_CONFIRM: confirmation dialog ────────────────────────────────

    if (g.mode == M_CONFIRM) {

        ui_clear();

        ui_draw_text_centered(SCREEN_H / 2, g.confirmMsg.c_str(), false, true);

        ui_draw_status("Enter确认 ESC取消", "");

        ui_commit();

        if (key == 0x0A || key == 0x0D) {

            if (!g.confirmIdxs.empty()) {

                // 批量删除: 升序后从高下标往低擦除,避免下标漂移
                std::sort(g.confirmIdxs.begin(), g.confirmIdxs.end());
                for (int n = (int)g.confirmIdxs.size() - 1; n >= 0; n--) {
                    int idx = g.confirmIdxs[n];
                    if (idx >= 0 && idx < (int)tasks.size())
                        tasks.elements.erase(tasks.elements.begin() + idx);
                }
                saveData();
                rebuildFilter();
                clearMultiSel();
                if (g.sel >= (int)g.filtered.size()) g.sel = (int)g.filtered.size() - 1;
                if (g.sel < 0) g.sel = 0;
                ctx.statusMessage = "已删除";
                g.confirmIdxs.clear();

            } else if (g.confirmIdx >= 0 && g.confirmIdx < (int)tasks.size()) {

                tasks.elements.erase(tasks.elements.begin() + g.confirmIdx);

                saveData();

                rebuildFilter();

                ctx.statusMessage = "已删除";

            }

            g.confirmIdx = -1;

            g.mode = M_BROWSE;

        } else if (key == 0x1B) {

            // 取消: 清确认列表但保留多选, 允许反悔后继续操作
            g.confirmIdxs.clear();

            g.confirmIdx = -1;

            g.mode = M_BROWSE;

        }

        return APP_GTD;

    }



    // ── M_ADD_PROJECT / M_RENAME_PROJECT ─────────────────────────────

    if (g.mode == M_ADD_PROJECT || g.mode == M_RENAME_PROJECT || g.mode == M_RENAME_CONTEXT || g.mode == M_RENAME_TAG) {

        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (g_ime.handleKey(key, imeOut)) {

                if (!imeOut.empty()) { g.editBuf.insert(g.editCur, imeOut); g.editCur += (int)imeOut.length(); }

                drawAdd(); return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) { g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive); drawAdd(); return APP_GTD; }

        if (key == 0x1B) {

            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x0A || key == 0x0D) {

            if (!g.editBuf.empty()) {

                auto &projs = g.data["projects"];

                if (g.mode == M_ADD_PROJECT) {

                    // Add new project name to stored list

                    projs.pushBack(g.editBuf);

                    saveData();

                    buildProjectList();

                } else if (g.mode == M_RENAME_PROJECT) {

                    // Rename: update all tasks + stored projects list

                    for (int i = 0; i < (int)tasks.size(); i++) {

                        if (tasks[i]["project"].asString() == g.renameTargetProject)

                            tasks[i].set("project", g.editBuf);

                    }

                    for (int i = 0; i < (int)projs.size(); i++) {

                        if (projs[i].asString() == g.renameTargetProject) {

                            projs.elements[i] = g.editBuf;

                        }

                    }

                    saveData();

                    buildProjectList();

                } else if (g.mode == M_RENAME_CONTEXT) {

                    // Rename context: update all tasks + stored list

                    auto &ctxArr = g.data["contexts"];

                    for (int i = 0; i < (int)tasks.size(); i++) {

                        if (tasks[i]["context"].asString() == g.renameTargetContext)

                            tasks[i].set("context", g.editBuf);

                    }

                    for (int i = 0; i < (int)ctxArr.size(); i++) {

                        if (ctxArr[i].asString() == g.renameTargetContext)

                            ctxArr.elements[i] = g.editBuf;

                    }

                    if (g.filterContext == g.renameTargetContext)

                        g.filterContext = g.editBuf;

                    saveData();

                    buildContextList();

                } else if (g.mode == M_RENAME_TAG) {

                    // Rename tag: update all tasks + stored list

                    auto &tagArr = g.data["tags"];

                    for (int i = 0; i < (int)tasks.size(); i++) {

                        auto &tt = tasks[i]["tags"];

                        if (tt.isArray()) {

                            for (int j = 0; j < (int)tt.size(); j++) {

                                if (tt[j].asString() == g.renameTargetTag)

                                    tt.elements[j] = g.editBuf;

                            }

                        }

                    }

                    for (int i = 0; i < (int)tagArr.size(); i++) {

                        if (tagArr[i].asString() == g.renameTargetTag)

                            tagArr.elements[i] = g.editBuf;

                    }

                    for (int i = 0; i < (int)g.filterTags.size(); i++) {

                        if (g.filterTags[i] == g.renameTargetTag)

                            g.filterTags[i] = g.editBuf;

                    }

                    saveData();

                    buildTagList();

                }

            }

            g.mode = M_BROWSE; g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            if (g.editCur > 0) { int prev = g.editCur - 1; while (prev > 0 && ((unsigned char)g.editBuf[prev] & 0xC0) == 0x80) prev--; g.editBuf.erase(prev, g.editCur - prev); g.editCur = prev; }

        } else if (key >= 0x20 && key <= 0x7E) { g.editBuf.insert(g.editCur, 1, (char)key); g.editCur++; }

        drawAdd();

        return APP_GTD;

    }



    // ── M_DETAIL: task detail view ───────────────────────────────────

    // ── M_EDIT_FIELD: editing a field inside detail ──────────────────

    // ── M_PICKER: popup selector for p/t/j fields ───────────────────

    // ── M_CALENDAR: date picker for due field ────────────────────────

    if (g.mode == M_DETAIL || g.mode == M_EDIT_FIELD || g.mode == M_PICKER || g.mode == M_CALENDAR) {

        if (g.mode == M_CALENDAR) {

            if (key == 0x1B) {

                g.mode = M_DETAIL;

            } else if (key == KEY_LEFT) {

                // Previous day

                if (g.calSelDay > 1) g.calSelDay--;

            } else if (key == KEY_RIGHT) {

                // Next day

                int dim = daysInMonth(g.calYear, g.calMonth);

                if (g.calSelDay < dim) g.calSelDay++;

            } else if (key == KEY_UP || key == 'k') {

                g.calSelDay -= 7;

                if (g.calSelDay < 1) g.calSelDay = 1;

            } else if (key == KEY_DOWN || key == 'j') {

                g.calSelDay += 7;

                int dim = daysInMonth(g.calYear, g.calMonth);

                if (g.calSelDay > dim) g.calSelDay = dim;

            } else if (key == 'h' || key == 'H') {

                // Previous month

                g.calMonth--;

                if (g.calMonth < 1) { g.calMonth = 12; g.calYear--; }

                int dim = daysInMonth(g.calYear, g.calMonth);

                if (g.calSelDay > dim) g.calSelDay = dim;

            } else if (key == 'l' || key == 'L') {

                // Next month

                g.calMonth++;

                if (g.calMonth > 12) { g.calMonth = 1; g.calYear++; }

                int dim = daysInMonth(g.calYear, g.calMonth);

                if (g.calSelDay > dim) g.calSelDay = dim;

            } else if (key == 0x0A || key == 0x0D) {

                // Confirm date

                auto &task = tasks[g.detailTaskIdx];

                char ds[16];

                snprintf(ds, sizeof(ds), "%04d-%02d-%02d", g.calYear, g.calMonth, g.calSelDay);

                task.set("due", ds);

                saveData();

                rebuildFilter();

                g.mode = M_DETAIL;

            } else if (key == 0x7F || key == 0x08) {

                // Clear date

                auto &task = tasks[g.detailTaskIdx];

                task.set("due", "");

                saveData();

                rebuildFilter();

                g.mode = M_DETAIL;

            }

            drawCalendar();

            ui_commit();

            return APP_GTD;

        }



        if (g.mode == M_PICKER) {

            auto &df = DETAIL_FIELDS[g.pickerField];

            if (key == 0x1B) {

                g.mode = M_DETAIL;

            } else if (key == KEY_UP || key == 'k') {

                if (g.pickerSel > 0) g.pickerSel--;

            } else if (key == KEY_DOWN || key == 'j') {

                if (g.pickerSel < (int)g.pickerOpts.size() - 1) g.pickerSel++;

            } else if (key == ' ' && df.type == 'g') {

                // Toggle multi-select for tags

                if (g.pickerToggled.count(g.pickerSel))

                    g.pickerToggled.erase(g.pickerSel);

                else

                    g.pickerToggled.insert(g.pickerSel);

            } else if (key == 0x0A || key == 0x0D) {

                // Apply selection

                auto &task = tasks[g.detailTaskIdx];

                std::string val = g.pickerOpts[g.pickerSel].value;

                if (df.type == 'p') task.set("priority", val);

                else if (df.type == 't') task.set("status", val);

                else if (df.type == 'j') task.set("project", val);

                else if (df.type == 'c') task.set("context", val);

                else if (df.type == 'n') task.set("progress", std::stoi(val));

                else if (df.type == 'g') {

                    JsonValue tags(JsonValue::array());

                    for (int i = 0; i < (int)g.pickerOpts.size(); i++) {

                        if (g.pickerToggled.count(i))

                            tags.pushBack(g.pickerOpts[i].value);

                    }

                    task.set("tags", tags);

                }

                saveData();

                if (g.view == V_PROJECT) buildProjectList();

                rebuildFilter();

                g.mode = M_DETAIL;

            }

            drawPicker();

            ui_commit();

            return APP_GTD;

        }



        if (g.mode == M_EDIT_FIELD) {

            auto &task = tasks[g.detailTaskIdx];

            auto &df = DETAIL_FIELDS[g.detailField];



            if (g.imeActive && key != 0) {

                std::string imeOut;

                if (g_ime.handleKey(key, imeOut)) {

                    if (!imeOut.empty()) {

                        g.editBuf.insert(g.editCur, imeOut);

                        g.editCur += (int)imeOut.length();

                    }

                    drawDetail();

                    ui_commit();

                    return APP_GTD;

                }

            }

            if (key == KEY_IME_TOGGLE) {

                g.imeActive = !g.imeActive;

                g_ime.setActive(g.imeActive);

                drawDetail();

                ui_commit();

                return APP_GTD;

            }



            if (key == 0x1B) {

                g.mode = M_DETAIL;

                g.imeActive = false; g_ime.setActive(false);

            } else if ((key == 0x0A || key == 0x0D) && df.type == 'm') {

                // Multi-line: Enter inserts newline

                g.editBuf.insert(g.editCur, 1, '\n');

                g.editCur++;

            } else if (key == '\t' && df.type == 'm') {

                // Tab commits multi-line note

                task.set(df.key, g.editBuf);

                saveData();

                rebuildFilter();

                g.mode = M_DETAIL;

                g.imeActive = false; g_ime.setActive(false);

            } else if ((key == 0x0A || key == 0x0D) && df.type != 'm') {

                // Commit field value

                if (df.type == 's')

                    task.set(df.key, g.editBuf);

                saveData();

                if (g.view == V_PROJECT) buildProjectList();

                rebuildFilter();

                g.mode = M_DETAIL;

                g.imeActive = false; g_ime.setActive(false);

            } else if (key == 0x7F || key == 0x08) {

                if (g.editCur > 0) {

                    int prev = g.editCur - 1;

                    while (prev > 0 && ((unsigned char)g.editBuf[prev] & 0xC0) == 0x80) prev--;

                    g.editBuf.erase(prev, g.editCur - prev);

                    g.editCur = prev;

                }

            } else if (key == KEY_LEFT) {

                if (g.editCur > 0) { g.editCur--;

                    while (g.editCur > 0 && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur--; }

            } else if (key == KEY_RIGHT) {

                if (g.editCur < (int)g.editBuf.length()) { g.editCur++;

                    while (g.editCur < (int)g.editBuf.length() && ((unsigned char)g.editBuf[g.editCur] & 0xC0) == 0x80) g.editCur++; }

            } else if (key >= 0x20 && key <= 0x7E) {

                g.editBuf.insert(g.editCur, 1, (char)key);

                g.editCur++;

            }

            drawDetail();

            ui_commit();

            return APP_GTD;

        }



        // M_DETAIL navigation

        if (key == 0x1B || key == 'q' || key == 'Q') {

            g.mode = M_BROWSE;

        } else if (key == KEY_UP || key == 'k') {

            if (g.detailField > 0) g.detailField--;

        } else if (key == KEY_DOWN || key == 'j') {

            if (g.detailField < NUM_DETAIL_FIELDS - 1) g.detailField++;

        } else if (key == 0x0A || key == 0x0D) {

            auto &df = DETAIL_FIELDS[g.detailField];

            auto &task = tasks[g.detailTaskIdx];

            // Open picker for selection types, calendar for date, text editor for string/number

            switch (df.type) {

            case 'p': case 't': case 'j': case 'c': case 'g': case 'n':

                openPicker(g.detailField);

                break;

            case 'd':

                openCalendar();

                break;

            case 's':

                g.editBuf = task[df.key].asString();

                g.editCur = (int)g.editBuf.length();

                g.imeActive = true;

                g_ime.setActive(true);

                g.mode = M_EDIT_FIELD;

                break;

            case 'm':

                openNoteEditor();

                break;

            }

        } else if (key == 's' || key == 'S') {

            g.summaryScroll = 0;

            g.summaryPrevMode = M_DETAIL;

            g.mode = M_SUMMARY;

            drawSummary();

            return APP_GTD;

        } else if (key == '?') {

            g.helpScroll = 0;

            g.helpPrevMode = M_DETAIL;

            g.mode = M_HELP;

            drawHelp();

            ui_commit();

            return APP_GTD;

        }



        drawDetail();

        ui_commit();

        return APP_GTD;

    }



    // ── M_CONTEXT_MGR / M_TAG_MGR / M_ADD_CONTEXT / M_ADD_TAG ──────────

    if (g.mode == M_CONTEXT_MGR || g.mode == M_TAG_MGR) {

        bool isCtx = (g.mode == M_CONTEXT_MGR);

        auto &list = isCtx ? g.contextList : g.tagList;

        int &sel = isCtx ? g.ctxMgrSel : g.tagMgrSel;

        const char *title = isCtx ? "情境管理" : "标签管理";

        char prefix = isCtx ? '@' : '#';



        if (key == 0x1B || key == 'q' || key == 'Q') {

            g.mode = M_BROWSE;

        } else if (key == KEY_UP || key == 'k') {

            if (sel > 0) sel--;

        } else if (key == KEY_DOWN || key == 'j') {

            if (sel < (int)list.size() - 1) sel++;

        } else if (key == 'a' || key == 'A') {

            g.mode = isCtx ? M_ADD_CONTEXT : M_ADD_TAG;

            g.editBuf.clear(); g.editCur = 0;

            g.imeActive = true; g_ime.setActive(true);

        } else if ((key == 'd' || key == 'D') && sel < (int)list.size()) {

            std::string name = list[sel];

            // Remove from stored list

            auto &arr = isCtx ? g.data["contexts"] : g.data["tags"];

            for (int i = (int)arr.size() - 1; i >= 0; i--)

                if (arr[i].asString() == name) arr.elements.erase(arr.elements.begin() + i);

            // Clear from tasks

            auto &tasks = g.data["tasks"];

            if (isCtx) {

                for (int i = 0; i < (int)tasks.size(); i++)

                    if (tasks[i]["context"].asString() == name) tasks[i].set("context", "");

                if (g.filterContext == name) g.filterContext.clear();

            } else {

                for (int i = 0; i < (int)tasks.size(); i++) {

                    auto &tt = tasks[i]["tags"];

                    if (tt.isArray()) {

                        for (int j = (int)tt.size() - 1; j >= 0; j--)

                            if (tt[j].asString() == name) tt.elements.erase(tt.elements.begin() + j);

                    }

                }

                for (int i = (int)g.filterTags.size() - 1; i >= 0; i--)

                    if (g.filterTags[i] == name) g.filterTags.erase(g.filterTags.begin() + i);

            }

            saveData();

            if (isCtx) buildContextList(); else buildTagList();

            if (sel >= (int)list.size()) sel = (int)list.size() - 1;

            if (sel < 0) sel = 0;

            rebuildFilter();

        } else if (key == 0x0A || key == 0x0D) {

            if (sel < (int)list.size()) {

                if (isCtx) {

                    g.filterContext = (g.filterContext == list[sel]) ? "" : list[sel];

                } else {

                    std::string name = list[sel];

                    bool found = false;

                    for (int i = 0; i < (int)g.filterTags.size(); i++) {

                        if (g.filterTags[i] == name) { g.filterTags.erase(g.filterTags.begin() + i); found = true; break; }

                    }

                    if (!found) g.filterTags.push_back(name);

                }

                rebuildFilter();

            }

        } else if (key == ' ' && !isCtx && sel < (int)list.size()) {

            // Tag multi-select toggle

            std::string name = list[sel];

            bool found = false;

            for (int i = 0; i < (int)g.filterTags.size(); i++) {

                if (g.filterTags[i] == name) { g.filterTags.erase(g.filterTags.begin() + i); found = true; break; }

            }

            if (!found) g.filterTags.push_back(name);

            rebuildFilter();

        } else if ((key == 'r' || key == 'R') && sel < (int)list.size()) {

            g.mode = isCtx ? M_RENAME_CONTEXT : M_RENAME_TAG;

            g.editBuf = list[sel];

            g.editCur = (int)g.editBuf.length();

            g.imeActive = true; g_ime.setActive(true);

            if (isCtx) g.renameTargetContext = list[sel];

            else g.renameTargetTag = list[sel];

        }



        // Draw

        ui_clear(); int y = FONT_H + 6 + LINE_SPACING;

        ui_draw_text(4, FONT_H, title, false, true);

        u8g2_DrawHLine(g_u8g2, 0, FONT_H + 4, SCREEN_W);

        int maxY = STATUS_Y;

        int vis = (maxY - y + LINE_SPACING - 1) / LINE_SPACING;

        if (vis < 1) vis = 1;

        if (sel < g.scroll) g.scroll = sel;

        if (sel >= g.scroll + vis) g.scroll = sel - vis + 1;



        for (int i = 0; i < vis && (g.scroll + i) < (int)list.size(); i++) {

            int idx = g.scroll + i;

            bool s = (idx == sel);

            char buf[64];

            snprintf(buf, sizeof(buf), "%c%s", prefix, list[idx].c_str());

            ui_draw_text(8, y + i * LINE_SPACING, buf, s);

            // Show filter indicator

            if (isCtx && g.filterContext == list[idx]) {

                g_font.drawText(SCREEN_W - g_font.textWidth("●") - 4, y + i * LINE_SPACING, "●", false);

            }

            if (!isCtx) {

                bool active = false;

                for (auto &ft : g.filterTags) if (ft == list[idx]) { active = true; break; }

                if (active) g_font.drawText(SCREEN_W - g_font.textWidth("●") - 4, y + i * LINE_SPACING, "●", false);

            }

        }

        if (list.empty()) ui_draw_text(8, y, "暂无 — 按a添加");



        // Show active filter

        char sl[96];

        if (isCtx)

            snprintf(sl, sizeof(sl), "a:添加 d:删除 r:重命名 Enter:筛选 Esc:返回 %d项", (int)list.size());

        else

            snprintf(sl, sizeof(sl), "a:添加 d:删除 r:重命名 Enter/Space:筛选 Esc:返回 %d项", (int)list.size());

        char rbuf[24]; gtdStatusRight(rbuf, sizeof(rbuf));
        ui_draw_status(sl, rbuf);

        drawIMEStatus(); ui_commit();

        return APP_GTD;

    }



    // M_ADD_CONTEXT / M_ADD_TAG

    if (g.mode == M_ADD_CONTEXT || g.mode == M_ADD_TAG) {

        bool isCtx = (g.mode == M_ADD_CONTEXT);

        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (g_ime.handleKey(key, imeOut)) {

                if (!imeOut.empty()) { g.editBuf.insert(g.editCur, imeOut); g.editCur += (int)imeOut.length(); }

                drawAdd(); return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) { g.imeActive = !g.imeActive; g_ime.setActive(g.imeActive); drawAdd(); return APP_GTD; }



        if (key == 0x1B) {

            g.mode = isCtx ? M_CONTEXT_MGR : M_TAG_MGR;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x0A || key == 0x0D) {

            if (!g.editBuf.empty()) {

                std::string name = g.editBuf;

                if (!name.empty() && (name[0] == '@' || name[0] == '#')) name = name.substr(1);

                if (!name.empty()) {

                    auto &arr = isCtx ? g.data["contexts"] : g.data["tags"];

                    arr.pushBack(name);

                    saveData();

                    if (isCtx) buildContextList(); else buildTagList();

                }

            }

            g.mode = isCtx ? M_CONTEXT_MGR : M_TAG_MGR;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            if (g.editCur > 0) { int prev = g.editCur - 1; while (prev > 0 && ((unsigned char)g.editBuf[prev] & 0xC0) == 0x80) prev--; g.editBuf.erase(prev, g.editCur - prev); g.editCur = prev; }

        } else if (key >= 0x20 && key <= 0x7E) { g.editBuf.insert(g.editCur, 1, (char)key); g.editCur++; }

        drawAdd();

        return APP_GTD;

    }



    // ── M_SUMMARY: task summary dialog ────────────────────────────

    if (g.mode == M_SUMMARY) {

        if (key == 0x1B || key == 'q' || key == 'Q') {

            g.mode = g.summaryPrevMode;

        } else if (key == KEY_UP || key == 'k') {

            if (g.summaryScroll > 0) g.summaryScroll--;

            if (g.summaryPrevMode == M_BROWSE) { drawList(); drawSummary(); }
            else drawSummary();

            return APP_GTD;

        } else if (key == KEY_DOWN || key == 'j') {

            g.summaryScroll++;

            if (g.summaryPrevMode == M_BROWSE) { drawList(); drawSummary(); }
            else drawSummary();

            return APP_GTD;

        }

        // Re-draw underlying screen, then overlay summary

        if (g.summaryPrevMode == M_BROWSE) drawList();
        else drawDetail();

        drawSummary();

        return APP_GTD;

    }



    // ── M_ARCHIVE: archive manager ────────────────────────────────────

    if (g.mode == M_ARCHIVE) {

        if (key == 0x1B || key == 'q' || key == 'Q') {
            if (g.archiveBrowsing) {
                g.archiveBrowsing = false;
            } else {
                g.mode = M_BROWSE;
            }
        } else if (key == KEY_UP) {
            if (g.archiveBrowsing) {
                if (g.archiveViewSel > 0) g.archiveViewSel--;
            } else {
                if (g.archiveSel > 0) g.archiveSel--;
            }
        } else if (key == KEY_DOWN) {
            if (g.archiveBrowsing) {
                if (g.archiveViewSel < (int)g.archiveTasks.size() - 1) g.archiveViewSel++;
            } else {
                if (g.archiveSel < (int)g.archiveMonths.size() - 1) g.archiveSel++;
            }
        } else if ((key == 'd' || key == 'D') && !g.archiveBrowsing) {
            if (g.archiveSel >= 0 && g.archiveSel < (int)g.archiveMonths.size()) {
                std::string path = std::string(ARCHIVE_DIR) + "/" + g.archiveMonths[g.archiveSel] + ".json";
                remove(path.c_str());
                g.archiveMonths.erase(g.archiveMonths.begin() + g.archiveSel);
                g.archiveCounts.erase(g.archiveCounts.begin() + g.archiveSel);
                if (g.archiveSel >= (int)g.archiveMonths.size()) g.archiveSel = (int)g.archiveMonths.size() - 1;
                if (g.archiveSel < 0) g.archiveSel = 0;
            }
        } else if ((key == 0x0A || key == 0x0D) && !g.archiveBrowsing) {
            if (g.archiveSel >= 0 && g.archiveSel < (int)g.archiveMonths.size()) {
                g.archiveViewMonth = g.archiveMonths[g.archiveSel];
                loadArchiveMonthTasks(g.archiveViewMonth);
                g.archiveBrowsing = true;
                g.archiveViewSel = 0;
                g.archiveViewScroll = 0;
            }
        } else if (key == '?' || key == 'h') {
            g.helpScroll = 0;
            g.helpPrevMode = M_ARCHIVE;
            g.mode = M_HELP;
            drawHelp();
            ui_commit();
            return APP_GTD;
        }

        if (g.mode != M_HELP) drawArchiveMgr();
        return APP_GTD;
    }



    // ── M_HELP: shortcut help dialog ─────────────────────────────────

    if (g.mode == M_HELP) {

        if (key == 0x1B || key == 'q' || key == 'Q' || key == 0x0A || key == 0x0D) {

            g.mode = g.helpPrevMode;

        } else if (key == KEY_UP || key == 'k') {

            if (g.helpScroll > 0) g.helpScroll--;

        } else if (key == KEY_DOWN || key == 'j') {

            g.helpScroll++;

        } else if (key == KEY_LEFT) {

            g.helpScroll -= 5;

            if (g.helpScroll < 0) g.helpScroll = 0;

        } else if (key == KEY_RIGHT) {

            g.helpScroll += 5;

        }

        drawHelp();

        ui_commit();

        return APP_GTD;

    }



    // ── M_EDIT_NOTE: multi-line note editor ──────────────────────────

    if (g.mode == M_EDIT_NOTE) {

        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (g_ime.handleKey(key, imeOut)) {

                if (!imeOut.empty()) {

                    g.noteLines[g.noteRow].insert(g.noteCol, imeOut);

                    g.noteCol += (int)imeOut.length();

                    g.noteVrowsDirty = true;

                }

                drawNoteEditor();

                return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) {

            g.imeActive = !g.imeActive;

            g_ime.setActive(g.imeActive);

            drawNoteEditor();

            return APP_GTD;

        }



        if (key == 0x1B) {

            // Cancel

            g.mode = M_DETAIL;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x13 || key == KEY_CTRL_ENTER) {

            // Ctrl+S or Ctrl+Enter — save

            std::string text;

            for (size_t i = 0; i < g.noteLines.size(); i++) {

                if (i > 0) text += '\n';

                text += g.noteLines[i];

            }

            auto &task = tasks[g.detailTaskIdx];

            task.set("note", text);

            saveData();

            g.mode = M_DETAIL;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x0A || key == 0x0D) {

            // Enter — insert newline

            std::string rest = g.noteLines[g.noteRow].substr(g.noteCol);

            g.noteLines[g.noteRow].erase(g.noteCol);

            g.noteLines.insert(g.noteLines.begin() + g.noteRow + 1, rest);

            g.noteRow++;

            g.noteCol = 0;

            g.noteVrowsDirty = true;

        } else if (key == 0x7F || key == 0x08) {

            // Backspace

            if (g.noteCol > 0) {

                int prev = g.noteCol - 1;

                while (prev > 0 && ((unsigned char)g.noteLines[g.noteRow][prev] & 0xC0) == 0x80) prev--;

                g.noteLines[g.noteRow].erase(prev, g.noteCol - prev);

                g.noteCol = prev;

                g.noteVrowsDirty = true;

            } else if (g.noteRow > 0) {

                // Join with previous line

                g.noteCol = (int)g.noteLines[g.noteRow - 1].length();

                g.noteLines[g.noteRow - 1] += g.noteLines[g.noteRow];

                g.noteLines.erase(g.noteLines.begin() + g.noteRow);

                g.noteRow--;

                g.noteVrowsDirty = true;

            }

        } else if (key == KEY_UP || key == 'k') {

            if (g.noteRow > 0) {

                g.noteRow--;

                if (g.noteCol > (int)g.noteLines[g.noteRow].length())

                    g.noteCol = (int)g.noteLines[g.noteRow].length();

            }

        } else if (key == KEY_DOWN || key == 'j') {

            if (g.noteRow < (int)g.noteLines.size() - 1) {

                g.noteRow++;

                if (g.noteCol > (int)g.noteLines[g.noteRow].length())

                    g.noteCol = (int)g.noteLines[g.noteRow].length();

            }

        } else if (key == KEY_LEFT) {

            if (g.noteCol > 0) {

                g.noteCol--;

                while (g.noteCol > 0 && ((unsigned char)g.noteLines[g.noteRow][g.noteCol] & 0xC0) == 0x80) g.noteCol--;

            } else if (g.noteRow > 0) {

                g.noteRow--;

                g.noteCol = (int)g.noteLines[g.noteRow].length();

            }

        } else if (key == KEY_RIGHT) {

            if (g.noteCol < (int)g.noteLines[g.noteRow].length()) {

                g.noteCol++;

                while (g.noteCol < (int)g.noteLines[g.noteRow].length() && ((unsigned char)g.noteLines[g.noteRow][g.noteCol] & 0xC0) == 0x80) g.noteCol++;

            } else if (g.noteRow < (int)g.noteLines.size() - 1) {

                g.noteRow++;

                g.noteCol = 0;

            }

        } else if (key >= 0x20 && key <= 0x7E) {

            g.noteLines[g.noteRow].insert(g.noteCol, 1, (char)key);

            g.noteCol++;

            g.noteVrowsDirty = true;

        }

        drawNoteEditor();

        return APP_GTD;

    }



    // ── M_FILTER: filter input ──────────────────────────────────────

    if (g.mode == M_FILTER) {

        if (g.imeActive && key != 0) {

            std::string imeOut;

            if (g_ime.handleKey(key, imeOut)) {

                if (!imeOut.empty()) {

                    g.filterText += imeOut;

                }

                rebuildFilter();

                drawList();

                ui_commit();

                return APP_GTD;

            }

        }

        if (key == KEY_IME_TOGGLE) {

            g.imeActive = !g.imeActive;

            g_ime.setActive(g.imeActive);

            drawList();

            ui_commit();

            return APP_GTD;

        }

        if (key == 0x1B) {

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

            g.filterText.clear();

            rebuildFilter();

        } else if (key == 0x0A || key == 0x0D) {

            g.mode = M_BROWSE;

            g.imeActive = false; g_ime.setActive(false);

        } else if (key == 0x7F || key == 0x08) {

            if (!g.filterText.empty()) {

                g.filterText.pop_back();

                rebuildFilter();

            }

        } else if (key >= 0x20 && key <= 0x7E) {

            g.filterText += (char)key;

            rebuildFilter();

        }

        drawList();

        ui_commit();

        return APP_GTD;

    }



    // ── M_BROWSE: main list view ─────────────────────────────────────

    // 任何非 Shift 多选/删除/空闲按键都清除多选状态
    if (key != 0 && key != KEY_SHIFT_UP && key != KEY_SHIFT_DOWN && key != 'd' && key != 'D') {
        clearMultiSel();
    }

    // Project list management (only in project list view)

    if (isInProjectList()) {

        if (key == 'n' || key == 'N') {

            g.mode = M_ADD_PROJECT;

            g.editBuf.clear();

            g.editCur = 0;

            g.imeActive = true;

            g_ime.setActive(true);

        }

        if ((key == 'd' || key == 'D') && g.sel < (int)g.projectList.size()) {

            std::string projName = g.projectList[g.sel];

            int taskCount = 0;

            for (int i = 0; i < (int)tasks.size(); i++) {

                if (tasks[i]["project"].asString() == projName) taskCount++;

            }

            if (taskCount > 0) {

                ctx.statusMessage = "项目非空，无法删除";

            } else {

                // Remove from stored projects list

                auto &projs = g.data["projects"];

                for (int i = (int)projs.size() - 1; i >= 0; i--) {

                    if (projs[i].asString() == projName)

                        projs.elements.erase(projs.elements.begin() + i);

                }

                saveData();

                buildProjectList();

                if (g.sel >= (int)g.projectList.size()) g.sel = (int)g.projectList.size() - 1;

                if (g.sel < 0) g.sel = 0;

                ctx.statusMessage = "已删除项目";

            }

        }

        if ((key == 'r' || key == 'R') && g.sel < (int)g.projectList.size()) {

            g.renameTargetProject = g.projectList[g.sel];

            g.mode = M_RENAME_PROJECT;

            g.editBuf = g.projectList[g.sel];

            g.editCur = (int)g.editBuf.length();

            g.imeActive = true;

            g_ime.setActive(true);

        }

    }



    if (key == 'q' || key == 'Q' || key == 0x1B) {

        if (!g.filterContext.empty() || !g.filterTags.empty()) {

            g.filterContext.clear();

            g.filterTags.clear();

            rebuildFilter();

            return APP_GTD;

        }

        g_ime.setActive(false);

        ctx.nextState = APP_MAIN;

        return APP_MAIN;

    }



    if (key == '\t') {

        g.view = (g.view + 1) % V_COUNT;

        g.sel = 0; g.scroll = 0;

        g.projectDrillIdx = -1;

        if (g.view == V_PROJECT) buildProjectList();

        rebuildFilter();

    }



    if (key == KEY_UP) {

        if (g.sel > 0) g.sel--;

    }

    if (key == KEY_DOWN) {

        if (g.sel < (int)g.filtered.size() - 1) g.sel++;

    }

    // Shift+↑/↓ 连续多选(仅平铺列表; 项目列表/项目树显示顺序与 filtered 不一致)
    if (g.view != V_PROJECT) {

        if (key == KEY_SHIFT_UP) {

            if (g.multiAnchor < 0) g.multiAnchor = g.sel;

            if (g.sel > 0) g.sel--;

            rebuildMultiSel();

        }

        if (key == KEY_SHIFT_DOWN) {

            if (g.multiAnchor < 0) g.multiAnchor = g.sel;

            if (g.sel < (int)g.filtered.size() - 1) g.sel++;

            rebuildMultiSel();

        }

    }



    if (key == 'a' || key == 'A') {

        g.mode = M_ADD;

        g.editBuf.clear();

        g.editCur = 0;

        g.imeActive = true;

        g_ime.setActive(true);

        // same-level: inherit parent and project from selected task, insert after

        if (g.sel < (int)g.filtered.size()) {

            g.pendingParent = tasks[g.filtered[g.sel]]["parent"].asString();

            g.insertAfter = g.filtered[g.sel];

        } else {

            g.pendingParent.clear();

            g.insertAfter = -1;

        }

        // In project drill-down, auto-set project for new task

        if (g.view == V_PROJECT && g.projectDrillIdx >= 0 && g.projectDrillIdx < (int)g.projectList.size()) {

            g.pendingProject = g.projectList[g.projectDrillIdx];

        } else {

            g.pendingProject.clear();

        }

    }



    if (key == ' ' && g.sel < (int)g.filtered.size()) {

        auto &task = tasks[g.filtered[g.sel]];

        std::string s = task["status"].asString("todo");

        if (s == "todo") task.set("status", "doing");

        else if (s == "doing") { task.set("status", "done");

            time_t n; time(&n); struct tm *tm = localtime(&n);

            char b[16]; strftime(b, sizeof(b), "%Y-%m-%d", tm);

            task.set("completed", std::string(b));

        }

        else if (s == "done") task.set("status", "waiting");

        else task.set("status", "todo");

        saveData();

        rebuildFilter();

    }



    if (key == 0x0A || key == 0x0D) {

        if (isInProjectList()) {

            // Project list: drill into selected project

            if (g.sel < (int)g.projectList.size()) {

                g.projectDrillIdx = g.sel;

                g.sel = 0; g.scroll = 0;
                g.foldedNodes.clear();

                rebuildFilter();

            }

        } else if (g.sel < (int)g.filtered.size()) {

            // All tabs: open detail panel

            g.detailTaskIdx = g.filtered[g.sel];

            g.detailField = 0;

            g.mode = M_DETAIL;

            drawDetail();

            ui_commit();

            return APP_GTD;

        }

    }



    if ((key == 'i' || key == 'I') && g.sel < (int)g.filtered.size()) {

        // add subtask under selected, insert after parent

        auto &parentTask = tasks[g.filtered[g.sel]];

        g.pendingParent = parentTask["id"].asString();

        g.insertAfter = g.filtered[g.sel];

        // In project drill-down, auto-set project

        if (g.view == V_PROJECT && g.projectDrillIdx >= 0 && g.projectDrillIdx < (int)g.projectList.size())

            g.pendingProject = g.projectList[g.projectDrillIdx];

        else

            g.pendingProject.clear();

        g.mode = M_ADD;

        g.editBuf.clear();

        g.editCur = 0;

        g.imeActive = true;

        g_ime.setActive(true);

    }



    if (key == '/') {

        g.mode = M_FILTER;

        g.imeActive = true;

        g_ime.setActive(true);

    }



    if (key == 'A') {

        mkdir(ARCHIVE_DIR, 0755);

        loadArchiveMonths();

        g.archiveSel = 0;

        g.archiveScroll = 0;

        g.archiveBrowsing = false;

        g.mode = M_ARCHIVE;

    }



    if (key == 'c' || key == 'C') {

        buildContextList();

        g.ctxMgrSel = 0;

        g.mode = M_CONTEXT_MGR;

    }



    if (key == 't' || key == 'T') {

        buildTagList();

        g.tagMgrSel = 0;

        g.mode = M_TAG_MGR;

    }

    // s: open summary dialog
    if ((key == 's' || key == 'S') && !isInProjectList() && g.sel >= 0 && g.sel < (int)g.filtered.size()) {
        g.detailTaskIdx = g.filtered[g.sel];
        g.summaryScroll = 0;
        g.summaryPrevMode = M_BROWSE;
        g.mode = M_SUMMARY;
        drawList(); drawSummary();
        return APP_GTD;
    }

    // hjkl: reorder and hierarchy adjustment

    if (!isInProjectList() && g.view != V_COMPLETED && g.sel >= 0 && g.sel < (int)g.filtered.size()) {

        auto &ts = g.data["tasks"];

        if (key == 'j' && g.sel > 0) {

            // Move task up in order (swap with above)

            int a = g.filtered[g.sel];

            int b = g.filtered[g.sel - 1];

            std::swap(ts.elements[a], ts.elements[b]);

            g.sel--;

            saveData();

        } else if (key == 'k' && g.sel < (int)g.filtered.size() - 1) {

            // Move task down in order (swap with below)

            int a = g.filtered[g.sel];

            int b = g.filtered[g.sel + 1];

            std::swap(ts.elements[a], ts.elements[b]);

            g.sel++;

            saveData();

        } else if (key == 'h') {
            // Promote: clear parent (increase hierarchy level)
            ts[g.filtered[g.sel]].set("parent", "");
            saveData();
            rebuildFilter();
        } else if (key == 'l') {
            // Demote: find previous sibling (same parent) and make current its child
            auto &task = ts[g.filtered[g.sel]];
            std::string myParent = task["parent"].asString();
            int sibIdx = -1;
            for (int i = g.sel - 1; i >= 0; i--) {
                if (ts[g.filtered[i]]["parent"].asString() == myParent) {
                    sibIdx = g.filtered[i];
                    break;
                }
            }
            if (sibIdx >= 0) {
                task.set("parent", ts[sibIdx]["id"].asString());
                saveData();
                rebuildFilter();
            }

        }

    }



    // z: toggle fold, Z: fold/unfold all (project drill-down only)
    if (g.view == V_PROJECT && g.projectDrillIdx >= 0 && !g_visibleTreeIdx.empty()) {
        if (key == 'z' && g.sel >= 0 && g.sel < (int)g_visibleTreeIdx.size()) {
            int treeIdx = g_visibleTreeIdx[g.sel];
            // Check if has children
            if (treeIdx + 1 < (int)g_gtdTree.size() && g_gtdTree[treeIdx + 1].depth > g_gtdTree[treeIdx].depth) {
                if (g.foldedNodes.count(treeIdx)) g.foldedNodes.erase(treeIdx);
                else g.foldedNodes.insert(treeIdx);
                rebuildFilter();
            }
        }
        if (key == 'Z') {
            if (g.foldedNodes.empty()) {
                // Fold all nodes that have children
                for (int i = 0; i < (int)g_gtdTree.size(); i++) {
                    if (i + 1 < (int)g_gtdTree.size() && g_gtdTree[i + 1].depth > g_gtdTree[i].depth)
                        g.foldedNodes.insert(i);
                }
            } else {
                g.foldedNodes.clear();
            }
            rebuildFilter();
        }
    }



    if (key == 'r' || key == 'R') {

        if (g.sel < (int)g.filtered.size()) {

            g.detailTaskIdx = g.filtered[g.sel];

            g.editBuf = tasks[g.detailTaskIdx]["title"].asString();

            g.editCur = (int)g.editBuf.length();

            g.imeActive = true;

            g_ime.setActive(true);

            g.mode = M_RENAME;

            drawAdd();  // reuse add-task overlay for rename

            ui_commit();

            return APP_GTD;

        }

    }



    if (key == 0x05) {  // Ctrl+E — export

        std::string md = exportMD();

        time_t now; time(&now); struct tm *tm = localtime(&now);

        char fname[64];

        strftime(fname, sizeof(fname), "/sdcard/gtd/gtd_export_%Y%m%d_%H%M%S.md", tm);

        g_journal.saveEntryRaw(std::string(fname).c_str() + 12, md);

        ctx.statusMessage = "已导出Markdown";

    }



    if ((key == 'd' || key == 'D') && g.sel < (int)g.filtered.size()) {

        if (g.view != V_PROJECT && hasMultiSel()) {

            // 批量删除: 收集选中 id 对应的任务下标
            std::vector<int> idxs;
            for (int i = 0; i < (int)tasks.size(); i++) {
                if (g.multiSel.count(tasks[i]["id"].asString()) > 0) idxs.push_back(i);
            }
            if (!idxs.empty()) {
                g.confirmIdxs = idxs;
                g.confirmMsg = "删除所选 " + std::to_string((int)idxs.size()) + " 个任务?";
                g.mode = M_CONFIRM;
            }

        } else {

            int idx = g.filtered[g.sel];

            g.confirmIdx = idx;

            g.confirmMsg = std::string("删除任务「") + tasks[idx]["title"].asString() + "」?";

            g.mode = M_CONFIRM;

        }

    }



    if (key == '?') {

        g.helpScroll = 0;

        g.helpPrevMode = M_BROWSE;

        g.mode = M_HELP;

        drawHelp();

        ui_commit();

        return APP_GTD;

    }



    drawList();

    ui_commit();

    return APP_GTD;

}
