#pragma once

#include <cstdint>

// U盘模式:把整张 SD 卡经 TinyUSB MSC 暴露给电脑。模式期间设备侧不挂 FATFS、
// 也不访问 SD —— 卡完全归电脑所有。
//
// 进入走:排空 IME 写盘 → 停文件管理服务 → 卸载应用 FATFS → 裸卡重新初始化 →
// 建 MSC 存储 → 装 TinyUSB。退出完全逆序, 最后把 FSLS PHY 还给 USB-Serial-JTAG
// 并重挂应用 FATFS。细节与顺序照 pico 固件的 main/usb_msc.cpp。

typedef enum {
    USB_MSC_OK = 0,
    USB_MSC_ERR_ALREADY_ACTIVE,
    USB_MSC_ERR_NOT_MOUNTED,     // 进模式前应用 FATFS 就没挂上
    USB_MSC_ERR_UNMOUNT,         // 卸载应用 FATFS 失败
    USB_MSC_ERR_NO_CARD,         // 裸卡初始化失败(无卡/卡坏)
    USB_MSC_ERR_STORAGE,         // 创建 MSC 存储失败
    USB_MSC_ERR_USB,             // TinyUSB 设备栈安装失败
    USB_MSC_ERR_HOST_HOLD,       // 退出被拒:电脑仍占用介质
    USB_MSC_ERR_TEARDOWN,        // 拆除失败 / 词库写盘未排空
    USB_MSC_ERR_REMOUNT,         // 退出后应用 FATFS 重挂失败
} usb_msc_result_t;

// 进入/退出。失败时 usbMscEnter 已尽力回滚并重挂应用 FATFS;
// usbMscExit 在 REMOUNT 失败后可以再次调用, 只重试重挂。
usb_msc_result_t usbMscEnter();
usb_msc_result_t usbMscExit();

// 裸卡是否被 MSC 占用。safe_file / settings / BT 在访问 SD 前查这个。
bool usbMscActive();

// 电脑是否已挂载 U 盘; 电脑是否仍占用介质(未安全弹出)。
bool usbMscHostMounted();
bool usbMscHostHold();

// 卡容量(字节), 模式未激活时返回 0。
uint64_t usbMscCardCapacityBytes();

// 给界面用的中文错误文案(不返回 nullptr)。
const char *usbMscResultText(usb_msc_result_t r);
