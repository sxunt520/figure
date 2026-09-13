import {
  Injectable,
  Logger,
  NotFoundException,
  ServiceUnavailableException,
} from '@nestjs/common';
import { mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { randomUUID } from 'node:crypto';

interface DashScopeResponse {
  output?: {
    audio?: { url?: string; expires_at?: number };
    url?: string;
  };
  request_id?: string;
  code?: string;
  message?: string;
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

  async synthesize(
    text: string,
    requestedVoiceId?: string | null,
    requestedModel?: string | null,
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

    const spokenText = this.toSpokenText(text);

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
          input: {
            text: spokenText,
            voice,
            format,
            sample_rate: sampleRate,
          },
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
    const audio = Buffer.from(await audioResponse.arrayBuffer());
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
      `TTS ready model=${model} voice=${voice} bytes=${audio.length} requestId=${payload.request_id ?? 'unknown'}`,
    );
    return {
      audioPath: `/audio/${fileName}`,
      format: 'wav' as const,
      sampleRate,
      voice,
      model,
      text: spokenText,
      provider: 'aliyun-dashscope' as const,
      requestId: payload.request_id ?? null,
    };
  }

  private toSpokenText(text: string) {
    const stripped = text
      .replace(/[（(【\[]([^（）()【】\[\]]{1,24})[）)】\]]/g, (matched, inner) => {
        const cue = String(inner).trim();
        if (this.stageDirectionKeywords.some((keyword) => cue.includes(keyword))) {
          return '';
        }
        return matched;
      })
      .replace(/\s+/g, ' ')
      .replace(/\s*([，。！？、,.!?；;：:])\s*/g, '$1')
      .trim();

    return stripped || text.trim();
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
