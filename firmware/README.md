# ESP32-S3 固件

目标硬件：ESP32-S3-WROOM-1-N16R8 开发板。

当前固件支持：

- 串口输出芯片、Flash、PSRAM 和复位信息；
- GPIO48 板载 WS2812 RGB 灯状态提示；
- 连接 Wi-Fi 和 Nest.js 后端；
- 创建设备会话、发送心跳并拉取设备指令；
- 将音量和角色配置保存到 NVS 后 ACK；
- 在音频硬件接入前暂缓 ACK 播报及闹钟指令；
- 不使用屏幕、麦克风或功放引脚。

## 启用 ESP-IDF 6.1

```bash
export IDF_PATH=/Users/sxunt/Downloads/espressif/.espressif/v6.1/esp-idf
source "$IDF_PATH/export.sh"
```

## 编译、烧录和查看日志

```bash
cd /Users/sxunt/Downloads/expo/figure/firmware
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.usbmodem143201 flash monitor
```

在 `menuconfig` 中进入 `Figure Board Self-Test`，至少配置：

- `Wi-Fi SSID`
- `Wi-Fi password`
- `Figure API base URL`，当前电脑局域网地址示例为
  `http://192.168.18.225:3000/v1`

SSID 和密码只保存在已被 `.gitignore` 排除的 `sdkconfig` 中。

退出串口监视器：按 `Ctrl+]`。

这里使用开发板上标记为 `USB` 的原生 USB Serial/JTAG 接口。当前这块板通过
标记为 `UART` 的 CH343 接口读取正常，但上传 stub 和写 Flash 不稳定；设备名
也可能在重新插拔后变化，可用 `ls /dev/cu.usbmodem*` 重新确认。

RGB 含义：启动时红绿蓝各一次；等待配置时蓝色闪烁；连接 Wi-Fi 时黄色；
心跳成功短亮绿色；网络或后端失败短亮红色。

`backups/` 保存烧录前读取的开发板内容，不提交到 Git。
