#pragma once

// 本地笔记库。移植自 ../pico/pjournal-pico/main/flomo_db.{h,cpp}：整个库就是
// <dir>/memos.json 一个文件，原子写（safe_file）。

#include <string>
#include <vector>

#include "flomo_api.h"

struct Store {
    std::string token;
    std::vector<Memo> memos;
    std::string lastSync;      // 上次同步时间戳（展示用）
    // 增量同步游标：上次成功拉到的最后一条（memo/updated/ 的 latest_* 参数）。
    long long syncUpdatedAt = 0;   // Unix 秒，0 = 从头拉
    std::string syncSlug;
};

class MemoDb {
public:
    explicit MemoDb(std::string dir) : dir_(std::move(dir)) {}
    // false = 库文件存在但读不出来（损坏/超限/SD 错误）。这时绝不能当成空库写回。
    bool load(Store &store);
    bool save(const Store &store);
    const std::string &dir() const { return dir_; }

private:
    std::string path() const;
    std::string dir_;
};

// 落库去重规则（与 Flomo 一致）：同 slug 已存在时，若本地这条有未推上去的改动
// （dirty 且不是待删除），保留本地版本，否则用远端覆盖。新 slug 直接追加。
void upsertMemo(std::vector<Memo> &memos, const Memo &memo);

// 按 updatedAt 倒序（字符串比较即可，ISO 时间戳）。
void sortMemos(std::vector<Memo> &memos);
