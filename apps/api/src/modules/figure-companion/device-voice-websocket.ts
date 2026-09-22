import { INestApplication, Logger } from '@nestjs/common';
import { IncomingMessage } from 'http';
import { Socket } from 'net';
import { RawData, WebSocket, WebSocketServer } from 'ws';
import { AsrService, RealtimeAsrSession } from './asr.service';
import { DeviceEntity } from './entities';
import { DeviceRealtimeReplyEvent, StoreService } from './store.service';
import { VoiceRecognitionHub } from './voice-recognition-hub';

interface AudioStartRequest {
  type: 'audio.start';
  token: string;
  format: 'pcm_s16le';
  sampleRate: 16000;
  channels: 1;
  commandId?: string | null;
  pushToTalk?: boolean;
}

const STREAM_PATH = '/v1/device/conversation/stream';
const MAX_PCM_BYTES = 16000 * 2 * 10;
const MIN_PCM_BYTES = 16000 * 2 / 10;
const IDLE_TIMEOUT_MS = 15_000;
const REPLY_TIMEOUT_MS = 120_000;

function isDeviceVoiceUpgrade(request: IncomingMessage) {
  try {
    return new URL(request.url ?? '/', 'http://localhost').pathname === STREAM_PATH;
  } catch {
    return false;
  }
}

function readStart(data: RawData): AudioStartRequest {
  const payload = JSON.parse(data.toString()) as Partial<AudioStartRequest>;
  if (
    payload.type !== 'audio.start' ||
    typeof payload.token !== 'string' ||
    payload.format !== 'pcm_s16le' ||
    payload.sampleRate !== 16000 ||
    payload.channels !== 1 ||
    (payload.commandId !== undefined &&
      payload.commandId !== null &&
      typeof payload.commandId !== 'string')
  ) {
    throw new Error('语音流参数无效，仅支持 PCM16/16kHz/单声道');
  }
  return payload as AudioStartRequest;
}

function toBuffer(data: RawData) {
  if (Buffer.isBuffer(data)) return data;
  if (Array.isArray(data)) return Buffer.concat(data);
  return Buffer.from(data as ArrayBuffer);
}

