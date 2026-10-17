#pragma once

#include "pjournal_app.h"

// U盘模式屏幕。进入/退出在这里同步执行(前置页面已画好), 其余时间只轮询
// 电脑的挂载/占用状态。
AppState screen_usb_msc_init(ScreenContext &ctx);
AppState screen_usb_msc_handle(int key, ScreenContext &ctx);
