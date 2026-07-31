# Tab5X ECO7 程序适配测试报告

## 1. 测试结论

Tab5X（ESP32-P4 ECO7，实测芯片版本 v3.2）已完成基于
M5Tab5-UserDemo 的 ESP-IDF `release/v6.1` 适配测试。

最终固件可稳定启动，显示、PSRAM、ESP32-C6 SDIO/ESP-Hosted、Wi-Fi
AP、BMI270、INA226、CPU 温度、内部 I2C、RX8130 RTC 走时、音频驱动
数据链路和 SC202CS 摄像头采集均通过。最终串口会话连续完成 4 轮完整
smoke 测试，无失败标记、panic、异常重启或串口丢包。

本次结论为**Tab5X ECO7 基础适配及板载系统传感器、音频、摄像头驱动
闭环通过**。声学质量、摄像头成像质量、触摸坐标、外接 USB/RS485/SD 卡
和电池充放电等需要专用执行器或量测设备的项目未纳入通过范围。

## 2. 环境与版本

| 项目 | 内容 |
| --- | --- |
| 源仓库 | `https://github.com/m5stack/M5Tab5-UserDemo` |
| 适配分支 | `feature/tab5x` |
| 适配提交 | `b9d5302`、`609b9be`、`f5ad87f` |
| ESP-IDF | `release/v6.1` |
| ESP-IDF 提交 | `14f663f003eb8fd9a688c301a412a9540d29dacf` |
| 编译芯片范围 | ESP32-P4 v3.1 至 v3.99 |
| 实测芯片 | ESP32-P4 v3.2（ECO7） |
| 远端构建机 | `loki@192.168.199.128` |
| 测试中心 | `http://192.168.20.69:8000` |
| 测试位 | `80f1b2d14b33`（Tab5X） |
| 夹具 ISP 接线 | TX=G12、RX=G13、BOOT=G10、RESET/EN=G11 |
| DUT 烧录速率 | `921600` |
| DUT 串口监控速率 | `115200` |
| 测试日期 | 2026-07-31（Asia/Shanghai） |

关键组件版本：

- `espressif/esp_wifi_remote 1.6.3`
- `espressif/esp_hosted 2.12.11`
- `espressif/esp_codec_dev 1.5.5`
- `lvgl/lvgl 9.5.0`（组件锁文件；构建使用仓库内依赖）
- vendored `esp_video 0.7.0`，增加 IDF 6.x CSI 兼容修正

## 3. 适配内容

- 配置 ESP32-P4 v3.1 及以上芯片版本范围，覆盖 Tab5X ECO7。
- 适配 ESP-IDF 6.1 的 MIPI DSI、DMA2D、PPA、I2C、温度传感器和音频接口。
- 更新 ESP-Hosted/esp_wifi_remote，并配置 ESP32-C6 的 SDIO2 4-bit 总线。
- 修复 `sdmmc_card_init failed`、ESP-Hosted 重配失败和循环重启。
- 动态识别 ST7121/ST7123；ST7121 使用 RGB565，修复下半屏倾斜和灰屏。
- 启用 Tab5/Tab5X 的 SC202CS MIPI-CSI RAW8 1280x720 传感器配置。
- 适配 IDF 6.x CSI/ISP 格式处理，避免 CSI 桥接器执行不支持的
  `RAW8 -> RGB565` 转换。
- 将摄像头任务固定到 Core 0，使其与音频初始化建立的 DW-GDMA 私有中断组
  保持同核，修复 DMA 中断分配失败。
- 摄像头初始化失败改为显式结束任务，避免失败路径跳转空地址造成
  `MEPC=0` panic。
- 增加显示、音频、摄像头首帧和 smoke 完成串口标记。
- 增加 BMI270 六轴数据、INA226 电源数据、CPU 温度、内部 I2C 必需地址和
  RX8130 RTC 走时的循环 smoke 测试及明确 PASS/FAIL 标记。

## 4. 最终固件与烧录

### 4.1 DUT 测试镜像

