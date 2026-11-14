// 网络剪贴板 HTTP 服务。
//
// 内存约束：
//   - 网页是 const 字符串，编译进 flash，不占 RAM；
//   - 条目内容直接从 PSRAM 里的历史缓冲区分块发出，不复制；
//   - 上传/保存的请求体一次性分配 PSRAM（上限 = 单条上限），handler 栈上只有
//     1 KB 接收缓冲；httpd 任务栈压到 4 KB，退出网络模式即 httpd_stop 释放；
//   - 只在"网络模式"期间存在，不在常驻路径上增加任何分配。
//
// 并发：esp_http_server 只有一个工作任务，请求串行处理；网络模式下主循环吞掉
// 所有按键（见 clip_menu_handle），不会同时写剪贴板历史，因此历史表无需加锁。

#include "clip_web.h"

#include "clip_history.h"

#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "ClipWeb";

static httpd_handle_t s_server = nullptr;
static uint16_t s_port = 80;

bool clip_web_active() { return s_server != nullptr; }
uint16_t clip_web_port() { return s_port; }

// ── 网页（flash 常量，不占 RAM） ──────────────────────────────────────────

static const char PAGE[] = R"raw(<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>pjournal 网络剪贴板</title>
<style>
body{font-family:system-ui,sans-serif;max-width:820px;margin:0 auto;padding:12px;font-size:14px}
h1{font-size:18px;margin:0 0 8px}
#msg{padding:6px;border-radius:4px;margin:8px 0;display:none}
.ok{background:#d4edda;color:#155724}.err{background:#f8d7da;color:#721c24}
table{width:100%;border-collapse:collapse;margin-bottom:10px}
th,td{padding:6px;border-bottom:1px solid #eee;text-align:left;font-size:13px}
.pv{cursor:pointer;color:#2563eb;word-break:break-all}
button{padding:3px 10px;font-size:12px;cursor:pointer;margin-right:4px}
textarea{width:100%;height:220px;font-family:monospace;font-size:13px;box-sizing:border-box}
.row{display:flex;gap:8px;margin:8px 0;flex-wrap:wrap}
</style></head><body>
<h1>pjournal 网络剪贴板</h1>
<div id="msg"></div>
<table><thead><tr><th>#</th><th>内容（首行）</th><th>字节</th><th>操作</th></tr></thead>
<tbody id="list"></tbody></table>
<div class="row">
<button onclick="newEntry()">新建</button>
<button onclick="save()">保存</button>
<button onclick="del()">删除当前条目</button>
</div>
<textarea id="txt" placeholder="在 PC 上粘贴文本，点“保存”即写入设备剪贴板历史（最新的在最前）"></textarea>
<script>
var cur=-1;
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;')}
function msg(t,ok){var e=document.getElementById('msg');e.textContent=t;e.className=ok?'ok':'err';e.style.display='block';setTimeout(function(){e.style.display='none'},2500)}
function load(){
 fetch('/api/list').then(r=>r.json()).then(d=>{
  var h='';
  d.forEach(e=>{h+='<tr><td>'+e.i+'</td><td class="pv" onclick="edit('+e.i+')">'+esc(e.preview||'（空）')+'</td><td>'+e.len+'</td><td><button onclick="edit('+e.i+')">编辑</button><button onclick="delAt('+e.i+')">删除</button></td></tr>'});
  if(!d.length)h='<tr><td colspan="4">（暂无条目）</td></tr>';
  document.getElementById('list').innerHTML=h;
 }).catch(()=>msg('加载失败',false));
}
function edit(i){fetch('/api/item?i='+i).then(r=>r.text()).then(t=>{cur=i;document.getElementById('txt').value=t;msg('已载入第 '+i+' 条')}).catch(()=>msg('读取失败',false))}
function newEntry(){cur=-1;document.getElementById('txt').value='';msg('输入新文本后点保存')}
function save(){
 var body=document.getElementById('txt').value;
 if(!body){msg('内容为空',false);return}
 var url=cur<0?'/api/add':'/api/item?i='+cur;
 fetch(url,{method:'POST',headers:{'Content-Type':'text/plain; charset=utf-8'},body:body})
 .then(r=>r.json()).then(d=>{if(d.ok){msg('已保存',true);load()}else msg('保存失败: '+(d.error||''),false)})
 .catch(()=>msg('保存失败',false));
}
function del(){if(cur>=0)delAt(cur);else msg('先选择一条',false)}
function delAt(i){if(!confirm('删除第 '+i+' 条?'))return;fetch('/api/delete?i='+i,{method:'POST'}).then(r=>r.json()).then(d=>{
 if(d.ok){if(cur===i)cur=-1;else if(cur>i)cur--;if(cur<0)document.getElementById('txt').value='';}
 msg(d.ok?'已删除':('删除失败: '+(d.error||'')),d.ok);load()})}
load();
</script></body></html>)raw";

// ── 小工具 ───────────────────────────────────────────────────────────────

static bool parseIndex(httpd_req_t *req, int &out) {
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    char v[16];
    if (httpd_query_key_value(q, "i", v, sizeof(v)) != ESP_OK) return false;
    // atoi 对超长数字是未定义行为，用 strtol 并显式限范围。
    char *end = nullptr;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0') return false;
    if (n < 0 || n >= CLIPHIST_MAX_ENTRIES) return false;
    out = (int)n;
    return true;
}

static void sendJson(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

static void sendJsonOk(httpd_req_t *req) { sendJson(req, "{\"ok\":true}"); }

static void sendJsonError(httpd_req_t *req, const char *msg) {
    httpd_resp_set_type(req, "application/json");
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, buf);
}

// 把整个请求体读进一块 PSRAM 缓冲（失败回退内部 RAM）。上限 CLIPHIST_MAX_BYTES。
// 调用方负责 heap_caps_free()。
static char *readBody(httpd_req_t *req, size_t &outLen) {
    outLen = 0;
    const size_t total = (size_t)req->content_len;
    if (total == 0 || total > CLIPHIST_MAX_BYTES) return nullptr;

    char *buf = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "body alloc failed (%u bytes)", (unsigned)total);
        return nullptr;
    }

    size_t got = 0;
    int timeouts = 0;
    char tmp[1024];
    while (got < total) {
        size_t want = total - got;
        if (want > sizeof(tmp)) want = sizeof(tmp);
        int r = httpd_req_recv(req, tmp, want);
        if (r <= 0) {
            // 客户端发一半卡住时不要无限等，超时几次就放弃这次保存。
            // 这个时间也会算进退出网络模式时 httpd_stop() 的等待，别设太长。
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 2) continue;
            heap_caps_free(buf);
            return nullptr;
        }
        memcpy(buf + got, tmp, (size_t)r);
        got += (size_t)r;
    }
    buf[got] = '\0';
    outLen = got;
    return buf;
}