export function registerDeviceVoiceWebSocket(app: INestApplication) {
  const logger = new Logger('DeviceVoiceWebSocket');
  const store = app.get(StoreService);
  const asr = app.get(AsrService);
  const voiceHub = app.get(VoiceRecognitionHub);
  const server = app.getHttpServer();
  const webSocketServer = new WebSocketServer({ noServer: true });

  server.on(
    'upgrade',
    (request: IncomingMessage, socket: Socket, head: Buffer) => {
      if (!isDeviceVoiceUpgrade(request)) return;
      webSocketServer.handleUpgrade(request, socket, head, (webSocket) => {
        webSocketServer.emit('connection', webSocket, request);
      });
    },
  );

  webSocketServer.on('connection', (socket) => {
    let device: DeviceEntity | null = null;
    let request: AudioStartRequest | null = null;
    let chunks: Buffer[] = [];
    let receivedBytes = 0;
    let finishing = false;
    let realtimeAsr: RealtimeAsrSession | null = null;
    let realtimeFallbackReason: string | null = null;
    let idleTimer: NodeJS.Timeout | undefined;
    let replyTimer: NodeJS.Timeout | undefined;
    let speechEndedAt: number | null = null;

    const emit = (event: Record<string, unknown>) => {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify(event));
      }
    };
    const closeWithError = (message: string, code = 1008) => {
      logger.warn(
        `Device voice socket failed device=${device?.id ?? 'unauthenticated'}: ${message}`,
      );
      if (device) {
        voiceHub.publish({ type: 'voice.error', deviceId: device.id, message });
      }
      emit({ type: 'error', message });
      if (socket.readyState === WebSocket.OPEN) socket.close(code, 'failed');
    };
    const resetIdleTimer = () => {
      if (idleTimer) clearTimeout(idleTimer);
      idleTimer = setTimeout(
        () => closeWithError('语音流等待超时', 1008),
        IDLE_TIMEOUT_MS,
      );
    };
    const resetReplyTimer = () => {
      if (replyTimer) clearTimeout(replyTimer);
      replyTimer = setTimeout(
        () => closeWithError('AI 回复等待超时', 1011),
        REPLY_TIMEOUT_MS,
      );
    };
    const emitRealtimeReply = (event: DeviceRealtimeReplyEvent) => {
      const elapsedMs = speechEndedAt === null ? null : Date.now() - speechEndedAt;
      emit({ ...event, elapsedMsSinceSpeechEnd: elapsedMs });
      resetReplyTimer();
      if (event.type === 'reply.audio') {
        logger.log(
          `Realtime reply audio ready device=${device?.id ?? 'unknown'} sequence=${event.sequence} elapsedMs=${elapsedMs ?? 'n/a'}`,
        );
      } else if (event.type === 'reply.completed') {
        logger.log(
          `Realtime reply completed device=${device?.id ?? 'unknown'} chunks=${event.chunkCount} elapsedMs=${elapsedMs ?? 'n/a'}`,
        );
      }
    };

    resetIdleTimer();
    socket.once('close', () => {
      if (idleTimer) clearTimeout(idleTimer);
      if (replyTimer) clearTimeout(replyTimer);
      if (!finishing) realtimeAsr?.abort();
      chunks = [];
    });
    socket.on('message', async (data, isBinary) => {
      if (finishing) {
        if (isBinary || !device) return;
        try {
          const payload = JSON.parse(data.toString()) as {
            type?: string;
            commandId?: string;
            played?: boolean;
          };
          if (
            payload.type === 'reply.audio.ack' &&
            typeof payload.commandId === 'string'
          ) {
            await store.acknowledgeCommand(device, payload.commandId);
            emit({
              type: 'reply.audio.acknowledged',
              commandId: payload.commandId,
              played: payload.played === true,
            });
            resetReplyTimer();
            return;
          }
          if (payload.type === 'reply.interrupt') {
            const cancellation = await store.interruptDeviceConversation(device);
            if (replyTimer) clearTimeout(replyTimer);
            emit({ type: 'reply.interrupted', ...cancellation });
            logger.log(`Realtime reply interrupted device=${device.id}`);
            if (socket.readyState === WebSocket.OPEN) {
              socket.close(1000, 'interrupted');
            }
            return;
          }
          if (payload.type === 'reply.done') {
            if (replyTimer) clearTimeout(replyTimer);
            emit({ type: 'session.completed' });
            logger.log(
              `Realtime voice turn finished device=${device.id} elapsedMs=${speechEndedAt === null ? 'n/a' : Date.now() - speechEndedAt}`,
            );
            if (socket.readyState === WebSocket.OPEN) {
              socket.close(1000, 'completed');
            }
            return;
          }
          throw new Error('未知的实时回复控制消息');
        } catch (error) {
          closeWithError(
            error instanceof Error ? error.message : '实时回复确认失败',
            1008,
          );
        }
        return;
      }
      resetIdleTimer();
      try {
        if (!device || !request) {
          if (isBinary) throw new Error('请先发送 audio.start');
          request = readStart(data);
          device = await store.getDeviceForToken(request.token);
          if (!device) throw new Error('设备登录状态无效，请重新创建会话');
          voiceHub.publish({ type: 'voice.listening', deviceId: device.id });
          try {
            realtimeAsr = await asr.startRealtimeRecognition((progress) => {
              if (device) {
                voiceHub.publish({
                  type: progress.final
                    ? 'voice.transcript.sentence'
                    : 'voice.transcript.partial',
                  deviceId: device.id,
                  text: progress.text,
                  elapsedMs: progress.elapsedMs,
                });
              }
              emit({
                type: progress.final
                  ? 'transcript.sentence'
                  : 'transcript.partial',
                text: progress.text,
                sentenceIndex: progress.sentenceIndex,
                audioTimeMs: progress.audioTimeMs,
                elapsedMs: progress.elapsedMs,
              });
            });
          } catch (error) {
            realtimeFallbackReason =
              error instanceof Error ? error.message : '实时识别连接失败';
            logger.warn(
              `Realtime ASR unavailable device=${device.id}; using batch fallback: ${realtimeFallbackReason}`,
            );
          }
          emit({
            type: 'session.ready',
            format: request.format,
            sampleRate: request.sampleRate,
            channels: request.channels,
            maxDurationMs: 10_000,
            asrMode: realtimeAsr ? 'realtime' : 'batch_fallback',
          });
          logger.log(`Device voice stream started device=${device.id}`);
          return;
        }

        if (isBinary) {
          const chunk = toBuffer(data);
          if ((chunk.length & 1) !== 0) {
            throw new Error('PCM 数据未按 16-bit 采样对齐');
          }
          if (receivedBytes + chunk.length > MAX_PCM_BYTES) {
            throw new Error('录音超过 10 秒上限');
          }
          chunks.push(Buffer.from(chunk));
          receivedBytes += chunk.length;
          realtimeAsr?.write(chunk);
          return;
        }

        const payload = JSON.parse(data.toString()) as { type?: string };
        if (payload.type !== 'audio.end') {
          throw new Error('未知的语音流控制消息');
        }
        if (receivedBytes < MIN_PCM_BYTES) {
          throw new Error('录音过短，请至少说话 0.1 秒');
        }
        finishing = true;
        speechEndedAt = Date.now();
        if (idleTimer) clearTimeout(idleTimer);
        const pcm = Buffer.concat(chunks, receivedBytes);
        chunks = [];
        emit({ type: 'audio.received', bytes: receivedBytes });
        let result: Awaited<ReturnType<StoreService['receiveDevicePcmStream']>>;
        if (realtimeAsr) {
          try {
            const recognition = await realtimeAsr.finish();
            result = await store.receiveDeviceRealtimeRecognition(
              device,
              request.commandId ?? undefined,
              recognition,
              emitRealtimeReply,
            );
          } catch (error) {
            realtimeFallbackReason =
              error instanceof Error ? error.message : '实时识别失败';
            logger.warn(
              `Realtime ASR failed device=${device.id}; retrying with batch ASR: ${realtimeFallbackReason}`,
            );
            result = await store.receiveDevicePcmStream(
              device,
              request.commandId ?? undefined,
              pcm,
              emitRealtimeReply,
            );
          }
        } else {
          result = await store.receiveDevicePcmStream(
            device,
            request.commandId ?? undefined,
            pcm,
            emitRealtimeReply,
          );
        }
        emit({
          type: 'transcript.final',
          text: result.text,
          durationMs: result.durationMs,
          taskId: result.taskId,
          asrMode:
            result.provider === 'aliyun-nls-realtime'
              ? 'realtime'
              : 'batch_fallback',
          firstPartialMs:
            'firstPartialMs' in result ? result.firstPartialMs : null,
        });
        voiceHub.publish({
          type: 'voice.transcript.final',
          deviceId: device.id,
          text: result.text,
          durationMs: result.durationMs,
          firstPartialMs:
            'firstPartialMs' in result ? result.firstPartialMs : null,
          asrMode:
            result.provider === 'aliyun-nls-realtime'
              ? 'realtime'
              : 'batch_fallback',
        });
        emit({
          type: 'conversation.accepted',
          accepted: true,
          hasText: Boolean(result.text),
        });
        logger.log(
          `Device voice stream completed device=${device.id} bytes=${receivedBytes} chars=${result.text.length} provider=${result.provider} firstPartialMs=${'firstPartialMs' in result ? result.firstPartialMs : 'n/a'}${realtimeFallbackReason ? ` fallback=${realtimeFallbackReason}` : ''}`,
        );
        if (!result.text && socket.readyState === WebSocket.OPEN) {
          socket.close(1000, 'completed');
        } else if (result.text) {
          resetReplyTimer();
        }
      } catch (error) {
        closeWithError(
          error instanceof Error ? error.message : '实时语音处理失败',
          finishing ? 1011 : 1008,
        );
      }
    });
  });

  logger.log(`Device voice WebSocket ready at ${STREAM_PATH}`);
}
