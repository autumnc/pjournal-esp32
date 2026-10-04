#pragma once

#include <string>
#include <vector>

extern bool g_fileEdit;

struct FileEditEntry {
    std::string name;
    std::string path;
    bool isDir = false;
};

bool fileEditInit();
std::string fileEditCurrentPath();
void fileEditSetCurrentPath(const std::string &path);
std::string fileEditLoad();
bool fileEditSave(const std::string &text);
std::vector<FileEditEntry> fileEditListDir(const std::string &dir);
std::string fileEditCurrentDir();
bool fileEditCreateFile(const std::string &dir, const std::string &name, std::string &outPath);
bool fileEditRenamePath(const std::string &path, const std::string &newName, std::string &outPath);
bool fileEditDeletePath(const std::string &path);
std::string fileEditUniqueName(const std::string &dir, const std::string &baseName);
std::string fileEditBaseName(const std::string &path);
bool fileEditIsSafeName(const std::string &name);