// ── Handlers ─────────────────────────────────────────────────────────────

static esp_err_t handler_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
    return ESP_OK;
}

static esp_err_t handler_list(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, "[", 1);

    const int n = cliphist_count();
    for (int i = 0; i < n; ++i) {
        const char *p = cliphist_text(i);
        const uint32_t len = cliphist_len(i);
        if (!p) continue;

        // 预览：只取首行前 64 字节（UTF-8 边界），控制字符换成空格。
        // 输入 ≤64 字节，转义后 ≤128 字节，je 留足余量；每个分支单独检查
        // 余量（多字节字符一次写 step 字节，不能只按 2 字节算）。
        char je[160];
        size_t jn = 0;
        size_t k = 0;
        while (k < len && k < 64) {
            unsigned char c = (unsigned char)p[k];
            if (c == '\n' || c == '\r') break;
            size_t step = 1;
            if ((c & 0xE0) == 0xC0) step = 2;
            else if ((c & 0xF0) == 0xE0) step = 3;
            else if ((c & 0xF8) == 0xF0) step = 4;
            if (k + step > len || k + step > 64) break;
            if (c == '"' || c == '\\') {
                if (jn + 2 >= sizeof(je)) break;
                je[jn++] = '\\';
                je[jn++] = (char)c;
            } else if (c < 0x20) {
                if (jn + 1 >= sizeof(je)) break;
                je[jn++] = ' ';
            } else {
                if (jn + step >= sizeof(je)) break;
                for (size_t b = 0; b < step; ++b) je[jn++] = p[k + b];
            }
            k += step;
        }
        je[jn] = '\0';

        char line[256];
        int m = snprintf(line, sizeof(line),
                         "{\"i\":%d,\"len\":%u,\"linewise\":%d,\"preview\":\"%s\"}%s",
                         i, (unsigned)len, cliphist_linewise(i) ? 1 : 0, je,
                         (i + 1 < n) ? "," : "");
        if (m <= 0) continue;
        // snprintf 返回的是“应有长度”，被截断时不能直接当发送长度用（会越界读）。
        if ((size_t)m >= sizeof(line)) m = (int)sizeof(line) - 1;
        if (httpd_resp_send_chunk(req, line, (size_t)m) != ESP_OK) return ESP_OK;
    }

    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t handler_item_get(httpd_req_t *req) {
    int i = 0;
    if (!parseIndex(req, i)) {
        sendJsonError(req, "bad index");
        return ESP_OK;
    }
    const char *p = cliphist_text(i);
    const uint32_t len = cliphist_len(i);
    if (!p) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // 直接按块发送 PSRAM 里的原文，不复制整条。
    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > 2048) chunk = 2048;
        if (httpd_resp_send_chunk(req, p + off, chunk) != ESP_OK) return ESP_OK;
        off += chunk;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t handler_item_put(httpd_req_t *req) {
    int i = 0;
    if (!parseIndex(req, i)) {
        sendJsonError(req, "bad index");
        return ESP_OK;
    }
    size_t n = 0;
    char *body = readBody(req, n);
    if (!body) {
        sendJsonError(req, "body empty/too large");
        return ESP_OK;
    }
    const bool ok = cliphist_replace_raw(i, body, n);
    heap_caps_free(body);
    if (ok) sendJsonOk(req);
    else sendJsonError(req, "replace failed");
    return ESP_OK;
}

