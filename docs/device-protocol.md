# ESP32-S3 设备协议（MVP）

基础地址示例：`http://192.168.1.20:3000/v1`

除创建设备会话外，设备接口都携带：

```http
Authorization: Bearer <deviceAccessToken>
Content-Type: application/json
```

## 1. 创建设备会话

```http
POST /device/session
```

```json
{
  "hardwareId": "ESP32S3-DEMO-001",
  "deviceSecret": "figure-dev-secret-001"
}
```

返回设备访问 Token、绑定码和当前绑定状态。正式量产时，`hardwareId` 和 `deviceSecret` 应在生产测试阶段写入 NVS，不能所有设备共用同一个密钥。

## 2. 心跳

建议每 10～15 秒发送一次：

```http
POST /device/heartbeat
```

```json
{
  "firmwareVersion": "0.1.0",
  "volume": 60
}
```

后端在 45 秒内收到过心跳，就把设备视为在线。

## 3. 拉取控制指令

```http
GET /device/commands
```

返回尚未确认的指令数组。MVP 支持：

| type | 用途 | payload 主要字段 |
|---|---|---|
| `sync_character` | 更新角色与音色配置 | `character` |
| `set_volume` | 调节输出音量 | `volume` |
| `speak_text` | 让角色播报文本 | `text`, `voiceId` |
| `play_reminder` | 触发语音提醒 | `title`, `voiceId` |

设备只有在执行成功或已经安全保存指令后才进行 ACK。

## 4. 确认指令

```http
POST /device/commands/<commandId>/ack
```

请求体使用空 JSON 对象 `{}`。未 ACK 的指令会在下一次拉取时再次返回，所以固件需要根据 `commandId` 防止重复执行。

## 5. 上报设备事件

```http
POST /device/events
```

```json
{
  "type": "button_pressed",
  "payload": {
    "button": "talk"
  }
}
```

后续可增加 `wake_word_detected`、`playback_started`、`playback_finished`、`wifi_signal` 和错误信息。

## 6. 文本对话联调

```http
POST /device/conversation/messages
```

```json
{
  "text": "今天有什么安排？"
}
```

当前返回模拟角色回复和 `audioUrl: null`。接入阿里云后，短期可以返回生成的音频 URL；实时语音阶段应升级为独立 WebSocket + Opus 流，不通过这个 JSON 接口传输大段 PCM 数据。
