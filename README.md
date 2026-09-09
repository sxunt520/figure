# 手办伙伴 MVP

这是硬件到货前使用的独立联调版本，包含：

- `apps/api`：NestJS 10 后端
- `apps/mobile`：Expo SDK 51 / React Native APP
- `firmware`：ESP-IDF 6.1 / ESP32-S3-N16R8 固件
- `apps/api/scripts/device-simulator.mjs`：ESP32-S3 模拟设备
- `docs/device-protocol.md`：未来固件需要实现的设备协议

## 已实现链路

```text
APP 演示账号
  └─ 绑定码绑定设备和角色
      ├─ 切换角色 → sync_character 指令
      ├─ 调节音量 → set_volume 指令
      ├─ 远程播报 → speak_text 指令
      └─ 定时提醒 → play_reminder 指令
                         ↓
                  ESP32 模拟器拉取并确认
```

业务数据已经持久化到 MySQL/MariaDB，后端重启不会丢失绑定、提醒或设备指令。

## 启动

需要 Node.js 20 或更新版本。

```bash
npm install
npm run db:create --workspace @figure/api
```

默认数据库配置为：

```dotenv
DB_HOST=127.0.0.1
DB_PORT=3306
DB_USERNAME=root
DB_PASSWORD=
DB_DATABASE=figure_companion
DB_SYNCHRONIZE=true
```

本机已验证的数据库服务实际为 MariaDB 10.1.37，因此 JSON 数据使用兼容的文本 JSON 映射。开发环境会通过 TypeORM 自动同步表结构；上线前应关闭 `DB_SYNCHRONIZE` 并改用数据库迁移。

终端 1，启动后端：

```bash
npm run dev:api
```

终端 2，启动模拟设备：

```bash
npm run simulate:device
```

模拟设备会每 3 秒发送心跳并拉取命令，预置凭证为：

- 硬件编号：`ESP32S3-DEMO-001`
- 设备密钥：`figure-dev-secret-001`
- 屏幕绑定码：`FIGURE-0001`

终端 3，启动 Expo APP：

```bash
npm run dev:mobile
```

在 iOS 模拟器运行时，默认 API 地址 `http://127.0.0.1:3000/v1` 可以直接使用。

在真机 Expo Go 中测试时，将 `apps/mobile/.env.example` 复制为 `apps/mobile/.env`，把地址改成电脑局域网 IP，例如：

```dotenv
EXPO_PUBLIC_API_BASE_URL=http://192.168.1.20:3000/v1
```

手机和电脑必须在同一 Wi-Fi，修改 `.env` 后重启 Expo。

## 检查命令

```bash
npm run typecheck
npm run build:api
```

验证 Expo 能否正常打包：

```bash
cd apps/mobile
npx expo export --platform ios --output-dir /tmp/figure-expo-export
```

## 下一阶段

硬件到货后依次替换：

1. 用真实 ESP32 固件替换模拟器，保留相同的设备会话、心跳、命令和 ACK 协议。
2. 将 APP 的演示登录替换为现有 APP 用户 Token。
3. 将 `figure_users` 演示用户映射替换为现有 APP 用户表和正式登录 Token。
4. 将模拟对话服务替换为现有角色对话服务。
5. 接入阿里云流式 ASR、音色复刻 TTS 和音频对象存储。
6. 语音实时对话改用 WebSocket 传输 Opus 音频；当前 HTTP 指令通道继续承担绑定、配置、提醒和控制。

## ESP32-S3 主板自检

硬件到货后的第一阶段固件位于 `firmware/`。它只验证串口、16MB Flash、
8MB Octal PSRAM 和板载 RGB 灯，不会访问屏幕、麦克风或功放引脚。

详细命令见 `firmware/README.md`。

## 目前有意保留的限制

- 演示 Token 和预置设备凭证只用于本地开发。
- 阿里云 `voiceId` 是占位值，没有调用收费接口。
- 没有实现 Wi-Fi 配网、OTA、解绑、设备恢复出厂和固件签名。
- 提醒由运行中的 API 进程触发，正式版应改成持久化任务队列。

## 数据表

| 表 | 内容 |
|---|---|
| `figure_users` | 当前 MVP 的用户映射，后续接入现有用户表 |
| `figure_characters` | 角色资料、问候语和阿里云音色 ID |
| `figure_devices` | 硬件编号、绑定账号、绑定角色、固件和在线状态 |
| `figure_device_sessions` | 设备访问 Token 哈希及过期时间 |
| `figure_reminders` | 闹钟/提醒时间、重复方式和执行状态 |
| `figure_device_commands` | APP 下发给 ESP32 的待执行指令及 ACK |
| `figure_device_events` | 按键、唤醒、播放完成和错误等设备事件 |

设备密钥和设备会话 Token 均以 SHA-256 哈希保存，API 响应不会返回数据库中的密钥哈希。聊天消息暂不重复存储，后续直接接入现有 APP 的 AI 会话数据。