static esp_err_t handler_add(httpd_req_t *req) {
    size_t n = 0;
    char *body = readBody(req, n);
    if (!body) {
        sendJsonError(req, "body empty/too large");
        return ESP_OK;
    }
    // PC 传来的文本按普通文本处理（不用 vi 的整行语义）。
    const bool ok = cliphist_add_raw(body, n, false);
    heap_caps_free(body);
    if (ok) sendJsonOk(req);
    else sendJsonError(req, "add failed");
    return ESP_OK;
}

static esp_err_t handler_delete(httpd_req_t *req) {
    int i = 0;
    if (!parseIndex(req, i)) {
        sendJsonError(req, "bad index");
        return ESP_OK;
    }
    // 浏览器可能拿着过期的索引（比如另开标签页删过），越界要明确报错。
    if (i >= cliphist_count()) {
        sendJsonError(req, "index out of range");
        return ESP_OK;
    }
    cliphist_remove(i);
    sendJsonOk(req);
    return ESP_OK;
}

// ── Public API ───────────────────────────────────────────────────────────

// 起服务。task_caps 决定 httpd 任务栈落在哪块内存：
// IDF 的 xTaskCreatePinnedToCoreWithCaps 会把 stack_size 乘 StackType_t，
// 4096 → 16 KB 的**连续**分配。内部 RAM 被 IME 索引占过之后最大连续块可能只剩
// 几 KB（见 bt_keyboard.cpp 里 bt_conn 的注释），所以优先放 PSRAM；PSRAM 不够
// 时才退回内部。TCB 始终在内部（几百字节），不进这里的账。
static bool startWithCaps(uint16_t port, uint32_t caps, const char *what) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.max_uri_handlers = 6;   // 正好 6 个路由，不预留给别的
    cfg.max_open_sockets = 3;   // 单浏览器页面足够
    cfg.max_resp_headers = 4;   // 只用到 Content-Type / Cache-Control
    cfg.stack_size = 4096;      // 栈上只有 1 KB 接收缓冲，任务栈放 PSRAM
    cfg.task_caps = caps;
    // 超时压短：退出网络模式时 httpd_stop() 要等当前请求收尾，
    // 客户端卡住时这段时间也卡住主循环，默认 5 秒太长。
    cfg.recv_wait_timeout = 3;
    cfg.send_wait_timeout = 2;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = nullptr;
        ESP_LOGW(TAG, "httpd_start (%s) failed, port %u, internal largest=%u",
                 what, (unsigned)port,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    return true;
}

bool clip_web_start(uint16_t port) {
    if (s_server) return true;

    if (!startWithCaps(port, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, "psram") &&
        !startWithCaps(port, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "internal")) {
        return false;
    }
    s_port = port;

    static const httpd_uri_t uris[] = {
        {"/",           HTTP_GET,  handler_index,     nullptr},
        {"/api/list",   HTTP_GET,  handler_list,      nullptr},
        {"/api/item",   HTTP_GET,  handler_item_get,  nullptr},
        {"/api/item",   HTTP_POST, handler_item_put,  nullptr},
        {"/api/add",    HTTP_POST, handler_add,       nullptr},
        {"/api/delete", HTTP_POST, handler_delete,    nullptr},
    };
    for (const auto &u : uris) {
        esp_err_t err = httpd_register_uri_handler(s_server, &u);
        if (err != ESP_OK) {
            // 注册失败就整个撤掉，别留一个半残的服务占着端口和任务。
            ESP_LOGE(TAG, "register %s failed: %s", u.uri, esp_err_to_name(err));
            clip_web_stop();
            return false;
        }
    }

    ESP_LOGI(TAG, "started on port %u (internal free=%u largest=%u, psram free=%u)",
             (unsigned)port,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

void clip_web_stop() {
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(TAG, "stopped (internal free=%u largest=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
