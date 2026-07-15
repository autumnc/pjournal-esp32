# pjournal-esp32

ESP32-S3 上的个人日记工具，配备 4.2 寸电子墨水屏，支持蓝牙键盘输入、拼音输入法、多端同步等功能。

## 硬件需求

| 组件 | 规格 |
|------|------|
| **主控** | ESP32-S3 (PSRAM 8MB+, Flash 16MB) |
| **屏幕** | RLCD 4.2" 400×300，ST7305 驱动 (SPI) |
| **RTC** | PCF85063 (I2C)，带 CR2032 电池 |
| **SD卡** | SDMMC 1-line 模式，FAT32 格式 |
| **键盘** | 蓝牙 HID 键盘 (BLE) |
| **电池** | 单节锂电池 (3.7V)，ADC 分压检测 |
| **按键** | BOOT (GPIO0) + 用户按键 (GPIO18) |

**引脚定义**: 见 `main/user_config.h`

## 功能特性

- **自由写作** / **提示写作**：新建日记条目
- **拼音输入法**：支持中文输入、词库联想
- **蓝牙键盘**：一键配对 / 自动重连（10 秒间隔）
- **过往日记浏览**：按日期排序、查看详情
- **编辑已有日记**：从浏览/详情界面按 `e` 进入编辑
- **自动保存**：3 秒无操作自动保存，或手动 `Ctrl+S`
- **发送至 Flomo**：`Ctrl+S` 发送当前正文到 Flomo
- **WebDAV 同步**：上传/下载日记到远程存储
- **AI 提示生成**：`Ctrl+P` 根据个人设置生成写作提示
- **NTP 时间同步**：支持自定义 NTP 服务器
- **固件 OTA 待烧录**：通过串口 `/dev/ttyUSB0` 烧写

## 快捷键

| 快捷键 | 功能 |
|--------|------|
| `p` | 提示写作（主界面） |
| `f` | 自由写作（主界面） |
| `v` | 查看过往日记（主界面） |
| `w` | WebDAV 同步（主界面） |
| `s` | 设置（主界面） |
| `Ctrl+Space` | 中/英输入法切换 |
| `Ctrl+S` | 保存 / 发送到 Flomo |
| `Ctrl+Q` | 退出编辑 |
| `Ctrl+P` | AI 生成写作提示 |
| `Ctrl+F` | 发送正文到 Flomo |
| `ESC` | 返回上一级面板 |
| `j` / `↓` | 向下移动 |
| `k` / `↑` | 向上移动 |
| `h` / `←` | 向左移动 |
| `l` / `→` | 向右移动 |
| `Enter` | 确认 / 进入详情 |
| `d` | 删除条目（过往日记列表） |
| `e` | 编辑当前条目 |

### 物理按键

| 按键 | 短按 | 长按 (14 ticks) |
|------|------|-----------------|
| **用户按键 (GPIO18)** | 双击 → ESC；单击 → `↑` | 进入蓝牙管理 |
| **BOOT (GPIO0)** | `↓` (蓝牙管理中) | 确认 (蓝牙管理中) |

## 编译

### 环境要求

- ESP-IDF **5.5**
- Python 3.14+
- 目标芯片: ESP32-S3

### 编译步骤

```bash
# 加载 ESP-IDF 环境
. /path/to/esp-idf/export.sh

# 编译
idf.py build

# 生成合并固件（bootloader + partition + app）
esptool.py --chip esp32s3 merge_bin -o build/merged-firmware.bin \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/pjournal.bin
```

### 烧录

```bash
# 合并固件一步烧录
esptool.py --chip esp32s3 -p /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 build/merged-firmware.bin

# 或分步烧录
idf.py -p /dev/ttyUSB0 flash
```

## SD 卡目录结构

```
/sdcard/pjournal/
├── entries/         # 日记文件 (YYYY-MM-DD_HHMMSS.txt)
├── settings.json    # 设置
└── bt_paired.dat    # 蓝牙配对信息
```

## 设置项

| 设置 | 说明 |
|------|------|
| Deepseek Key | AI 提示生成 API Key |
| Flomo 邮箱/密码 | Flomo API 登录凭证 |
| WebDAV URL/用户/密码 | 远程同步地址 |
| 个人经历/爱好 | AI 提示上下文 |
| WiFi SSID/密码 | 网络连接 |
| 时区 | 如 `CST-8` |
| NTP 服务器 | 如 `pool.ntp.org` |
| 自动保存 | 开启后 3 秒无操作自动存储 |

## 导航层级

```
主界面
├── 编辑器（自由写作 / 提示写作 / Ctrl+P）
│   └── ESC → 主界面 / 过往列表 / 详情
├── 过往日记列表
│   ├── Enter → 详情
│   │   └── e → 编辑器 → ESC → 详情
│   └── e → 编辑器 → ESC → 列表
├── 设置
├── WebDAV 同步
├── 蓝牙管理（长按用户按键）
└── Flomo 发送
```

## 版本

当前版本: v1.4.0

## License

MIT
