#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>

// 剪贴板历史：最多 10 条，单条 32 KB，内容与描述符都放 PSRAM。
// 新条目在 0 号；剪贴板写入由 clipboardSet() 统一调用 cliphist_record()。
#define CLIPHIST_MAX_ENTRIES 10
#define CLIPHIST_MAX_BYTES   (32u * 1024u)

void        cliphist_record(const std::string &text, bool linewise);
int         cliphist_count();
const char *cliphist_text(int i);   // 越界返回 nullptr
uint32_t    cliphist_len(int i);    // 越界返回 0
bool        cliphist_linewise(int i);
void        cliphist_remove(int i); // 越界 no-op

// 网络剪贴板用：直接以原始缓冲区增/改条目，避免 std::string 中转拷贝。
// 返回 false 表示内容为空/超长截断为空/内存不足。
bool        cliphist_add_raw(const char *data, size_t len, bool linewise);
bool        cliphist_replace_raw(int i, const char *data, size_t len);
