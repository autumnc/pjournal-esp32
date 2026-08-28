#include "file_manager_server.h"
#include "journal_storage.h"
#include "settings_manager.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>
#include <algorithm>
#include <cstdint>

static const char *TAG = "FileMgr";
static httpd_handle_t s_server = nullptr;
static uint16_t s_port = 80;

// ── Helpers ──────────────────────────────────────────────────────────────

static std::string urlDecode(const char *src) {
    std::string out;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            out += (char)strtol(hex, nullptr, 16);
            src += 3;
        } else if (*src == '+') {
            out += ' ';
            src++;
        } else {
            out += *src++;
        }
    }
    return out;
}

static bool isSafePath(const std::string &path) {
    if (path.find("..") != std::string::npos) return false;
    // /sdcard must be the mount root itself or followed by '/', otherwise
    // "/sdcard2/..." would bypass the check.
    if (path.compare(0, 7, "/sdcard") != 0) return false;
    if (path.size() > 7 && path[7] != '/') return false;
    return true;
}

static std::string formatSize(off_t size) {
    char buf[32];
    if (size < 1024) snprintf(buf, sizeof(buf), "%lld B", (long long)size);
    else if (size < 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f KB", size / 1024.0);
    else snprintf(buf, sizeof(buf), "%.1f MB", size / (1024.0 * 1024.0));
    return buf;
}

static void sendJsonOK(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void sendJsonError(httpd_req_t *req, const char *msg) {
    httpd_resp_set_type(req, "application/json");
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, buf);
}

static std::string getQueryParam(httpd_req_t *req, const char *key) {
    size_t len = httpd_req_get_url_query_len(req);
    if (len == 0) return "";
    std::vector<char> query(len + 1);
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) return "";
    char val[512] = {};
    if (httpd_query_key_value(query.data(), key, val, sizeof(val)) != ESP_OK) return "";
    return urlDecode(val);
}

// Optional access token. When a "文件管理密码" is set in settings, every
// request must carry it via the X-Auth-Token header or the ?token= query
// param. Empty setting keeps the old open access.
static bool authOk(httpd_req_t *req) {
    std::string expected = g_settings.getString("file_mgr_token");
    if (expected.empty()) return true;
    char buf[128] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", buf, sizeof(buf)) == ESP_OK &&
        expected == buf) return true;
    std::string q = getQueryParam(req, "token");
    return !q.empty() && q == expected;
}

static esp_err_t sendAuthError(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    sendJsonError(req, "unauthorized");
    return ESP_OK;
}

// ── ZIP helpers (store mode, no compression) ────────────────────────────

struct ZipEntry {
    std::string relPath;   // relative path inside zip
    uint32_t crc32;
    uint32_t size;
    uint32_t offset;       // offset of local file header in zip stream
};

static uint32_t crc32Table[256];
static bool crc32TableInit = false;
static const size_t MAX_ZIP_FILES = 512;
static const uint64_t MAX_ZIP_TOTAL_SIZE = 16ULL * 1024 * 1024;

static void initCRC32Table() {
    if (crc32TableInit) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32Table[i] = c;
    }
    crc32TableInit = true;
}

