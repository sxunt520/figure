import {
  Injectable,
  Logger,
  NotFoundException,
  ServiceUnavailableException,
} from '@nestjs/common';
import { mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { randomUUID } from 'node:crypto';
import { RawData, WebSocket } from 'ws';

interface DashScopeResponse {
  output?: {
    audio?: { url?: string; expires_at?: number };
    url?: string;
    voice_id?: string;
  };
  request_id?: string;
  code?: string;
  message?: string;
}

interface DashScopeRealtimeEvent {
  header?: {
    event?: string;
    error_code?: string;
    error_message?: string;
  };
}

interface SynthesizedAudio {
  audio: Buffer;
  requestId: string | null;
  transport: 'websocket' | 'http';
  firstPackageMs?: number;
}

export interface RealtimeTtsStreamCallbacks {
  onPcmChunk(chunk: Buffer): void;
}

@Injectable()
export class TtsService {
  private readonly logger = new Logger(TtsService.name);
  private readonly cacheDirectory = resolve(process.cwd(), '.data', 'tts');
  private readonly stageDirectionKeywords = [
    '笑',
    '轻笑',
    '微笑',
    '偷笑',
    '苦笑',
    '叹气',
    '叹息',
    '沉默',
    '停顿',
    '小声',
    '低声',
    '温柔',
    '认真',
    '眨眼',
    '歪头',
    '点头',
    '摇头',
    '脸红',
    '害羞',
    '撒娇',
    '疑惑',
    '惊讶',
    '开心',
    '委屈',
    '抱抱',
  ];

  async cloneVoice(
    audioUrl: string,
    requestedPrefix: string,
    requestedModel = 'cosyvoice-v3.5-plus',
  ) {
    const apiKey = process.env.DASHSCOPE_API_KEY?.trim();
    if (!apiKey) {
      throw new ServiceUnavailableException(
        '尚未配置 DASHSCOPE_API_KEY，无法复刻音色',
      );
    }
    const endpoint =
      process.env.DASHSCOPE_VOICE_ENROLLMENT_ENDPOINT?.trim() ||
      'https://dashscope.aliyuncs.com/api/v1/services/audio/tts/customization';
    const model = requestedModel.trim() || 'cosyvoice-v3.5-plus';
    const prefix = requestedPrefix
      .replace(/[^a-zA-Z0-9]/g, '')
      .slice(0, 10) || 'alarm';
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 90_000);
    let response: Response;
    try {
      response = await fetch(endpoint, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${apiKey}`,
          'content-type': 'application/json',
        },
        body: JSON.stringify({
          model: 'voice-enrollment',
          input: {
            action: 'create_voice',
            target_model: model,
            prefix,
            url: audioUrl,
            language_hints: ['zh'],
            max_prompt_audio_length: 20,
            enable_volume_normalization: 'true',
          },
        }),
        signal: controller.signal,
      });
    } catch (error) {
      throw new ServiceUnavailableException(
        `连接阿里云音色复刻服务失败：${error instanceof Error ? error.message : '未知错误'}`,
      );
    } finally {
      clearTimeout(timeout);
    }

    const payload = (await response.json().catch(() => ({}))) as DashScopeResponse;
    if (!response.ok || !payload.output?.voice_id) {
      this.logger.error(
        `DashScope voice cloning failed status=${response.status} requestId=${payload.request_id ?? 'unknown'} code=${payload.code ?? 'unknown'}`,
      );
      throw new ServiceUnavailableException(
        `阿里云音色复刻失败：${payload.message || payload.code || response.status}`,
      );
    }
    this.logger.log(
      `Voice cloned model=${model} voice=${payload.output.voice_id} requestId=${payload.request_id ?? 'unknown'}`,
    );
    return {
      voiceId: payload.output.voice_id,
      model,
      requestId: payload.request_id ?? null,
    };
  }

  async synthesize(
    text: string,
    requestedVoiceId?: string | null,
    requestedModel?: string | null,
    streamCallbacks?: RealtimeTtsStreamCallbacks,
  ) {
    const apiKey = process.env.DASHSCOPE_API_KEY?.trim();
    if (!apiKey) {
      throw new ServiceUnavailableException(
        '尚未配置 DASHSCOPE_API_KEY，无法生成角色语音',
      );
    }

    const endpoint =
      process.env.DASHSCOPE_TTS_ENDPOINT?.trim() ||
      'https://dashscope.aliyuncs.com/api/v1/services/audio/tts/SpeechSynthesizer';
    const defaultModel =
      process.env.DASHSCOPE_TTS_MODEL?.trim() || 'cosyvoice-v3.5-plus';
    const model = requestedModel?.trim() || defaultModel;
    const format = process.env.DASHSCOPE_TTS_FORMAT?.trim() || 'wav';
    const sampleRate = Number(process.env.DASHSCOPE_TTS_SAMPLE_RATE || 24000);
    const defaultVoice =
      process.env.DASHSCOPE_TTS_DEFAULT_VOICE?.trim() || 'longanyang';
    const voice =
      requestedVoiceId && !requestedVoiceId.startsWith('aliyun-voice-placeholder-')
        ? requestedVoiceId
        : defaultVoice;

    if (format !== 'wav') {
      throw new ServiceUnavailableException(
        '当前 ESP32 固件只支持 WAV，请将 DASHSCOPE_TTS_FORMAT 设置为 wav',
      );
    }
    if (!Number.isInteger(sampleRate) || sampleRate < 8000 || sampleRate > 48000) {
      throw new ServiceUnavailableException('DASHSCOPE_TTS_SAMPLE_RATE 配置无效');
    }

    const spokenText = this.normalizeForSpeech(text);
    if (!spokenText) {
      throw new ServiceUnavailableException('文本中没有需要朗读的内容');
    }

    let synthesized: SynthesizedAudio;
    let realtimeStreamStarted = false;
    if (process.env.DASHSCOPE_TTS_REALTIME_ENABLED?.trim() !== 'false') {
      try {
        synthesized = await this.synthesizeRealtimeAudio(
          apiKey,
          model,
          voice,
          sampleRate,
          spokenText,
          streamCallbacks
            ? {
                onPcmChunk: (chunk) => {
                  realtimeStreamStarted = true;
                  streamCallbacks.onPcmChunk(chunk);
                },
              }
            : undefined,
        );
      } catch (error) {
        if (realtimeStreamStarted) {
          throw new ServiceUnavailableException(
            `实时语音流中断：${error instanceof Error ? error.message : '未知错误'}`,
          );
        }
        this.logger.warn(
          `Realtime TTS failed; falling back to HTTP: ${
            error instanceof Error ? error.message : 'unknown error'
          }`,
        );
        synthesized = await this.synthesizeHttpAudio(
          endpoint,
          apiKey,
          model,
          voice,
          format,
          sampleRate,
          spokenText,
        );
      }
    } else {
      synthesized = await this.synthesizeHttpAudio(
        endpoint,
        apiKey,
        model,
        voice,
        format,
        sampleRate,
        spokenText,
      );
    }
    const { audio } = synthesized;
    if (
      audio.length < 44 ||
      audio.toString('ascii', 0, 4) !== 'RIFF' ||
      audio.toString('ascii', 8, 12) !== 'WAVE'
    ) {
      throw new ServiceUnavailableException('阿里云返回的音频不是有效 WAV 文件');
    }
    if (audio.length > 8 * 1024 * 1024) {
      throw new ServiceUnavailableException('生成的音频超过设备支持的 8MB 上限');
    }

    await mkdir(this.cacheDirectory, { recursive: true });
    const fileName = `${randomUUID()}.wav`;
    await writeFile(resolve(this.cacheDirectory, fileName), audio);
    this.logger.log(
      `TTS ready model=${model} voice=${voice} transport=${synthesized.transport} bytes=${audio.length} firstPackageMs=${synthesized.firstPackageMs ?? 'n/a'} requestId=${synthesized.requestId ?? 'unknown'}`,
    );
    return {
      audio,
      audioPath: `/audio/${fileName}`,
      format: 'wav' as const,
      sampleRate,
      voice,
      model,
      text: spokenText,
      provider: 'aliyun-dashscope' as const,
      requestId: synthesized.requestId,
    };
  }

  private synthesizeRealtimeAudio(
    apiKey: string,
    model: string,
    voice: string,
    sampleRate: number,
    text: string,
    streamCallbacks?: RealtimeTtsStreamCallbacks,
  ): Promise<SynthesizedAudio> {
    const endpoint =
      process.env.DASHSCOPE_TTS_WS_ENDPOINT?.trim() ||
      'wss://dashscope.aliyuncs.com/api-ws/v1/inference';
    const taskId = randomUUID();
    const startedAt = Date.now();

    return new Promise((resolvePromise, rejectPromise) => {
      const chunks: Buffer[] = [];
      let byteLength = 0;
      let firstPackageMs: number | undefined;
      let settled = false;
      const socket = new WebSocket(endpoint, {
        headers: {
          authorization: `bearer ${apiKey}`,
          'X-DashScope-DataInspection': 'enable',
        },
      });
      const timeout = setTimeout(
        () => fail(new Error('实时语音合成等待超时')),
        30_000,
      );
      const finish = (pcm: Buffer) => {
        if (settled) return;
        settled = true;
        clearTimeout(timeout);
        resolvePromise({
          audio: this.wrapPcmAsWav(pcm, sampleRate),
          requestId: taskId,
          transport: 'websocket',
          firstPackageMs,
        });
        socket.close();
      };
      const fail = (error: Error) => {
        if (settled) return;
        settled = true;
        clearTimeout(timeout);
        rejectPromise(error);
        socket.terminate();
      };
      const send = (action: string, payload: Record<string, unknown>) => {
        socket.send(
          JSON.stringify({
            header: { action, task_id: taskId, streaming: 'duplex' },
            payload,
          }),
        );
      };

      socket.once('open', () => {
        send('run-task', {
          task_group: 'audio',
          task: 'tts',
          function: 'SpeechSynthesizer',
          model,
          parameters: {
            text_type: 'PlainText',
            voice,
            format: 'pcm',
            sample_rate: sampleRate,
            volume: 50,
            rate: 1,
            pitch: 1,
            enable_ssml: false,
          },
          input: {},
        });
      });
      socket.on('message', (data: RawData, isBinary: boolean) => {
        if (settled) return;
        if (isBinary) {
          const chunk = Array.isArray(data)
            ? Buffer.concat(data)
            : Buffer.from(data as ArrayBuffer);
          if (firstPackageMs === undefined) {
            firstPackageMs = Date.now() - startedAt;
          }
          byteLength += chunk.length;
          if (byteLength > 8 * 1024 * 1024) {
            fail(new Error('实时语音音频超过设备支持的 8MB 上限'));
            return;
          }
          chunks.push(chunk);
          try {
            streamCallbacks?.onPcmChunk(chunk);
          } catch (error) {
            fail(
              error instanceof Error
                ? error
                : new Error('实时语音分块处理失败'),
            );
          }
          return;
        }

        let event: DashScopeRealtimeEvent;
        try {
          event = JSON.parse(data.toString()) as DashScopeRealtimeEvent;
        } catch {
          fail(new Error('实时语音服务返回了无效事件'));
          return;
        }
        if (event.header?.event === 'task-started') {
          send('continue-task', { input: { text } });
          send('finish-task', { input: {} });
          return;
        }
        if (event.header?.event === 'task-finished') {
          finish(Buffer.concat(chunks, byteLength));
          return;
        }
        if (event.header?.event === 'task-failed') {
          fail(
            new Error(
              event.header.error_message ||
                event.header.error_code ||
                '实时语音合成失败',
            ),
          );
        }
      });
      socket.once('error', (error) => fail(error));
      socket.once('close', () => {
        if (!settled) fail(new Error('实时语音连接提前关闭'));
      });
    });
  }

  private wrapPcmAsWav(pcm: Buffer, sampleRate: number) {
    const header = Buffer.alloc(44);
    header.write('RIFF', 0, 'ascii');
    header.writeUInt32LE(36 + pcm.length, 4);
    header.write('WAVE', 8, 'ascii');
    header.write('fmt ', 12, 'ascii');
    header.writeUInt32LE(16, 16);
    header.writeUInt16LE(1, 20);
    header.writeUInt16LE(1, 22);
    header.writeUInt32LE(sampleRate, 24);
    header.writeUInt32LE(sampleRate * 2, 28);
    header.writeUInt16LE(2, 32);
    header.writeUInt16LE(16, 34);
    header.write('data', 36, 'ascii');
    header.writeUInt32LE(pcm.length, 40);
    return Buffer.concat([header, pcm], 44 + pcm.length);
  }

  private async synthesizeHttpAudio(
    endpoint: string,
    apiKey: string,
    model: string,
    voice: string,
    format: string,
    sampleRate: number,
    text: string,
  ): Promise<SynthesizedAudio> {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 60_000);
    let response: Response;
    try {
      response = await fetch(endpoint, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${apiKey}`,
          'content-type': 'application/json',
        },
        body: JSON.stringify({
          model,
          input: { text, voice, format, sample_rate: sampleRate },
        }),
        signal: controller.signal,
      });
    } catch (error) {
      throw new ServiceUnavailableException(
        `连接阿里云语音服务失败：${error instanceof Error ? error.message : '未知错误'}`,
      );
    } finally {
      clearTimeout(timeout);
    }

    const payload = (await response.json().catch(() => ({}))) as DashScopeResponse;
    if (!response.ok) {
      this.logger.error(
        `DashScope TTS failed status=${response.status} requestId=${payload.request_id ?? 'unknown'} code=${payload.code ?? 'unknown'}`,
      );
      throw new ServiceUnavailableException(
        `阿里云语音生成失败：${payload.message || payload.code || response.status}`,
      );
    }
    const providerAudioUrl = payload.output?.audio?.url ?? payload.output?.url;
    if (!providerAudioUrl) {
      throw new ServiceUnavailableException('阿里云响应中没有音频地址');
    }
    const audioResponse = await fetch(providerAudioUrl);
    if (!audioResponse.ok) {
      throw new ServiceUnavailableException(
        `下载阿里云生成的音频失败：${audioResponse.status}`,
      );
    }
    return {
      audio: Buffer.from(await audioResponse.arrayBuffer()),
      requestId: payload.request_id ?? null,
      transport: 'http',
    };
  }

  normalizeForSpeech(text: string) {
    const stripped = text
      .replace(/[（(【\[]([^（）()【】\[\]]{1,40})[）)】\]]/g, (matched, inner, offset) => {
        const cue = String(inner).trim();
        if (
          offset === 0 ||
          this.stageDirectionKeywords.some((keyword) => cue.includes(keyword))
        ) {
          return '';
        }
        return matched;
      })
      .replace(/\s+/g, ' ')
      .replace(/\s*([，。！？、,.!?；;：:])\s*/g, '$1')
      .trim();

    return stripped;
  }

  async readCachedAudio(fileName: string) {
    if (!/^[0-9a-f-]{36}\.wav$/i.test(fileName)) {
      throw new NotFoundException('音频不存在');
    }
    const filePath = resolve(this.cacheDirectory, fileName);
    try {
      const [buffer, fileStat] = await Promise.all([
        readFile(filePath),
        stat(filePath),
      ]);
      return { buffer, size: fileStat.size };
    } catch {
      throw new NotFoundException('音频不存在或已过期');
    }
  }
}