| 项目 | 内容 |
| --- | --- |
| 构建目录 | `platforms/tab5/build.tab5x-smoke-eco7-camera` |
| 应用镜像大小 | 5,792,400 字节（`0x586290`） |
| 合并镜像大小 | 5,857,936 字节（`0x596290`） |
| 应用镜像 SHA-256 | `a9ea791c6caf175ffd64ad7b2ada4c8692c4b2eb1c5663c0335c791c078b964f` |
| 合并镜像 SHA-256 | `3631ddcdca2509932c8d41aee77c11846ed6da3c52f4a40fcc1c97be55406f37` |
| 最小应用分区余量 | 45% |

### 4.2 高速烧录载体

旧夹具固件 `0.2.0` 接受高波特率参数但实际写入速度未提高，因此本次使用
一次性夹具 OTA 包。该包固定以 `921600` 烧录 DUT，完成 MD5 校验并复位
DUT 后自动回滚夹具至 `0.2.0`。

| 项目 | 内容 |
| --- | --- |
| 夹具资产 | `fw-df11217614684ff1` |
| 一次性夹具版本 | `0.2.14-tab5x-system-smoke` |
| 夹具包 SHA-256 | `7fdf7006cee7eff0b4e85731737162ed139e8182155d473ecac167be704fcf66` |
| 夹具包内嵌 DUT SHA-256 | `3631ddcdca2509932c8d41aee77c11846ed6da3c52f4a40fcc1c97be55406f37` |
| ROM 连接预检 | `task-ffe34e62a77e430a`，通过，ESP32-P4、16 MB Flash、MAC `E8:F6:0A:E7:BD:09` |
| rollout | `rollout-3ee0e86134c2485d` |
| 观测结果 | 夹具离线完成写入和校验后，以新 boot ID `boot-80f1b2d14b33-dbadafb5` 回滚上线 |

测试中心的 rollout 可能保持 `rebooting`，因为一次性固件主动回滚到
`0.2.0`，不会以 rollout 目标版本持续注册；DUT 烧录结果以新 boot ID、
后续复位启动日志和 smoke 标记为准。

## 5. 硬件测试结果

| 测试项 | 结果 | 证据/说明 |
| --- | --- | --- |
| 编译与链接 | 通过 | ESP-IDF 6.1 完整构建及 `merge-bin` 通过 |
| 高速烧录与镜像关系 | 通过 | 资产元数据、构建 SHA 和内嵌 DUT SHA 一致；固定 `921600` |
| 冷启动/EN 复位 | 通过 | 最终会话 `ser-c6b4caf6f1b1daf1`，`115200`，无 panic、重启或丢包 |
| ESP32-P4 ECO7 识别 | 通过 | 最低 v3.1、最高 v3.99、实测 v3.2 |
| PSRAM | 通过 | 32 MB、200 MHz，内存测试通过 |
| 显示 | 通过 | ST7121、720x1280、RGB565；整屏点亮，未见下半屏灰色/倾斜分区 |
| 显示图像证据 | 通过（曝光偏高） | 帧 `cap-497ddb83c4864291`，SHA `f2f06344dc2e388e9a11575682d05b7d8c6f2827fbb7f455f7505a19eaad9b92` |
| 触摸控制器初始化 | 通过 | ST7121 触摸固件版本 1，最多 10 点 |
| 触摸坐标功能 | 未执行 | 夹具无触摸执行器 |
| 内部 I2C | 通过 | 4 轮均检测到必需地址 `0x32` RTC、`0x36` SC202CS、`0x41` INA226、`0x68` BMI270 |
| ESP32-C6 SDIO | 通过 | 4-bit/40 MHz，Card init success，Transport active |
| Wi-Fi AP 协议栈 | 通过 | `M5Tab5-UserDemo-WiFi` 和 DHCP 服务启动 |
| Wi-Fi RF/HTTP | 未执行 | 未做终端关联、吞吐和 HTTP 访问 |
| 音频驱动数据闭环 | 通过 | 连续 4 轮；每轮读取 1,152,000 字节、写入 576,000 字节 |
| 音频声学质量 | 未执行 | 未用外部麦克风/声级计评估响度、失真、声道和信噪比 |
| SC202CS 摄像头 | 通过 | PID `0xeb52`，RAW8 1280x720，首帧 1,843,200 字节 |
| 摄像头持续采集 | 通过 | 连续 4 轮分别采集 137、136、137、138 帧，均出现 PASS |
| 摄像头成像质量 | 未执行 | smoke 仅验证帧采集和 PPA 数据处理 |
| BMI270 IMU | 通过 | 连续 4 轮加速度模长 `0.979` 至 `0.983`，三轴陀螺仪数值均有效 |
| INA226 | 通过 | 连续 4 轮电压、分流电压、功率和电流均可读取；总线电压 `4.4325` 至 `8.4050 V` |
| CPU 温度 | 通过 | 连续 4 轮为 `39` 至 `40` 摄氏度 |
| RX8130 RTC 走时 | 通过 | 连续 4 轮间隔 2.1 秒读取，时间增量均为 2 秒；时间内容见已知问题 |
| USB Host/HID | 初始化通过 | USB Host 安装，HID 等待设备 |
| USB HID 枚举 | 未执行 | 未连接可控 HID 外设 |
| 接口检测状态 | 记录 | 4 轮均为 USB-C=1、USB-A HID=0、耳机=0；未作为外设功能通过依据 |
| RS485 | 初始化通过 | UART 引脚、模式和驱动安装成功 |
| RS485 回环 | 未执行 | 无回环夹具或对端 |
| SD 卡文件系统 | 未执行 | 无可控 SD 卡介质 |
| GPIO 电气测试 | 未执行 | 无 GPIO 回环/量测夹具 |
| 电池、充电和掉电恢复 | 未执行 | 未做充放电曲线和断电保持测试 |

