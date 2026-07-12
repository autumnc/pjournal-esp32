# pjournal-esp32 编译说明

## 编译环境
- ESP-IDF 5.5
- Python 3.14.3
- 目标芯片: ESP32-S3

## 编译步骤

1. 加载 ESP-IDF 环境:
```bash
source /home/ywz/esp/esp-idf/export.sh
```

2. 编译项目:
```bash
idf.py build
```

3. 生成合并固件 (bootloader + partition_table + app):
```bash
esptool.py --chip esp32s3 merge_bin -o pjournal-merged.bin \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/pjournal.bin
```

## 固件信息

- **文件名**: pjournal-merged.bin
- **大小**: 7.4 MB
- **分区布局**:
  - Bootloader: 0x0 (0x57e0 bytes)
  - Partition Table: 0x8000
  - Application: 0x10000 (0x7533a0 bytes, 39% 空间剩余)

## 烧录方法

使用 esptool 直接烧录合并固件:
```bash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 pjournal-merged.bin
```

或者使用 idf.py 烧录分离文件:
```bash
idf.py -p /dev/ttyUSB0 flash
```

## 版本历史

### v1.0 (2025-07-12)
- 修复 WiFi 事件循环重复创建问题
- 修复 Flomo 客户端缓冲区截断风险
- 改进 WebDAV HTTP 错误处理
- 增加 WebDAV 同步时间阈值到 60 秒
- 优化 IME 用户字典写入逻辑
- 修复编辑器光标移动逻辑
- 调整电池电量显示位置
- **重要**: 修复蓝牙反复断开重连问题
  - 新增连接中状态管理
  - 防止重复连接请求
  - 增加重连间隔到 10 秒
  - 优化连接状态检查逻辑
- **重要**: 添加启动失败提示
  - SD 卡初始化失败时显示错误信息
  - WiFi 连接失败时显示提示（不阻塞启动）
  - 改善启动过程可视化
- **重要**: WiFi 改为按需连接
  - 不在启动时自动连接 WiFi
  - 仅在需要时连接（WebDAV 同步、Flomo 发送、Deepseek 提示生成）
  - 提升启动速度，减少功耗