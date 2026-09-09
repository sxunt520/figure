# ESP32-S3 固件

目标硬件：ESP32-S3-WROOM-1-N16R8 开发板。

当前固件支持：

- 串口输出芯片、Flash、PSRAM 和复位信息；
- GPIO48 板载 WS2812 RGB 灯状态提示；
- 连接 Wi-Fi 和 Nest.js 后端；
- 创建设备会话、发送心跳并拉取设备指令；
- 将音量和角色配置保存到 NVS 后 ACK；
- 驱动 1.54 英寸 ST7789 屏幕显示设备状态；
- 驱动 HT517 I2S 功放并播放启动音；
- 下载后端 CosyVoice 生成的 WAV，经 I2S 播放完成后 ACK `speak_text`；
- 到点下载并播放当前角色音色的 `play_reminder` 语音提醒；
- 麦克风仍处于硬件调试阶段，等 INMP441 到货后继续。

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

## TTS 实机链路

后端使用 `DASHSCOPE_API_KEY` 请求 CosyVoice，把生成的 WAV 缓存在
`.data/tts/`，再向设备下发带 `audioPath` 的 `speak_text`。ESP32 使用设备令牌
下载音频，屏幕依次显示 `DOWNLOADING`、`SPEAKING` 和 `SPEAK OK`。

当前仅接受 WAV PCM 16-bit、单/双声道、8～48 kHz，单个文件最大 8 MB。
音频下载或解析失败时不会 ACK，设备会在下一轮继续重试。

`backups/` 保存烧录前读取的开发板内容，不提交到 Git。
