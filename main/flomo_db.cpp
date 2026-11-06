#include "flomo_db.h"

#include "flomo_json.h"
#include "safe_file.h"

#include <algorithm>

std::string MemoDb::path() const { return dir_ + "/memos.json"; }

// 返回值区分"没有库"和"库读不出来"：
//   true  = 读到一份有效的库（文件不存在也算：新用户，空库）
//   false = 文件在，但读取/解析失败（SD 读错误、文件损坏、超过 4MB 上限）
// 调用方必须区别对待 —— 同步把"读失败"当成空库继续跑，收工会拿空库覆盖掉
// 整库（用户笔记全丢）。
bool MemoDb::load(Store &store) {
    ensureDirPath(dir_);
    repairSafeWriteFile(path());          // 主文件丢了还有 .bak 时先修回来
    if (!fileExists(path())) return true; // 还没有库文件：新用户
    JsonValue root = JsonValue::loadFromFile(path());
    if (!root.isObject()) return false;
    store.token = root["token"].asString();
    store.lastSync = root["last_sync"].asString();
    store.syncUpdatedAt = (long long)root["sync_updated_at"].asNumber(0);
    store.syncSlug = root["sync_slug"].asString();
    const JsonValue &arr = root["memos"];
    if (arr.isArray()) {
        for (size_t i = 0; i < arr.size(); ++i) {
            Memo m = memoFromJson(arr[i]);
            m.contentText = arr[i]["content_text"].asString(m.contentText);
            m.dirty = arr[i]["dirty"].asBool(false);
            m.pendingOp = arr[i]["pending_op"].asString();
            store.memos.push_back(m);
        }
    }
    sortMemos(store.memos);
    return true;
}

// 直接拼 JSON 文本，不建 JsonValue DOM。整库几十上百条笔记时，DOM 里每个字段都是
// 一个小于 512B 的 vector/string 分配（CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512），
// 全落在内部 RAM；同步收工那会儿 WiFi 刚断，内部内存本来就紧，SD 卡的 DMA 临时缓冲
// （FATFS 缓冲在 PSRAM，S3 的 SDMMC 不能直接 DMA PSRAM）就分配不出来了，表现为
// sdmmc_cmd allocate_dma_buf err=0x101 + EIO。直接拼串只有一块大缓冲（>512B 走
// PSRAM），内部 RAM 压力小得多。
static void appendJsonString(std::string &out, const std::string &s) {
    static const char *hex = "0123456789abcdef";
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                out += "\\u00";
                out += hex[((unsigned char)c) >> 4];
                out += hex[((unsigned char)c) & 15];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

bool MemoDb::save(const Store &store) {
    ensureDirPath(dir_);
    std::string out;
    out.reserve(256 + store.memos.size() * 512);
    out += "{\"token\":";
    appendJsonString(out, store.token);
    out += ",\"last_sync\":";
    appendJsonString(out, store.lastSync);
    out += ",\"sync_updated_at\":";
    out += std::to_string(store.syncUpdatedAt);
    out += ",\"sync_slug\":";
    appendJsonString(out, store.syncSlug);
    out += ",\"memos\":[";
    bool firstMemo = true;
    for (const auto &m : store.memos) {
        if (!firstMemo) out += ',';
        firstMemo = false;
        out += "{\"slug\":";
        appendJsonString(out, m.slug);
        out += ",\"content\":";
        appendJsonString(out, m.contentHtml);
        out += ",\"content_text\":";
        appendJsonString(out, m.contentText);
        out += ",\"created_at\":";
        appendJsonString(out, m.createdAt);
        out += ",\"updated_at\":";
        appendJsonString(out, m.updatedAt);
        out += ",\"deleted_at\":";
        if (m.deleted) appendJsonString(out, "1");
        else out += "null";
        out += ",\"dirty\":";
        out += m.dirty ? "true" : "false";
        out += ",\"pending_op\":";
        appendJsonString(out, m.pendingOp);
        out += ",\"tags\":[";
        for (size_t i = 0; i < m.tags.size(); ++i) {
            if (i) out += ',';
            appendJsonString(out, m.tags[i]);
        }
        out += "]}";
    }
    out += "]}";
    return safeWriteFile(path(), out);
}

void upsertMemo(std::vector<Memo> &memos, const Memo &memo) {
    for (auto &m : memos) {
        if (m.slug == memo.slug) {
            bool keepLocal = m.dirty && m.pendingOp != "delete";
            if (!keepLocal) m = memo;
            return;
        }
    }
    memos.push_back(memo);
}

void sortMemos(std::vector<Memo> &memos) {
    std::sort(memos.begin(), memos.end(), [](const Memo &a, const Memo &b) {
        return a.updatedAt > b.updatedAt;
    });
}
