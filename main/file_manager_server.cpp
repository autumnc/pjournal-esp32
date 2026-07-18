#include "file_manager_server.h"
#include "journal_storage.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>

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
    return path.compare(0, 7, "/sdcard") == 0;
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

// ── ZIP helpers (store mode, no compression) ────────────────────────────

struct ZipEntry {
    std::string relPath;   // relative path inside zip
    uint32_t crc32;
    uint32_t size;
    uint32_t offset;       // offset of local file header in zip stream
};

static uint32_t crc32Table[256];
static bool crc32TableInit = false;

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
                         std::vector<ZipEntry> &entries) {
    DIR *dir = opendir(dirPath.c_str());
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = dirPath + "/" + ent->d_name;
        std::string rel = basePath.empty() ? ent->d_name : basePath + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collectFiles(full, rel, entries);
        } else {
            ZipEntry e;
            e.relPath = rel;
            e.crc32 = 0;
            e.size = (uint32_t)st.st_size;
            e.offset = 0;
            entries.push_back(e);
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
</div>
<div id="msg"></div>
<table><thead><tr><th>名称</th><th>大小</th><th>操作</th></tr></thead>
<tbody id="list"></tbody></table>
<script>
var curPath='/sdcard';
function showMsg(t,ok){var e=document.getElementById('msg');e.textContent=t;e.className=ok?'ok':'err';e.style.display='block';setTimeout(function(){e.style.display='none'},3000)}
function loadDir(p){
  curPath=p;
  fetch('/api/list?path='+encodeURIComponent(p)).then(r=>r.json()).then(d=>{
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
  fetch('/api/upload?path='+encodeURIComponent(curPath)+'&name='+encodeURIComponent(f.name),{method:'POST',body:fd})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'上传成功':'上传失败: '+d.error,d.ok);loadDir(curPath)})
  .catch(()=>showMsg('上传失败',false));
}
function dl(p){window.open('/api/download?path='+encodeURIComponent(p))}
function dlDir(p){window.open('/api/download_dir?path='+encodeURIComponent(p))}
function del(p,isDir){
  if(!confirm('确认删除?'))return;
  fetch('/api/delete?path='+encodeURIComponent(p)+'&dir='+isDir,{method:'POST'})
  .then(r=>r.json()).then(d=>{showMsg(d.ok?'删除成功':'删除失败: '+d.error,d.ok);loadDir(curPath)})
  .catch(()=>showMsg('删除失败',false));
}
function mkdir(){
  var n=document.getElementById('dirName').value.trim();if(!n)return;
  fetch('/api/mkdir?path='+encodeURIComponent(curPath+'/'+n),{method:'POST'})
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
    std::string path = getQueryParam(req, "path");
    if (!isSafePath(path)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        httpd_resp_send_404(req);
        return ESP_OK;
    }

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
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    fclose(f);
    if (mtx) xSemaphoreGiveRecursive(mtx);
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_download_dir(httpd_req_t *req) {
    std::string path = getQueryParam(req, "path");
    if (!isSafePath(path)) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    auto mtx = JournalStorage::sdMutex();
    if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY);

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    // Collect all files
    std::vector<ZipEntry> entries;
    std::string dirName = path;
    auto slash = dirName.rfind('/');
    if (slash != std::string::npos) dirName = dirName.substr(slash + 1);
    collectFiles(path, dirName, entries);

    if (entries.empty()) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        sendJsonError(req, "empty directory");
        return ESP_OK;
    }

    // Set response headers
    httpd_resp_set_type(req, "application/zip");
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s.zip\"", dirName.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);

    // Write zip: local file headers + file data, then central directory, then end record
    uint8_t lfh[30 + 256];  // local file header buffer
    uint8_t buf[4096];      // file read buffer
    uint32_t offset = 0;

    // Pass 1: write local file headers + data
    for (auto &e : entries) {
        e.offset = offset;
        uint16_t nameLen = (uint16_t)e.relPath.length();

        // Local file header (30 + nameLen bytes)
        putU32(lfh + 0, 0x04034b50);   // signature
        putU16(lfh + 4, 20);           // version needed
        putU16(lfh + 6, 0);            // flags
        putU16(lfh + 8, 0);            // compression: store
        putU16(lfh + 10, 0);           // mod time
        putU16(lfh + 12, 0);           // mod date
        putU32(lfh + 14, 0);           // crc32 (placeholder, fill after reading)
        putU32(lfh + 18, 0);           // compressed size (placeholder)
        putU32(lfh + 22, 0);           // uncompressed size (placeholder)
        putU16(lfh + 26, nameLen);     // filename length
        putU16(lfh + 28, 0);           // extra field length
        memcpy(lfh + 30, e.relPath.c_str(), nameLen);

        // Read file, compute CRC32 incrementally
        std::string fullPath = path + "/" + e.relPath.substr(dirName.length() + 1);
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
        crc = crc32Finish(crc);

        // Fill in CRC and sizes
        putU32(lfh + 14, crc);
        putU32(lfh + 18, fsize);
        putU32(lfh + 22, fsize);
        e.crc32 = crc;
        e.size = fsize;

        // Send local file header
        if (httpd_resp_send_chunk(req, (const char*)lfh, 30 + nameLen) != ESP_OK) {
            if (f) fclose(f);
            if (mtx) xSemaphoreGiveRecursive(mtx);
            return ESP_OK;
        }
        offset += 30 + nameLen;

        // Send file data
        if (f) {
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                if (httpd_resp_send_chunk(req, (const char*)buf, n) != ESP_OK) {
                    fclose(f);
                    if (mtx) xSemaphoreGiveRecursive(mtx);
                    return ESP_OK;
                }
            }
            fclose(f);
        }
        offset += fsize;
    }

    // Pass 2: central directory
    uint8_t cdh[46 + 256];  // central directory header buffer
    uint32_t cdOffset = offset;

    for (auto &e : entries) {
        uint16_t nameLen = (uint16_t)e.relPath.length();

        putU32(cdh + 0, 0x02014b50);   // signature
        putU16(cdh + 4, 20);           // version made by
        putU16(cdh + 6, 20);           // version needed
        putU16(cdh + 8, 0);            // flags
        putU16(cdh + 10, 0);           // compression: store
        putU16(cdh + 12, 0);           // mod time
        putU16(cdh + 14, 0);           // mod date
        putU32(cdh + 16, e.crc32);     // crc32
        putU32(cdh + 20, e.size);      // compressed size
        putU32(cdh + 24, e.size);      // uncompressed size
        putU16(cdh + 28, nameLen);     // filename length
        putU16(cdh + 30, 0);           // extra field length
        putU16(cdh + 32, 0);           // file comment length
        putU16(cdh + 34, 0);           // disk number start
        putU16(cdh + 36, 0);           // internal file attributes
        putU32(cdh + 38, 0);           // external file attributes
        putU32(cdh + 42, e.offset);    // relative offset of local header
        memcpy(cdh + 46, e.relPath.c_str(), nameLen);

        if (httpd_resp_send_chunk(req, (const char*)cdh, 46 + nameLen) != ESP_OK) {
            if (mtx) xSemaphoreGiveRecursive(mtx);
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

    if (mtx) xSemaphoreGiveRecursive(mtx);
    ESP_LOGI(TAG, "ZIP download: %s (%d files)", path.c_str(), (int)entries.size());
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_upload(httpd_req_t *req) {
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

    // parse multipart/form-data
    size_t content_len = req->content_len;
    if (content_len == 0) {
        sendJsonError(req, "empty body");
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

    FILE *f = fopen(fullpath.c_str(), "wb");
    if (!f) {
        if (mtx) xSemaphoreGiveRecursive(mtx);
        sendJsonError(req, "cannot create file");
        return ESP_OK;
    }

    // read body in chunks, skip multipart headers, write file data
    std::vector<char> body(content_len + 1);
    size_t total_read = 0;
    while (total_read < content_len) {
        int r = httpd_req_recv(req, body.data() + total_read, content_len - total_read);
        if (r <= 0) break;
        total_read += r;
    }
    body[total_read] = '\0';

    // find start of file data (after \r\n\r\n following filename header)
    char *data_start = nullptr;
    char *search = body.data();
    while (search < body.data() + total_read) {
        char *hdr_end = strstr(search, "\r\n\r\n");
        if (!hdr_end) break;
        // check if this part has a filename
        char *part_start = search;
        if (strstr(part_start, "filename=") || strstr(part_start, "name=\"file\"")) {
            data_start = hdr_end + 4;
            break;
        }
        // skip to next boundary
        char *next = strstr(hdr_end + 4, boundary.c_str());
        if (!next) break;
        search = next + boundary.length();
        if (*search == '\r') search += 2;
    }

    if (!data_start) {
        // fallback: treat entire body as file data
        data_start = body.data();
    }

    // find end of file data (before closing boundary)
    char *data_end = body.data() + total_read;
    // search backwards for boundary marker
    std::string end_marker = "\r\n" + boundary;
    char *marker = (char*)memmem(data_start, data_end - data_start, end_marker.c_str(), end_marker.length());
    if (marker) data_end = marker;

    // also check for --boundary at very start (no preceding \r\n)
    if (!marker) {
        std::string start_marker = boundary;
        char *m2 = (char*)memmem(data_start, data_end - data_start, start_marker.c_str(), start_marker.length());
        if (m2) data_end = m2;
    }

    size_t write_len = data_end - data_start;
    // strip trailing \r\n before boundary
    if (write_len >= 2 && data_end[-2] == '\r' && data_end[-1] == '\n') {
        write_len -= 2;
    }

    fwrite(data_start, 1, write_len, f);
    fclose(f);
    if (mtx) xSemaphoreGiveRecursive(mtx);

    ESP_LOGI(TAG, "Uploaded: %s (%d bytes)", fullpath.c_str(), (int)write_len);
    sendJsonOK(req);
    return ESP_OK;
}

static esp_err_t __attribute__((unused)) handler_delete(httpd_req_t *req) {
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
