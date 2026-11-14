#pragma once

#include "pjournal_app.h"

void screen_gtd_init();
AppState screen_gtd_handle(int key, ScreenContext &ctx);
// 返回是否处于 GTD 列表浏览模式(该模式下物理按键导航快捷键生效)
bool screen_gtd_accept_physical_buttons();
// 是否处于 GTD 项目标签的项目列表视图(项目树顶层)
bool screen_gtd_in_project_list();
// 物理按键双击 BOOT: 项目树内返回项目选择菜单; 其他视图无动作
void screen_gtd_physical_double_boot();
// 剪贴板历史菜单粘贴入口（标题/字段/筛选/备注）
bool screen_gtd_paste_text(const std::string &text);