static uint32_t computeCRC32(const uint8_t *data, size_t len, uint32_t crc = 0xFFFFFFFF) {
    initCRC32Table();
    for (size_t i = 0; i < len; i++)
        crc = crc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static uint32_t crc32Finish(uint32_t crc) { return crc ^ 0xFFFFFFFF; }

// Write little-endian uint16/uint32 into buffer
static void putU16(uint8_t *buf, uint16_t v) { buf[0]=v; buf[1]=v>>8; }
static void putU32(uint8_t *buf, uint32_t v) { buf[0]=v; buf[1]=v>>8; buf[2]=v>>16; buf[3]=v>>24; }

// Recursively collect files under dirPath, storing relative paths from basePath
static void collectFiles(const std::string &dirPath, const std::string &basePath,
                         std::vector<ZipEntry> &entries, uint64_t &totalSize) {
    DIR *dir = opendir(dirPath.c_str());
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (entries.size() >= MAX_ZIP_FILES || totalSize >= MAX_ZIP_TOTAL_SIZE) break;
        if (ent->d_name[0] == '.') continue;
        std::string full = dirPath + "/" + ent->d_name;
        std::string rel = basePath.empty() ? ent->d_name : basePath + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collectFiles(full, rel, entries, totalSize);
        } else {
            if (rel.length() > 0xFFFF) {
                ESP_LOGW(TAG, "Skipping path too long for ZIP (%d bytes): %s", (int)rel.length(), rel.c_str());
                continue;
            }
            if (totalSize + (uint64_t)st.st_size > MAX_ZIP_TOTAL_SIZE) {
                ESP_LOGW(TAG, "Skipping ZIP file beyond size cap: %s", rel.c_str());
                continue;
            }
            ZipEntry e;
            e.relPath = rel;
            e.crc32 = 0;
            e.size = (uint32_t)st.st_size;
            e.offset = 0;
            entries.push_back(e);
            totalSize += (uint64_t)st.st_size;
        }
    }
    closedir(dir);
}

// ── Embedded HTML ────────────────────────────────────────────────────────

static const char *HTML_PAGE = R"raw(<!DOCTYPE html>
<html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>文件管理</title>
<style>
body{font-family:sans-serif;max-width:800px;margin:0 auto;padding:12px;font-size:14px}
h1{font-size:18px;margin:0 0 12px}
#breadcrumb{margin-bottom:8px;color:#666}
table{width:100%;border-collapse:collapse}
th,td{padding:6px 8px;text-align:left;border-bottom:1px solid #eee;font-size:13px}
th{background:#f5f5f5;font-weight:600}
.dir{color:#2563eb;cursor:pointer}
.dir:hover{text-decoration:underline}
.act{white-space:nowrap}
.act button{margin:0 2px;padding:2px 8px;font-size:12px;cursor:pointer}
#toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:12px 0}
#toolbar input[type=text]{width:120px;padding:3px 6px}
#toolbar input[type=file]{font-size:12px}
#msg{padding:6px;margin:8px 0;border-radius:4px;display:none}
.ok{background:#d4edda;color:#155724}
.err{background:#f8d7da;color:#721c24}
</style></head><body>
<h1>pjournal - 文件管理</h1>
<div id="breadcrumb"></div>
<div id="toolbar">
<input type="file" id="fileInput">
<button onclick="upload()">上传</button>
<input type="text" id="dirName" placeholder="文件夹名">
<button onclick="mkdir()">新建</button>
<button onclick="setToken()">密码</button>
</div>
<div id="msg"></div>
<table><thead><tr><th>名称</th><th>大小</th><th>操作</th></tr></thead>
<tbody id="list"></tbody></table>
<script>
var curPath='/sdcard';
var token='';try{token=localStorage.getItem('pjournal_token')||''}catch(e){}
function hd(){return token?{'X-Auth-Token':token}:{}}
function setToken(){var t=prompt('文件管理密码(留空则不设)',token);if(t!==null){token=t.trim();try{localStorage.setItem('pjournal_token',token)}catch(e){}}}
function showMsg(t,ok){var e=document.getElementById('msg');e.textContent=t;e.className=ok?'ok':'err';e.style.display='block';setTimeout(function(){e.style.display='none'},3000)}
function loadDir(p){
  curPath=p;
  fetch('/api/list?path='+encodeURIComponent(p),{headers:hd()}).then(r=>r.json()).then(d=>{
    document.getElementById('breadcrumb').textContent=d.path;
    var h='';
    if(d.path!=='/sdcard') h+='<tr><td class="dir" onclick="loadDir(\''+esc(p.replace(/\/[^/]+$/,''))+'\')">..</td><td></td><td></td></tr>';
    d.entries.forEach(e=>{
      var fp=esc((d.path==='/'?'':d.path)+'/'+e.name);
      if(e.type==='dir') h+='<tr><td class="dir" onclick="loadDir(\''+fp+'\')">'+esc(e.name)+'/</td><td></td><td class="act"><button onclick="dlDir(\''+fp+'\')">下载</button><button onclick="del(\''+fp+'\',true)">删除</button></td></tr>';
      else h+='<tr><td>'+esc(e.name)+'</td><td>'+e.size+'</td><td class="act"><button onclick="dl(\''+fp+'\')">下载</button><button onclick="del(\''+fp+'\',false)">删除</button></td></tr>';
    });
    document.getElementById('list').innerHTML=h;
  }).catch(e=>showMsg('加载失败',false));
}
function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/'/g,'&#39;')}
function upload(){
  var f=document.getElementById('fileInput').files[0];if(!f)return;
  var fd=new FormData();fd.append('file',f);
  fetch('/api/upload?path='+encodeURIComponent(curPath)+'&name='+encodeURIComponent(f.name),{method:'POST',body:fd,headers:hd()})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'上传成功':'上传失败: '+d.error,d.ok);loadDir(curPath)})
  .catch(()=>showMsg('上传失败',false));
}
function dl(p){window.open('/api/download?path='+encodeURIComponent(p)+'&token='+encodeURIComponent(token))}
function dlDir(p){window.open('/api/download_dir?path='+encodeURIComponent(p)+'&token='+encodeURIComponent(token))}
function del(p,isDir){
  if(!confirm('确认删除?'))return;
  fetch('/api/delete?path='+encodeURIComponent(p)+'&dir='+isDir,{method:'POST',headers:hd()})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'删除成功':'删除失败: '+d.error,d.ok);loadDir(curPath)})
  .catch(()=>showMsg('删除失败',false));
}
function mkdir(){
  var n=document.getElementById('dirName').value.trim();if(!n)return;
  fetch('/api/mkdir?path='+encodeURIComponent(curPath+'/'+n),{method:'POST',headers:hd()})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'创建成功':'创建失败: '+d.error,d.ok);if(d.ok){document.getElementById('dirName').value='';loadDir(curPath)}})
  .catch(()=>showMsg('创建失败',false));
}
loadDir('/sdcard');
</script></body></html>)raw";

