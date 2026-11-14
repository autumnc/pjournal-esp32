#pragma once

#include <string>
#include "pjournal_app.h"

// Win+V 剪贴板历史浮层。主循环接管：菜单打开时吞掉所有按键，
// 关闭后由底层界面的下一个空闲节拍自行重绘。
bool clip_menu_active();
bool clip_menu_open(AppState underlyingState);
void clip_menu_handle(int key);
void clip_menu_close(bool restore);

// 把 text 插到当前界面激活的文本框；没有输入焦点时返回 false
// （调用方已把 text 设为当前剪贴板）。
bool clip_paste_into_active_input(AppState state, const std::string &text);
