#pragma once

// Flomo 客户端 API。移植自 ../pico/pjournal-pico/main/flomo_api.{h,cpp}
// （C1 Flomo 客户端），逻辑逐字保留；HTTP 层换成本仓的 esp_http_client
// （与 flomo_client.cpp 同一个 TLS/证书配置）。
//
// 一条笔记 = 一个 Memo：远端字段（slug/时间戳/tags）+ 本地缓存字段
// （contentText 便于编辑与搜索、dirty/pendingOp 是"待同步的本地改动"标记）。
//
// 认证沿用设置里的 flomo_email/flomo_pass：token 由 login() 换得后存回设置
// （g_settings.setFlomoToken），与「发送到 Flomo」共用同一份登录信息。

#include <map>
#include <string>
#include <vector>

#include "flomo_json.h"

// 增量拉取每页条数。Flomo 接口允许 200，但 200 条响应解析出来的 JsonValue DOM
// 里全是小于 512B 的小分配（CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512），会挤占
// 内部 RAM；首次全量同步时 WiFi/TLS 又刚占了很大一块，容易 OOM。50 条一页把
// 单页峰值压到约四分之一，代价只是多几次请求（游标会跨同步保存，不必一次拉完）。
constexpr int FLOMO_PAGE_SIZE = 50;

struct Memo {
    std::string slug;
    std::string contentHtml;
    std::string contentText;
    std::vector<std::string> tags;
    std::string createdAt;
    std::string updatedAt;
    bool deleted = false;
    bool dirty = false;
    std::string pendingOp;   // "" | "create" | "update" | "delete"
};

struct ApiResult {
    bool ok = false;
    std::string message;
    JsonValue data;
    int code = -1;   // body 里的业务码（-10/-20 = 令牌过期，需要重新登录）
};

class FlomoApi {
public:
    FlomoApi() = default;
    explicit FlomoApi(std::string token) : token_(std::move(token)) {}

    // 邮箱/密码登录，成功时 data.access_token 是令牌。
    static ApiResult login(const std::string &email, const std::string &password);

    // 分页拉取整库（memo/updated/，按 updated_at 升序）。首屏 latestUpdatedAt 传 0，
    // 翻页时带上上一页最后一条的 slug 与更新时间的 Unix 秒。
    ApiResult listPage(const std::string &latestSlug, long long latestUpdatedAt);
    ApiResult createMemo(const std::string &content);
    ApiResult updateMemo(const std::string &slug, const std::string &content);
    ApiResult deleteMemo(const std::string &slug);

private:
    std::string token_;
    ApiResult request(const std::string &method, const std::string &path,
                      const std::map<std::string, std::string> &params);
};

// 富文本 ↔ 纯文本互转（编辑器里是纯文本，Flomo 存的是那套 <p>/<ul>/<strong>… HTML）。
std::string htmlToText(const std::string &html);
std::string textToHtml(const std::string &text);
std::vector<std::string> extractTags(const std::string &text);

// 把接口返回的一条 memo 转成本地 Memo（已把 content 的 HTML 转成纯文本）。
Memo memoFromJson(const JsonValue &v);
