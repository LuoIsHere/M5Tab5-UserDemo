# Tab5X ECO7 程序适配测试报告

## 1. 测试结论

Tab5X（ESP32-P4 ECO7，实测芯片版本 v3.2）已完成基于 M5Tab5-UserDemo 的 `release/v6.1` 适配。工程可完整编译，已烧录版本能够稳定启动，显示异常已修复，ESP32-C6 SDIO/ESP-Hosted/Wi-Fi AP 链路正常。

本次结论为**基础适配通过，受夹具限制的外设功能闭环待人工补测**。显示、启动、PSRAM、内部 I2C、Wi-Fi 协处理器链路已有硬件证据；音频、IMU、INA226、RTC、USB Host、RS485 和摄像头时钟仅确认驱动/接口初始化，未将缺少执行器或外设的项目标记为功能通过。

## 2. 环境与版本

| 项目 | 内容 |
| --- | --- |
| 源仓库 | `https://github.com/m5stack/M5Tab5-UserDemo` |
| 适配分支 | `feature/tab5x` |
| 基线提交 | `68b19d3` |
| ESP-IDF | `release/v6.1` |
| ESP-IDF 提交 | `14f663f003eb8fd9a688c301a412a9540d29dacf` |
| 目标芯片 | ESP32-P4，最低版本 v3.1，最高版本 v3.99 |
| 实测芯片 | ESP32-P4 v3.2（ECO7） |
| 远端构建机 | `loki@192.168.199.128` |
| 测试中心 | `http://192.168.20.69:8000` |
| 测试位 | `80f1b2d14b33`（Tab5X） |
| 测试日期 | 2026-07-30（Asia/Shanghai） |

关键组件版本：

- `espressif/esp_wifi_remote 1.6.3`
- `espressif/esp_hosted 2.12.11`
- `espressif/esp_codec_dev 1.5.5`
- `espressif/led_strip 3.0.3`
- `lvgl/lvgl 9.5.0`（组件锁文件）；构建实际选用仓库 `dependencies/lvgl`，日志版本为 9.2.2

## 3. 适配内容

- 增加 ESP32-P4 v3.1 及以上芯片版本约束，覆盖 Tab5X ECO7。
- 适配 ESP-IDF 6.1 的 MIPI DSI、DMA2D、PPA、摄像头、I2C、温度传感器和音频接口变化。
- 更新 ESP-Hosted/esp_wifi_remote，并配置 Tab5 ESP32-C6 的 SDIO2 引脚、4-bit 总线和高有效复位极性。
- 修复 Wi-Fi 初始化阶段 `sdmmc_card_init failed`、ESP-Hosted 重配失败和循环重启问题。
- 动态识别 ST7121/ST7123；ST7121 使用 RGB565 输出，ST7123 保持 RGB888 输出，修复 Tab5X 下半屏倾斜及灰屏。
- 增加 `TAB5X_DISPLAY_READY` 启动标记，便于串口自动验证。
- 更新 IDF 6.1 所需组件依赖和本地组件清单。

显示修复的关键配置：

```c
.out_color_format = is_st7121 ? LCD_COLOR_FMT_RGB565 : LCD_COLOR_FMT_RGB888,
```

## 4. 构建与固件

### 4.1 已完成硬件验证的固件

| 项目 | 内容 |
| --- | --- |
| 测试中心固件 ID | `fw-2523726e012c4927` |
| 合并镜像大小 | 5,845,776 字节 |
| SHA-256 | `be03b9a599379a07d35da13eb44ac17196c71ec2efd7318af67888351400d635` |
| 显示证据帧 | `cap-8cbd04c843934df7` |

### 4.2 最终提交源码构建校验

最终整理后的源码在 ESP-IDF `release/v6.1` 上重新执行完整构建和 `merge-bin`，构建通过。根据用户“取消重烧”的要求，该镜像未再次烧录。

| 项目 | 内容 |
| --- | --- |
| 应用镜像 | `/dev/shm/tab5x-build-final-20260730/m5stack_tab5.bin` |
| 应用镜像大小 | 5,780,240 字节（`0x583310`） |
| 应用镜像 SHA-256 | `03276ae29657ecd4e70ce04f77b3980c55ab6a5e0de1a6c28b62d310f258468a` |
| 合并镜像 | `/dev/shm/tab5x-final-merged-20260730.bin` |
| 合并镜像大小 | 5,845,776 字节 |
| 合并镜像 SHA-256 | `0f2f9abb68841c6fb54d949f73957c0d1ea3eba02626ebdc5064a4f0ddcc39ee` |
| 分区余量 | 最小应用分区剩余 45% |

## 5. 硬件测试结果