最终串口会话统计：

- `TAB5X_DISPLAY_READY`：1 次。
- `TAB5X_IMU_DRIVER_TEST_PASS`：4 次。
- `TAB5X_POWER_MONITOR_TEST_PASS`：4 次。
- `TAB5X_CPU_TEMP_TEST_PASS`：4 次。
- `TAB5X_I2C_REQUIRED_DEVICES_TEST_PASS`：4 次。
- `TAB5X_RTC_TEST_PASS`：4 次，增量均为 2 秒。
- `TAB5X_AUDIO_DRIVER_TEST_PASS`：4 次。
- `TAB5X_CAMERA_DRIVER_FRAME_OK`：4 次。
- `TAB5X_CAMERA_DRIVER_TEST_PASS`：4 次。
- `TAB5X_DRIVER_SMOKE_DONE`：4 次。
- `TAB5X_*FAIL`、Guru Meditation、panic、Rebooting：0 次。
- 验证窗口内串口丢包 0。

## 6. 已知问题与风险

1. 启动日志出现 `ledc: GPIO 22 is not usable, maybe conflict with others`；当前背光和显示正常，仍应确认 GPIO22 资源归属。
2. 启动动画音频出现一次 `Mode 1 conflict sample_rate 44100 with 48000`；之后 48 kHz 录放数据闭环连续通过。
3. RTC 内容为历史测试时间，量产初始化时应校验有效日期和时间。
4. `esp_codec_dev 1.5.5` 在组件仓库曾被标记为 yanked；当前工程可构建并连续完成录放数据测试，后续仍建议评估升级。
5. fixture USB 摄像头画面曝光偏高，只能确认整屏区域无明显灰屏分区，不能代替显示色彩和坏点量测。
6. 一次性高速夹具包属于测试辅助产物，不应作为夹具常驻生产固件。

## 7. 验收建议

当前版本可作为 Tab5X ECO7 的程序适配版本提交。量产或发布前建议在人工工位
补齐触摸坐标、音频声学、摄像头成像质量、SD 卡、USB HID、RS485 回环、
GPIO、电池/充电以及 Wi-Fi RF/HTTP 测试，并处理或接受第 6 节风险。
