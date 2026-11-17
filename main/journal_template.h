#pragma once

#include <ctime>
#include <string>
#include <vector>

// 日记模板：SD 卡根目录 /sdcard/Templates 下的 Markdown 文件（*.md）。
// 模板块里的 {{date}} / {{weekday}} 在套用时按当前时间展开；
// {{weather}} 暂不处理，原样保留。
extern const char *kJOURNAL_TEMPLATE_DIR;

// 模板文件名列表（不含路径，按文件名排序）。
std::vector<std::string> journalTemplateList();

// "晨间日记.md" -> "晨间日记"；没有扩展名时原样返回。
std::string journalTemplateDisplayName(const std::string &filename);

// 设置里保存的当前模板文件名（可能为空或已失效）。
std::string journalTemplateSelected();
void journalTemplateSelect(const std::string &filename);

// 模板选择「无」时关闭模板日记功能：主界面入口、模板预览、天气地区设置都会隐藏。
bool journalTemplateEnabled();
void journalTemplateSelectNone();

// 展开 {{date}} / {{weekday}}（兼容 {{ date }} / {{ weekday }} 写法）。
std::string journalTemplateExpand(const std::string &text, time_t now);

// 按文件名读取模板并展开占位符。文件名必须是 Templates 目录下的裸文件名。
bool journalTemplateLoad(const std::string &filename, std::string &out);

// 读取当前选中的模板；未选择时取列表中的第一个。失败时 error 为提示文本。
bool journalTemplateLoadSelected(std::string &out, std::string &error);

// 首次启动（SD 卡上没有 Templates 目录）时创建该目录，并写入固件内置的
// 默认模板（来自仓库根目录 Templates/）。
void journalTemplateEnsureDefaults();