| 测试项 | 结果 | 证据/说明 |
| --- | --- | --- |
| 编译与链接 | 通过 | ESP-IDF 6.1 完整构建 1940 个目标，镜像尺寸检查通过 |
| 烧录与镜像完整性 | 通过 | `fw-2523726e012c4927` 已烧录并运行；后续按要求取消重烧 |
| 冷启动/复位稳定性 | 通过 | 三次独立启动窗口均无 panic、abort 或重复启动；命令 `cmd-2d040c49b1b4447c`、`cmd-aa4c172a232c4db3`、`cmd-6458f5826cdb4d4b` |
| ESP32-P4 ECO7 识别 | 通过 | 启动日志：最低 v3.1、最高 v3.99、实测 v3.2 |
| PSRAM | 通过 | 识别 32 MB、200 MHz，`SPI SRAM memory test OK` |
| 显示 | 通过 | ST7121 检测成功，完整 UI 显示至屏幕底部；原下半屏倾斜/灰屏消失，用户已确认 |
| 复位后显示保持 | 通过 | 最终证据帧 `cap-66525fbdc3b5456d`，SHA-256 `95a2cdb022de8b8c29fa036bb95808c3ec02d4a6d8a33d54271249d5a3f4cf3c` |
| 触摸控制器初始化 | 通过 | ST7121 触摸固件版本 1，分辨率 720x1280，最多 10 点，LVGL 输入设备创建成功 |
| 触摸坐标功能 | 未执行 | 夹具无触摸执行器，未验证坐标、边缘和多点触控 |
| 内部 I2C | 通过 | 扫描到 `0x00, 0x10, 0x28, 0x32, 0x36, 0x40, 0x41, 0x43, 0x44, 0x55, 0x68` |
| ESP32-C6 SDIO | 通过 | 4-bit/40 MHz，`Card init success`，识别 `esp32c6`，Transport active |
| Wi-Fi AP 协议栈 | 通过 | `M5Tab5-UserDemo-WiFi` 启动，DHCP 地址 `192.168.4.1` |
| Wi-Fi RF/HTTP | 未执行 | 测试端 WLAN 软件关闭，未完成终端关联、吞吐和 HTTP 访问 |
| ES7210/ES8388 音频接口 | 初始化通过 | ES7210 TDM、ES8388 codec 均打开成功 |
| 麦克风/扬声器/耳机 | 未执行 | 无声学采集、播放和耳机插拔执行器，未做信噪比、声道和插拔闭环 |
| BMI270 IMU | 初始化通过 | ODR/量程设置成功，传感器启用 |
| INA226 | 初始化通过 | 总线电压读数约 1.92-1.94 V |
| RX8130 RTC | 初始化通过 | 芯片可访问并读取；日期内容异常，见已知问题 |
| USB Host/HID | 初始化通过 | USB Host 安装成功，HID 任务进入等待设备状态 |
| USB HID 枚举 | 未执行 | 夹具未连接键盘/鼠标等 HID 外设 |
| RS485 | 初始化通过 | UART 引脚、模式和驱动安装成功 |
| RS485 回环 | 未执行 | 无回环夹具/对端设备 |
| 摄像头时钟/接口 | 初始化通过 | 摄像头 24 MHz 时钟初始化；未采集 DUT 摄像头画面 |
| SD 卡文件系统 | 未执行 | 测试位未提供可控 SD 卡插拔和读写介质 |
| GPIO 电气测试 | 未执行 | 无 GPIO 回环/量测夹具 |
| 电池、充电和断电恢复 | 未执行 | 未做电池继电器闭环、充电曲线和掉电保持测试 |

新增复位串口会话：

- `ser-2a639c72db7e9a10`：首次独立复位会话，持续观察约 50 秒，无 panic 或重复启动。
- `ser-1fb81aff980fb8af`：13,243 字节、24 个数据块、0 丢包。
- `ser-db3500f9cd142d62`：12,479 字节、20 个数据块、0 丢包。

两次会话均出现 `TAB5X_DISPLAY_READY`、ESP32-C6 SDIO 建链和 Wi-Fi AP 启动标记，未出现 panic、abort、`sdmmc_card_init failed` 或循环重启。

## 6. 已知问题与风险

1. 启动日志出现 `ledc: GPIO 22 is not usable, maybe conflict with others`；当前背光和显示正常，但应继续确认 GPIO22 资源归属。
2. 音频启动时出现 `Mode 1 conflict sample_rate 44100 with 48000`；codec 随后重新打开且应用继续运行，仍需做真实音频闭环。
3. RTC 读到 `2010-10-00`，日期日字段无效；需要在生产初始化或用户固件中校验 RTC 内容。
4. `esp_codec_dev 1.5.5` 已被 Espressif 组件仓库标记为 yanked（原因：build failure），本项目当前可在 IDF 6.1 构建并初始化，但建议后续评估可替代版本。
5. `sdkconfig.defaults` 中若干旧 LVGL/Brookesia Kconfig 符号在 IDF 6.1 下不再识别；不影响本次构建，但建议后续清理配置。
6. 测试中心固件 0.2.0 未采用任务请求中的高烧录波特率参数，5.8 MB 镜像烧录约 584 秒。后续如需再次烧录，应使用 ACM2 和 921600 波特率，或先升级夹具固件；本轮未再烧录。

## 7. 验收建议

当前版本可作为 Tab5X ECO7 基础适配版本提交。量产或发布前仍应在人工工位补齐触摸坐标、音频声学、耳机、DUT 摄像头、SD 卡、USB HID、RS485 回环、GPIO、电池/充电以及 Wi-Fi RF/HTTP 测试，并处理或接受第 6 节所列风险。
