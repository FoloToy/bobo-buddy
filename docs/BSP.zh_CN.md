[English](BSP.md)

# Bobo M501 V1.5 / 博博

ESP32-S3、16MB Flash、8MB 八线 PSRAM；此工程为 M501 V1.5，不是 C5 M301。PMU 独占 I2C1，其余设备复用 I2C0。实际 ESP DTE 的 TX=GPIO5、RX=GPIO4，与 PCB 模块标签交叉；控制台使用 USB。
独立 Demo 将 NFC 数据复制到队列，在工作任务播放短音，并打印触摸事件；不会启动 PPP 或云服务。
UID/NDEF 写入有界字段，超长记录明确标记；回调数据临时有效，返回前必须复制。每次放卡触发一次，连续三次无卡轮询后同卡重新触发。UID 不能作为认证凭据。
NFC/触摸 stop 完成后才能重启；超时代表旧任务可能还在运行，应等待/重试 stop，不能创建第二个所有者；不能在其自身回调内调用 stop。触觉 API 可按时长阻塞，最多 1 秒。
4G 由应用显式传入 APN/SIM 卡槽，串行调用 start/wait/stop；原始 UART 与 PPP 不能同时持有；重连策略由应用负责。

| API | Hardware / ownership |
| --- | --- |
| bsp_i2c | I2C0 SDA41/SCL42; I2C1 SDA7/SCL16 dedicated to PMU |
| bsp_power | AXP2101 0x34 on I2C1, IRQ2 |
| bsp_audio | ES8311 0x18, I2S0 MCLK11/BCLK10/WS46/TX3/RX9; no PA GPIO |
| bsp_button | Active-low voice key GPIO21 |
| bsp_nfc | WS1850T 0x28, RESET18/IRQ17; UID and first NDEF record |
| bsp_touch | Front12/back13 active-low digital touch edges |
| bsp_led | WS2812 data6 |
| bsp_haptic | AW862XX 0x58, RESET47/IRQ48 |
| bsp_wakeup | GX8002 0x2F, IRQ14 |
| bsp_modem / bsp_4g | UART1 TX5/RX4, raw transport or opt-in PPP |

## 初始化、所有权与失败

先初始化 I2C，再初始化 PMU；GX8002 先关闭核心/IO，设定 IO 3.3V、核心 1.0V，再按 IO → 10ms → 核心上电，关闭反序。音频先 init 再 open，再 read/write；音量为 0–100%，close 在电源休眠前完成。

共享总线只能由 BSP 初始化一次；生命周期调用由一个应用任务串行拥有。回调不能执行网络/音频/磁盘，按需复制到应用队列。音频读写可能阻塞，初始化失败返回错误，不能把缺失传感器当作成功。根工程不启动 Wi-Fi、蓝牙、云服务或产品状态机。

## 验证与真机验收

`./tools/build.sh` 构建默认固件及 Demo。编译与主机测试不能证明真机通过。真机需核对实体板号/芯片、Flash/PSRAM、引脚、总线地址、供电、音频非零录音与可听回放、按键/触摸/NFC（适用时）、LED 与 RTC/唤醒。电机、触觉、关机、休眠、4G 与唤醒固件升级只能显式测试，不能默认触发。

交付分别记录 `Build`、`Host tests`、`Device tests`、`Unverified`。首次刷入开发分区可能覆盖量产身份或数据，写入前明确设备、镜像与数据影响并取得授权。
