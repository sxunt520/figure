import {
  BadRequestException,
  BadGatewayException,
  Injectable,
  Logger,
  ServiceUnavailableException,
} from '@nestjs/common';
import RPCClient = require('@alicloud/pop-core');

interface NlsTokenResponse {
  Token?: {
    Id?: string;
    ExpireTime?: number;
  };
}

interface NlsAsrResponse {
  task_id?: string;
  result?: string;
  status?: number;
  message?: string;
}

export interface WavMetadata {
  sampleRate: number;
  channels: number;
  bitsPerSample: number;
  dataBytes: number;
  durationMs: number;
}

@Injectable()
export class AsrService {
  private readonly logger = new Logger(AsrService.name);
  private cachedToken?: { id: string; expiresAtSeconds: number };

  async recognizeWav(audio: Buffer) {
    const metadata = this.validateWav(audio);
    const appKey = this.required('ALIYUN_NLS_APP_KEY');
    const token = await this.getToken();
    const endpoint =
      process.env.ALIYUN_NLS_ASR_ENDPOINT?.trim() ||
      'https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/asr';
    const url = new URL(endpoint);
    url.search = new URLSearchParams({
      appkey: appKey,
      format: 'wav',
      sample_rate: String(metadata.sampleRate),
      enable_punctuation_prediction: 'true',
      enable_inverse_text_normalization: 'true',
      enable_voice_detection: 'true',
    }).toString();

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 30_000);
    let response: Response;
    try {
      response = await fetch(url, {
        method: 'POST',
        headers: {
          'X-NLS-Token': token,
          'Content-Type': 'application/octet-stream',
          'Content-Length': String(audio.length),
        },
        body: new Uint8Array(audio),
        signal: controller.signal,
      });
    } catch (error) {
      throw new ServiceUnavailableException(
        `连接阿里云语音识别服务失败：${
          error instanceof Error ? error.message : '未知错误'
        }`,
      );
    } finally {
      clearTimeout(timeout);
    }

    const payload = (await response.json().catch(() => ({}))) as NlsAsrResponse;
    if (!response.ok || payload.status !== 20000000) {
      this.logger.error(
        `NLS ASR failed http=${response.status} status=${payload.status ?? 'unknown'} taskId=${payload.task_id ?? 'unknown'} message=${payload.message ?? 'unknown'}`,
      );
      throw new BadGatewayException(
        `阿里云语音识别失败：${payload.message || payload.status || response.status}`,
      );
    }

    const text = payload.result?.trim() ?? '';
    this.logger.log(
      `ASR ready chars=${text.length} durationMs=${metadata.durationMs} taskId=${payload.task_id ?? 'unknown'}`,
    );
    return {
      text,
      taskId: payload.task_id ?? null,
      provider: 'aliyun-nls' as const,
      ...metadata,
    };
  }

  private async getToken() {
    const nowSeconds = Math.floor(Date.now() / 1000);
    if (
      this.cachedToken &&
      this.cachedToken.expiresAtSeconds > nowSeconds + 300
    ) {
      return this.cachedToken.id;
    }

    const client = new RPCClient({
      accessKeyId: this.required('ALIYUN_ACCESS_KEY_ID'),
      accessKeySecret: this.required('ALIYUN_ACCESS_KEY_SECRET'),
      endpoint:
        process.env.ALIYUN_NLS_TOKEN_ENDPOINT?.trim() ||
        'https://nls-meta.cn-shanghai.aliyuncs.com',
      apiVersion: '2019-02-28',
    });
    let result: NlsTokenResponse;
    try {
      result = await client.request<NlsTokenResponse>(
        'CreateToken',
        {},
        { method: 'POST' },
      );
    } catch (error) {
      throw new ServiceUnavailableException(
        `获取阿里云 NLS Token 失败：${
          error instanceof Error ? error.message : '未知错误'
        }`,
      );
    }

    const id = result.Token?.Id;
    const expiresAtSeconds = Number(result.Token?.ExpireTime ?? 0);
    if (!id || !Number.isFinite(expiresAtSeconds)) {
      throw new ServiceUnavailableException('阿里云 NLS Token 响应无效');
    }
    this.cachedToken = { id, expiresAtSeconds };
    this.logger.log('NLS access token refreshed');
    return id;
  }

  private validateWav(audio: Buffer): WavMetadata {
    if (audio.length < 44 || audio.length > 512 * 1024) {
      throw new BadRequestException('录音大小无效，必须为 44B～512KB');
    }
    if (
      audio.toString('ascii', 0, 4) !== 'RIFF' ||
      audio.toString('ascii', 8, 12) !== 'WAVE'
    ) {
      throw new BadRequestException('设备上传的内容不是 RIFF/WAVE 音频');
    }

    let offset = 12;
    let audioFormat = 0;
    let channels = 0;
    let sampleRate = 0;
    let bitsPerSample = 0;
    let dataBytes = 0;
    while (offset + 8 <= audio.length) {
      const chunkName = audio.toString('ascii', offset, offset + 4);
      const chunkLength = audio.readUInt32LE(offset + 4);
      const dataOffset = offset + 8;
      if (chunkLength > audio.length - dataOffset) {
        throw new BadRequestException('WAV 分块长度无效');
      }
      if (chunkName === 'fmt ' && chunkLength >= 16) {
        audioFormat = audio.readUInt16LE(dataOffset);
        channels = audio.readUInt16LE(dataOffset + 2);
        sampleRate = audio.readUInt32LE(dataOffset + 4);
        bitsPerSample = audio.readUInt16LE(dataOffset + 14);
      } else if (chunkName === 'data') {
        dataBytes = chunkLength;
      }
      offset = dataOffset + chunkLength + (chunkLength & 1);
    }

    if (
      audioFormat !== 1 ||
      channels !== 1 ||
      sampleRate !== 16000 ||
      bitsPerSample !== 16 ||
      dataBytes === 0
    ) {
      throw new BadRequestException(
        '仅支持 16kHz、16-bit、单声道 PCM WAV 录音',
      );
    }
    return {
      sampleRate,
      channels,
      bitsPerSample,
      dataBytes,
      durationMs: Math.round(
        (dataBytes * 1000) / (sampleRate * channels * (bitsPerSample / 8)),
      ),
    };
  }

  private required(name: string) {
    const value = process.env[name]?.trim();
    if (!value) {
      throw new ServiceUnavailableException(`尚未配置 ${name}`);
    }
    return value;
  }
}
