import {
  Character,
  ConversationMessage,
  Device,
  DeviceCommand,
  DeviceEvent,
  LoginResponse,
  Reminder,
} from './types';

function normalizeApiBaseUrl(value?: string) {
  const raw = value?.trim() || 'http://192.168.18.225:3000';
  const withoutTrailingSlash = raw.replace(/\/+$/, '');
  return withoutTrailingSlash.endsWith('/v1')
    ? withoutTrailingSlash
    : `${withoutTrailingSlash}/v1`;
}

export const API_BASE_URL = normalizeApiBaseUrl(
  process.env.EXPO_PUBLIC_API_BASE_URL,
);

async function request<T>(
  path: string,
  options: RequestInit = {},
  token?: string,
): Promise<T> {
  const url = `${API_BASE_URL}${path}`;
  let response: Response;
  try {
    response = await fetch(url, {
      ...options,
      headers: {
        'content-type': 'application/json',
        ...(token ? { authorization: `Bearer ${token}` } : {}),
        ...(options.headers ?? {}),
      },
    });
  } catch (error) {
    const reason = error instanceof Error ? error.message : '网络不可达';
    throw new Error(`无法连接后端 ${API_BASE_URL}：${reason}`);
  }
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) {
    const message = Array.isArray(payload.message)
      ? payload.message.join('，')
      : payload.message;
    throw new Error(message || `请求失败（${response.status}）`);
  }
  return payload as T;
}

export const api = {
  loginDemo: () =>
    request<LoginResponse>('/auth/demo', {
      method: 'POST',
      body: '{}',
    }),

  listCharacters: (token: string) =>
    request<Character[]>('/characters', {}, token),

  updateCharacterPrompt: (token: string, characterId: string, prompt: string) =>
    request<Character>(
      `/characters/${characterId}/prompt`,
      { method: 'PATCH', body: JSON.stringify({ prompt }) },
      token,
    ),

  bindCharacterNfcTag: (token: string, characterId: string, uid: string) =>
    request<Character>(
      `/characters/${characterId}/nfc-tag`,
      { method: 'PUT', body: JSON.stringify({ uid }) },
      token,
    ),

  unbindCharacterNfcTag: (token: string, characterId: string) =>
    request<Character>(
      `/characters/${characterId}/nfc-tag`,
      { method: 'DELETE' },
      token,
    ),

  listDevices: (token: string) => request<Device[]>('/devices', {}, token),

  bindDevice: (
    token: string,
    body: { pairingCode: string; characterId?: string; name?: string },
  ) =>
    request<Device>(
      '/devices/bind',
      { method: 'POST', body: JSON.stringify(body) },
      token,
    ),

  unbindDevice: (token: string, deviceId: string) =>
    request<{ unbound: boolean; deviceId: string }>(
      `/devices/${deviceId}/binding`,
      { method: 'DELETE' },
      token,
    ),

  updateCharacter: (token: string, deviceId: string, characterId: string) =>
    request<Device>(
      `/devices/${deviceId}/character`,
      { method: 'PATCH', body: JSON.stringify({ characterId }) },
      token,
    ),

  updateDeviceName: (token: string, deviceId: string, name: string) =>
    request<Device>(
      `/devices/${deviceId}/name`,
      { method: 'PATCH', body: JSON.stringify({ name }) },
      token,
    ),

  updateVolume: (token: string, deviceId: string, volume: number) =>
    request<Device>(
      `/devices/${deviceId}/volume`,
      { method: 'PATCH', body: JSON.stringify({ volume }) },
      token,
    ),

  speakText: (token: string, deviceId: string, text: string) =>
    request(
      `/devices/${deviceId}/speak`,
      { method: 'POST', body: JSON.stringify({ text }) },
      token,
    ),

  startListening: (token: string, deviceId: string) =>
    request<DeviceCommand>(
      `/devices/${deviceId}/listen`,
      { method: 'POST', body: '{}' },
      token,
    ),

  listDeviceCommands: (token: string, deviceId: string) =>
    request<DeviceCommand[]>(`/devices/${deviceId}/commands`, {}, token),

  listDeviceEvents: (token: string, deviceId: string) =>
    request<DeviceEvent[]>(`/devices/${deviceId}/events`, {}, token),

  listConversationMessages: (token: string, deviceId: string) =>
    request<ConversationMessage[]>(`/devices/${deviceId}/messages`, {}, token),

  listReminders: (token: string, deviceId?: string) =>
    request<Reminder[]>(
      `/reminders${deviceId ? `?deviceId=${encodeURIComponent(deviceId)}` : ''}`,
      {},
      token,
    ),

  createReminder: (
    token: string,
    body: {
      deviceId: string;
      title: string;
      scheduledAt: string;
      repeat: 'none' | 'daily';
      kind: 'reminder' | 'alarm';
    },
  ) =>
    request<Reminder>(
      '/reminders',
      { method: 'POST', body: JSON.stringify(body) },
      token,
    ),

  toggleReminder: (token: string, reminderId: string, enabled: boolean) =>
    request<Reminder>(
      `/reminders/${reminderId}`,
      { method: 'PATCH', body: JSON.stringify({ enabled }) },
      token,
    ),

  deleteReminder: (token: string, reminderId: string) =>
    request(
      `/reminders/${reminderId}`,
      { method: 'DELETE' },
      token,
    ),
};
