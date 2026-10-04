#include "file_edit.h"
#include "settings_manager.h"
#include "safe_file.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <esp_log.h>

static const char *TAG = "FileEdit";
static const char *FILE_EDIT_PATH_KEY = "file_edit_path";
static const char *FILE_EDIT_DEFAULT_PATH = "/sdcard/file_edit.txt";

bool g_fileEdit = false;

static bool startsWithSdcard(const std::string &path) {
    return path == "/sdcard" || path.find("/sdcard/") == 0;
}

std::string fileEditBaseName(const std::string &path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string parentDir(const std::string &path) {
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return "/sdcard";
    std::string dir = path.substr(0, slash);
    if (!startsWithSdcard(dir)) return "/sdcard";
    return dir.empty() ? "/sdcard" : dir;
}

bool fileEditIsSafeName(const std::string &name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return false;
    return true;
}

bool fileEditInit() {
    std::string path = fileEditCurrentPath();
    if (path.empty() || !startsWithSdcard(path)) {
        path = FILE_EDIT_DEFAULT_PATH;
        fileEditSetCurrentPath(path);
    }
    if (!fileExists(path)) {
        if (!safeWriteFile(path, "")) {
            ESP_LOGE(TAG, "Cannot create %s", path.c_str());
            return false;
        }
    }
    ESP_LOGI(TAG, "File edit current=%s", path.c_str());
    return true;
}

std::string fileEditCurrentPath() {
    std::string path = g_settings.getString(FILE_EDIT_PATH_KEY, FILE_EDIT_DEFAULT_PATH);
    if (path.empty() || !startsWithSdcard(path)) path = FILE_EDIT_DEFAULT_PATH;
    return path;
}

void fileEditSetCurrentPath(const std::string &path) {
    if (!startsWithSdcard(path)) return;
    g_settings.setString(FILE_EDIT_PATH_KEY, path);
}

std::string fileEditLoad() {
    return readWholeFile(fileEditCurrentPath());
}

bool fileEditSave(const std::string &text) {
    std::string path = fileEditCurrentPath();
    if (!safeWriteFile(path, text)) {
        ESP_LOGE(TAG, "Cannot write %s", path.c_str());
        return false;
    }
    return true;
}

std::string fileEditCurrentDir() {
    return parentDir(fileEditCurrentPath());
}

std::vector<FileEditEntry> fileEditListDir(const std::string &dir) {
    std::vector<FileEditEntry> out;
    std::string safeDir = startsWithSdcard(dir) ? dir : "/sdcard";
    if (safeDir != "/sdcard") out.push_back({"..", parentDir(safeDir), true});
    DIR *d = opendir(safeDir.c_str());
    if (!d) return out;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name.empty() || name == "." || name == "..") continue;
        if (name[0] == '.') continue;
        std::string path = safeDir;
        if (path.size() > 1 && path.back() != '/') path += "/";
        path += name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        bool isDir = S_ISDIR(st.st_mode);
        bool isReg = S_ISREG(st.st_mode);
        if (!isDir && !isReg) continue;
        if (!isDir) {
            if (name.size() >= 4 && name.substr(name.size() - 4) == ".tmp") continue;
            if (name.size() >= 4 && name.substr(name.size() - 4) == ".bak") continue;
        }
        out.push_back({name, path, isDir});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const FileEditEntry &a, const FileEditEntry &b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return a.name < b.name;
    });
    return out;
}

std::string fileEditUniqueName(const std::string &dir, const std::string &baseName) {
    std::string base = baseName.empty() ? "untitled.txt" : baseName;
    std::string stem = base;
    std::string ext;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        stem = base.substr(0, dot);
        ext = base.substr(dot);
    }
    for (int i = 0; i < 1000; i++) {
        std::string name = (i == 0) ? base : (stem + "-" + std::to_string(i) + ext);
        std::string path = dir + "/" + name;
        if (!fileExists(path)) return name;
    }
    return stem + "-new" + ext;
}

bool fileEditCreateFile(const std::string &dir, const std::string &name, std::string &outPath) {
    if (!fileEditIsSafeName(name)) return false;
    std::string safeDir = startsWithSdcard(dir) ? dir : "/sdcard";
    outPath = safeDir + "/" + name;
    if (fileExists(outPath)) return false;
    return safeWriteFile(outPath, "");
}

bool fileEditRenamePath(const std::string &path, const std::string &newName, std::string &outPath) {
    if (!startsWithSdcard(path) || !fileEditIsSafeName(newName)) return false;
    std::string dir = parentDir(path);
    outPath = dir + "/" + newName;
    if (outPath == path) return true;
    if (fileExists(outPath)) return false;
    if (rename(path.c_str(), outPath.c_str()) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed errno=%d", path.c_str(), outPath.c_str(), errno);
        return false;
    }
    if (fileEditCurrentPath() == path) fileEditSetCurrentPath(outPath);
    return true;
}

bool fileEditDeletePath(const std::string &path) {
    if (!startsWithSdcard(path) || path == "/sdcard") return false;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) return rmdir(path.c_str()) == 0;
    return remove(path.c_str()) == 0;
}
