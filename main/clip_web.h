#pragma once

#include <cstdint>

// 网络剪贴板：在局域网里用浏览器查看/编辑/新增剪贴板历史条目。
// 只在剪贴板历史菜单的"网络模式"期间运行，退出即停服并释放任务/端口。
bool        clip_web_start(uint16_t port = 80);
void        clip_web_stop();
bool        clip_web_active();
uint16_t    clip_web_port();
