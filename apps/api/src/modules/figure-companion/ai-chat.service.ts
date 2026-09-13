import {
  BadGatewayException,
  Injectable,
  Logger,
  ServiceUnavailableException,
} from '@nestjs/common';

export interface AiChatMessage {
  role: 'user' | 'assistant';
  content: string;
}

interface MiniMaxResponse {
  id?: string;
  choices?: Array<{
    message?: {
      role?: string;
      content?: string;
    };
    finish_reason?: string;
  }>;
  model?: string;
  base_resp?: {
    status_code?: number;
    status_msg?: string;
  };
  error?: {
    message?: string;
  };
}

@Injectable()
export class AiChatService {
  private readonly logger = new Logger(AiChatService.name);

  async complete(prompt: string, history: AiChatMessage[]) {
    const apiKey = this.required('AI_API_KEY');
    const endpoint = this.resolveEndpoint(this.required('AI_API_URL'));
    const model = process.env.AI_MODEL?.trim() || 'MiniMax-M2.7';
    const maxCompletionTokens = this.numberSetting(
      'AI_MAX_COMPLETION_TOKENS',
      300,
      32,
      2048,
    );
    const temperature = this.numberSetting('AI_TEMPERATURE', 1, 0.01, 1);

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
          messages: [
            { role: 'system', content: prompt },
            ...history.map((message) => ({
              role: message.role,
              content: message.content,
            })),
          ],
          temperature,
          max_completion_tokens: maxCompletionTokens,
          stream: false,
        }),
        signal: controller.signal,
      });
    } catch (error) {
      throw new ServiceUnavailableException(
        `连接 MiniMax 对话服务失败：${
          error instanceof Error ? error.message : '未知错误'
        }`,
      );
    } finally {
      clearTimeout(timeout);
    }

    const payload = (await response.json().catch(() => ({}))) as MiniMaxResponse;
    if (
      !response.ok ||
      (payload.base_resp?.status_code !== undefined &&
        payload.base_resp.status_code !== 0)
    ) {
      const message =
        payload.error?.message ||
        payload.base_resp?.status_msg ||
        `HTTP ${response.status}`;
      this.logger.error(
        `MiniMax failed status=${response.status} model=${model} requestId=${payload.id ?? 'unknown'} message=${message}`,
      );
      throw new BadGatewayException(`MiniMax 对话失败：${message}`);
    }

    const rawContent = payload.choices?.[0]?.message?.content?.trim() ?? '';
    const content = this.removeThinking(rawContent);
    if (!content) {
      throw new BadGatewayException('MiniMax 没有返回可播放的回复文字');
    }
    this.logger.log(
      `MiniMax reply ready model=${payload.model ?? model} chars=${content.length} requestId=${payload.id ?? 'unknown'}`,
    );
    return {
      content,
      model: payload.model ?? model,
      requestId: payload.id ?? null,
    };
  }

  private resolveEndpoint(baseUrl: string) {
    const normalized = baseUrl.replace(/\/+$/, '');
    if (
      normalized.endsWith('/chat/completions') ||
      normalized.endsWith('/text/chatcompletion_v2')
    ) {
      return normalized;
    }
    return `${normalized}/chat/completions`;
  }

  private removeThinking(content: string) {
    return content.replace(/<think>[\s\S]*?<\/think>/gi, '').trim();
  }

  private numberSetting(name: string, fallback: number, min: number, max: number) {
    const value = Number(process.env[name] ?? fallback);
    if (!Number.isFinite(value) || value < min || value > max) {
      throw new ServiceUnavailableException(`${name} 配置无效`);
    }
    return value;
  }

  private required(name: string) {
    const value = process.env[name]?.trim();
    if (!value) {
      throw new ServiceUnavailableException(`尚未配置 ${name}`);
    }
    return value;
  }
}
