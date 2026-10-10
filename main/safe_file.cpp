#include "safe_file.h"
#include "io_probe.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>

static const char *TAG = "SafeFile";

bool fileExists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// 判断 p 是不是一个已存在的目录。
//
// 关键点: 挂载点(/sdcard)上 stat 和 opendir 都是正常的, 唯独 mkdir 会失败 ——
// 实测 "DIRPROBE /sdcard: stat=0(0) opendir=ok(0) mkdir=-1(22)"。FatFs 那边
// f_mkdir("0:/") 走到 follow_path 的 NS_NONAME 分支(认出"这是根目录自己"),
// 返回 FR_INVALID_NAME → EINVAL, 不是 EEXIST。
//
// 所以要判断"目录已存在"必须用 stat/opendir, 不能用 mkdir 的 errno。
// 这里用 stat: 它比 opendir 少分配一个 DIR 对象(内部 RAM 紧张时这点有意义),
// 而且语义上正好是 fileExists() 的目录版。
static bool dirExists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensureDirPath(const std::string &path) {
    if (path.empty()) return false;
    std::string cur;
    size_t start = (path[0] == '/') ? 1 : 0;
    if (start == 1) cur = "/";
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += "/";
            cur += part;
            // 已经存在的目录一律跳过, 绝不 mkdir。
            //
            // 这里曾经是无条件 mkdir + "errno != EEXIST 就算失败"。第一层分量
            // 恰好就是挂载点本身(/sdcard), 而 mkdir(<挂载点>) 拿到的是 EINVAL
            // 不是 EEXIST —— 于是对**任何** "/sdcard/..." 路径, ensureDirPath 都
            // 在第一个分量上返回 false, safeWriteFile 全线失败: 设置写不进去
            // (set() 见写失败直接 return, 连内存缓存都不更新, 界面上就是"翻不动"),
            // journal 存不下来、恢复草稿和历史版本全丢。日志里那句
            // "mkdir failed: /sdcard errno=22" 就是它, 和内存无关。
            if (!dirExists(cur)) {
                if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) {
                    ESP_LOGE(TAG, "mkdir failed: %s errno=%d", cur.c_str(), errno);
                    return false;
                }
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

std::string readWholeFile(const std::string &path) {
    repairSafeWriteFile(path);
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return "";
    std::string result;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        result.append(buf, n);
    }
    fclose(f);
    return result;
}

static bool flushAndClose(FILE *f) {
    bool ok = fflush(f) == 0;
    int fd = fileno(f);
    if (fd >= 0 && fsync(fd) != 0) {
        ESP_LOGW(TAG, "fsync failed errno=%d; continuing after fflush", errno);
    }
    if (fclose(f) != 0) ok = false;
    return ok;
}

void repairSafeWriteFile(const std::string &path) {
    std::string tmp = path + ".tmp";
    std::string bak = path + ".bak";
    bool hasFinal = fileExists(path);
    bool hasBak = fileExists(bak);
    if (!hasFinal && hasBak) {
        rename(bak.c_str(), path.c_str());
    }
    if (fileExists(tmp)) remove(tmp.c_str());
}

bool safeWriteFile(const std::string &path, const std::string &content) {
    // 合计探针: 含 fopen / 数据写 / fsync / 两次 rename / 两次 remove。
    // 和下面"数据+fsync"那条一比, 差值就是 FATFS 元数据操作(rename/remove)的开销。
    char probeDetail[144];
    snprintf(probeDetail, sizeof(probeDetail), "%s  %u B", path.c_str(),
             (unsigned)content.size());
    IO_PROBE("safeWriteFile 合计", probeDetail);

    size_t slash = path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        if (!ensureDirPath(path.substr(0, slash))) return false;
    }

    std::string tmp = path + ".tmp";
    std::string bak = path + ".bak";
    repairSafeWriteFile(path);
    remove(tmp.c_str());

    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "open tmp failed: %s", tmp.c_str());
        return false;
    }
    bool ok;
    {
        // 只量"写数据 + fflush + fsync + fclose"这一段
        IO_PROBE("safeWrite:数据+fsync", probeDetail);
        size_t written = fwrite(content.data(), 1, content.size(), f);
        ok = (written == content.size()) && flushAndClose(f);
    }
    if (!ok) {
        ESP_LOGE(TAG, "write tmp failed: %s", tmp.c_str());
        remove(tmp.c_str());
        return false;
    }

    remove(bak.c_str());
    bool hadOriginal = fileExists(path);
    if (hadOriginal && rename(path.c_str(), bak.c_str()) != 0) {
        ESP_LOGE(TAG, "backup failed: %s", path.c_str());
        remove(tmp.c_str());
        return false;
    }

    if (rename(tmp.c_str(), path.c_str()) != 0) {
        ESP_LOGE(TAG, "commit failed: %s", path.c_str());
        if (hadOriginal) rename(bak.c_str(), path.c_str());
        remove(tmp.c_str());
        return false;
    }

    if (hadOriginal) remove(bak.c_str());
    return true;
}