// ── URI Handlers ─────────────────────────────────────────────────────────

static esp_err_t __attribute__((unused)) handler_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, HTML_PAGE);
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_list(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    if (path.empty()) path = "/sdcard";
    if (!isSafePath(path)) {
        sendJsonError(req, "invalid path");
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    DIR *dir = opendir(path.c_str());
    if (!dir) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        sendJsonError(req, "cannot open directory");
        return ESP_OK;
    }

    std::string json = "{\"path\":\"";
    json += path;
    json += "\",\"entries\":[";

    struct dirent *ent;
    bool first = true;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = path + "/" + ent->d_name;
        struct stat st;
        bool isDir = false;
        off_t fsize = 0;
        if (stat(full.c_str(), &st) == 0) {
            isDir = S_ISDIR(st.st_mode);
            fsize = st.st_size;
        }
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"";
        // escape JSON string
        for (const char *p = ent->d_name; *p; p++) {
            if (*p == '"' || *p == '\\') json += '\\';
            json += *p;
        }
        json += "\",\"type\":\"";
        json += isDir ? "dir" : "file";
        json += "\",\"size\":\"";
        json += formatSize(fsize);
        json += "\"}";
    }
    closedir(dir);
    if (mtx) xSemaphoreGiveRecursive(mtx);

    json += "]}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_download(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    if (!isSafePath(path)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // extract filename for Content-Disposition
    std::string filename = path;
    auto slash = filename.rfind('/');
    if (slash != std::string::npos) filename = filename.substr(slash + 1);

    httpd_resp_set_type(req, "application/octet-stream");
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s\"", filename.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);

    char buf[4096];
    size_t n;
    while (true) {
        n = fread(buf, 1, sizeof(buf), f);
        if (mtx) xSemaphoreGiveRecursive(mtx);
        if (n == 0) break;
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
        if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    fclose(f);
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_download_dir(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    if (!isSafePath(path)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    // Collect all files
    std::vector<ZipEntry> entries;
    std::string dirName = path;
    auto slash = dirName.rfind('/');
    if (slash != std::string::npos) dirName = dirName.substr(slash + 1);
    uint64_t totalSize = 0;
    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    collectFiles(path, dirName, entries, totalSize);
    if (mtx) xSemaphoreGiveRecursive(mtx);

    if (entries.empty()) {
        sendJsonError(req, "empty directory");
        return ESP_OK;
    }

    // Set response headers
    httpd_resp_set_type(req, "application/zip");
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s.zip\"", dirName.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);

    // Write zip: local file headers + file data, then central directory, then end record
    uint8_t buf[4096];      // file read buffer
    uint32_t offset = 0;

    // Pass 1: write local file headers + data
    for (auto &e : entries) {
        e.offset = offset;
        size_t nameLen = e.relPath.length();
        std::vector<uint8_t> lfh(30 + nameLen);

        // Local file header (30 + nameLen bytes)
        putU32(lfh.data() + 0, 0x04034b50);   // signature
        putU16(lfh.data() + 4, 20);           // version needed
        putU16(lfh.data() + 6, 0);            // flags
        putU16(lfh.data() + 8, 0);            // compression: store
        putU16(lfh.data() + 10, 0);           // mod time
        putU16(lfh.data() + 12, 0);           // mod date
        putU32(lfh.data() + 14, 0);           // crc32 (placeholder, fill after reading)
        putU32(lfh.data() + 18, 0);           // compressed size (placeholder)
        putU32(lfh.data() + 22, 0);           // uncompressed size (placeholder)
        putU16(lfh.data() + 26, (uint16_t)nameLen);  // filename length
        putU16(lfh.data() + 28, 0);           // extra field length
        memcpy(lfh.data() + 30, e.relPath.c_str(), nameLen);

        // Read file, compute CRC32 incrementally
        std::string fullPath = path + "/" + e.relPath.substr(dirName.length() + 1);
        if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
        FILE *f = fopen(fullPath.c_str(), "rb");
        uint32_t crc = 0xFFFFFFFF;
        uint32_t fsize = 0;
        if (f) {
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                crc = computeCRC32(buf, n, crc);
                fsize += n;
            }
            rewind(f);
        }
        if (mtx) xSemaphoreGiveRecursive(mtx);
        crc = crc32Finish(crc);

        // Fill in CRC and sizes
        putU32(lfh.data() + 14, crc);
        putU32(lfh.data() + 18, fsize);
        putU32(lfh.data() + 22, fsize);
        e.crc32 = crc;
        e.size = fsize;

        // Send local file header
        if (httpd_resp_send_chunk(req, (const char*)lfh.data(), 30 + nameLen) != ESP_OK) {
            if (f) fclose(f);
            return ESP_OK;
        }
        offset += 30 + nameLen;

        // Send file data
        if (f) {
            size_t n;
            while (true) {
                if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
                n = fread(buf, 1, sizeof(buf), f);
                if (mtx) xSemaphoreGiveRecursive(mtx);
                if (n == 0) break;
                if (httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) {
                    fclose(f);
                    return ESP_OK;
                }
            }
            fclose(f);
        }
        offset += fsize;
    }

    // Pass 2: central directory
    std::vector<uint8_t> cdh;  // central directory header buffer
    uint32_t cdOffset = offset;

    for (auto &e : entries) {
        size_t nameLen = e.relPath.length();
        cdh.resize(46 + nameLen);

        putU32(cdh.data() + 0, 0x02014b50);   // signature
        putU16(cdh.data() + 4, 20);           // version made by
        putU16(cdh.data() + 6, 20);           // version needed
        putU16(cdh.data() + 8, 0);            // flags
        putU16(cdh.data() + 10, 0);           // compression: store
        putU16(cdh.data() + 12, 0);           // mod time
        putU16(cdh.data() + 14, 0);           // mod date
        putU32(cdh.data() + 16, e.crc32);     // crc32
        putU32(cdh.data() + 20, e.size);      // compressed size
        putU32(cdh.data() + 24, e.size);      // uncompressed size
        putU16(cdh.data() + 28, (uint16_t)nameLen);  // filename length
        putU16(cdh.data() + 30, 0);           // extra field length
        putU16(cdh.data() + 32, 0);           // file comment length
        putU16(cdh.data() + 34, 0);           // disk number start
        putU16(cdh.data() + 36, 0);           // internal file attributes
        putU32(cdh.data() + 38, 0);           // external file attributes
        putU32(cdh.data() + 42, e.offset);    // relative offset of local header
        memcpy(cdh.data() + 46, e.relPath.c_str(), nameLen);

        if (httpd_resp_send_chunk(req, (const char*)cdh.data(), 46 + nameLen) != ESP_OK) {
            return ESP_OK;
        }
        offset += 46 + nameLen;
    }

    uint32_t cdSize = offset - cdOffset;

    // End of central directory record
    uint8_t eocd[22];
    putU32(eocd + 0, 0x06054b50);          // signature
    putU16(eocd + 4, 0);                   // disk number
    putU16(eocd + 6, 0);                   // disk with central dir
    putU16(eocd + 8, (uint16_t)entries.size());  // entries on this disk
    putU16(eocd + 10, (uint16_t)entries.size()); // total entries
    putU32(eocd + 12, cdSize);             // central dir size
    putU32(eocd + 16, cdOffset);           // central dir offset
    putU16(eocd + 20, 0);                  // comment length

    httpd_resp_send_chunk(req, (const char*)eocd, 22);
    httpd_resp_send_chunk(req, nullptr, 0);

    ESP_LOGI(TAG, "ZIP download: %s (%d files)", path.c_str(), (int)entries.size());
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_upload(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string dir = getQueryParam(req, "path");
    std::string name = getQueryParam(req, "name");
    if (!isSafePath(dir) || name.empty()) {
        sendJsonError(req, "invalid path or name");
        return ESP_OK;
    }

    // reject names with path separators
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        sendJsonError(req, "invalid filename");
        return ESP_OK;
    }

    std::string fullpath = dir + "/" + name;
    std::string tmpPath = dir + "/." + name + ".upload";

    // Cap uploads so an oversized body can't exhaust heap or fill the card.
    const size_t MAX_UPLOAD = 8 * 1024 * 1024;
    size_t content_len = req->content_len;
    if (content_len == 0) {
        sendJsonError(req, "empty body");
        return ESP_OK;
    }
    if (content_len > MAX_UPLOAD) {
        sendJsonError(req, "file too large");
        return ESP_OK;
    }

    // get content-type to find boundary
    char ct[128] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        sendJsonError(req, "no content-type");
        return ESP_OK;
    }

    // find boundary
    std::string ctStr(ct);
    auto bpos = ctStr.find("boundary=");
    if (bpos == std::string::npos) {
        sendJsonError(req, "no boundary");
        return ESP_OK;
    }
    std::string boundary = "--" + ctStr.substr(bpos + 9);

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    FILE *f = fopen(tmpPath.c_str(), "wb");
    if (!f) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        sendJsonError(req, "cannot create file");
        return ESP_OK;
    }
    if (mtx) xSemaphoreGiveRecursive(mtx);

    // Stream the multipart body to disk with a sliding window so only a few KB
    // are buffered instead of the whole upload. The file part's data ends at
    // "\r\n--boundary", which also consumes the trailing CRLF.
    const size_t CHUNK = 2048;
    const std::string hdrSep = "\r\n\r\n";
    // boundary already carries the leading "--", so the part separator in the
    // body is "\r\n--<value>", i.e. "\r\n" + boundary.
    const std::string marker = "\r\n" + boundary;
    const size_t keep = marker.size() - 1;   // tail kept as a possible partial marker
    std::string window;
    size_t remaining = content_len;
    bool inHeader = true;
    bool done = false;
    bool ok = true;
    size_t written = 0;

    while (remaining > 0 && !done && ok) {
        char raw[CHUNK];
        size_t want = remaining < CHUNK ? remaining : CHUNK;
        int r = httpd_req_recv(req, raw, want);
        if (r <= 0) { ok = false; break; }
        remaining -= (size_t)r;
        window.append(raw, (size_t)r);

        if (inHeader) {
            auto it = std::search(window.begin(), window.end(), hdrSep.begin(), hdrSep.end());
            if (it == window.end()) {
                if (window.size() > 8192) ok = false;   // malformed: no header end
                continue;
            }
            window.erase(window.begin(), it + (int)hdrSep.size());
            inHeader = false;
        }

        while (!done && !window.empty()) {
            auto it = std::search(window.begin(), window.end(), marker.begin(), marker.end());
            if (it != window.end()) {
                size_t n = (size_t)(it - window.begin());
                if (n > 0) {
                    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
                    if (fwrite(window.data(), 1, n, f) != n) ok = false;
                    if (mtx) xSemaphoreGiveRecursive(mtx);
                    written += n;
                }
                window.clear();
                done = true;
                break;
            }
            if (window.size() <= keep) break;   // need more data before deciding
            size_t writeNow = window.size() - keep;
            if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
            if (fwrite(window.data(), 1, writeNow, f) != writeNow) ok = false;
            if (mtx) xSemaphoreGiveRecursive(mtx);
            written += writeNow;
            window.erase(0, writeNow);
        }
    }
    // Body ended without the closing boundary — flush whatever is buffered.
    if (ok && !inHeader && !done && !window.empty()) {
        if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
        if (fwrite(window.data(), 1, window.size(), f) != window.size()) ok = false;
        if (mtx) xSemaphoreGiveRecursive(mtx);
        written += window.size();
        window.clear();
    }
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (mtx) xSemaphoreGiveRecursive(mtx);

    if (!ok) {
        remove(tmpPath.c_str());   // discard the partial file
        sendJsonError(req, "upload failed");
        return ESP_OK;
    }
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);
    remove(fullpath.c_str());
    bool renamed = rename(tmpPath.c_str(), fullpath.c_str()) == 0;
    if (mtx) xSemaphoreGiveRecursive(mtx);
    if (!renamed) {
        remove(tmpPath.c_str());
        sendJsonError(req, "upload failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Uploaded: %s (%d bytes)", fullpath.c_str(), (int)written);
    sendJsonOK(req);
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_delete(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    std::string dirFlag = getQueryParam(req, "dir");
    if (!isSafePath(path)) {
        sendJsonError(req, "invalid path");
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    int ret;
    if (dirFlag == "1" || dirFlag == "true") {
        ret = rmdir(path.c_str());
    } else {
        ret = remove(path.c_str());
    }
    if (mtx) xSemaphoreGiveRecursive(mtx);

    if (ret == 0) {
        ESP_LOGI(TAG, "Deleted: %s", path.c_str());
        sendJsonOK(req);
    } else {
        sendJsonError(req, "delete failed");
    }
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_mkdir(httpd_req_t *req) {
    if (!authOk(req)) return sendAuthError(req);
    std::string path = getQueryParam(req, "path");
    if (!isSafePath(path)) {
        sendJsonError(req, "invalid path");
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    int ret = mkdir(path.c_str(), 0777);
    if (mtx) xSemaphoreGiveRecursive(mtx);

    if (ret == 0) {
        ESP_LOGI(TAG, "Created dir: %s", path.c_str());
        sendJsonOK(req);
    } else {
        sendJsonError(req, "mkdir failed");
    }
    return ESP_OK;
}

// ── Public API ───────────────────────────────────────────────────────────

bool file_manager_server_start(uint16_t port) {
    if (s_server) return true;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return false;
    }
    s_port = port;

    httpd_uri_t uris[] = {
        {"/",                HTTP_GET,  handler_index,        nullptr},
        {"/api/list",        HTTP_GET,  handler_list,         nullptr},
        {"/api/download",    HTTP_GET,  handler_download,     nullptr},
        {"/api/download_dir",HTTP_GET,  handler_download_dir, nullptr},
        {"/api/upload",      HTTP_POST, handler_upload,       nullptr},
        {"/api/delete",      HTTP_POST, handler_delete,       nullptr},
        {"/api/mkdir",       HTTP_POST, handler_mkdir,        nullptr},
    };
    for (auto &u : uris) {
        httpd_register_uri_handler(s_server, &u);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", port);
    return true;
}

void file_manager_server_stop() {
    if (s_server) {
        httpd_stop(s_server);
        s_server = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

uint16_t file_manager_server_get_port() {
    return s_port;
}
