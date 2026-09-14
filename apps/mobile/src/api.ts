import {
  Alarm,
  AlarmSound,
  Character,
  ConversationMessage,
  Device,
  DeviceCommand,
  DeviceEvent,
  LoginResponse,
  Reminder,
} from './types';
import * as FileSystem from 'expo-file-system';

export type AlarmInput = Pick<
  Alarm,
  | 'deviceId'
  | 'hour'
  | 'minute'
  | 'days'
  | 'enabled'
  | 'snoozeEnabled'
  | 'snoozeMinutes'
  | 'snoozeCount'
  | 'themeId'
  | 'useThemeSound'
  | 'soundTitle'
  | 'soundId'
  | 'timezone'
>;

const DEFAULT_API_BASE_URL = process.env.EXPO_PUBLIC_API_BASE_URL || 'http://192.168.18.225:3000';
const API_SETTINGS_FILE = FileSystem.documentDirectory
  ? `${FileSystem.documentDirectory}backend-settings.json`
  : null;

export function normalizeApiBaseUrl(value?: string) {
  const raw = value?.trim();
  if (!raw) throw new Error('请输入后端服务地址');

  const withProtocol = /^https?:\/\//i.test(raw) ? raw : `http://${raw}`;
  let parsed: URL;
  try {
    parsed = new URL(withProtocol);
  } catch {
    throw new Error('后端地址格式不正确，例如：http://192.168.31.157:3000');
  }
  if (!['http:', 'https:'].includes(parsed.protocol) || !parsed.hostname) {
    throw new Error('后端地址只支持 http 或 https');
  }
  if (parsed.username || parsed.password || parsed.search || parsed.hash) {
    throw new Error('后端地址不能包含账号、查询参数或锚点');
  }
  const path = parsed.pathname.replace(/\/+$/, '');
  if (path && path !== '/v1') {
    throw new Error('后端地址只需填写 IP（或域名）和端口，不要附加其他路径');
  }
  return `${parsed.protocol}//${parsed.host}/v1`;
}

export let API_BASE_URL = normalizeApiBaseUrl(DEFAULT_API_BASE_URL);

export function getApiBaseUrl() {
  return API_BASE_URL;
}

export async function initializeApiBaseUrl() {
  if (!API_SETTINGS_FILE) return API_BASE_URL;
  try {
    const raw = await FileSystem.readAsStringAsync(API_SETTINGS_FILE);
    const settings = JSON.parse(raw) as { apiBaseUrl?: string };
    API_BASE_URL = normalizeApiBaseUrl(settings.apiBaseUrl);
  } catch {
    API_BASE_URL = normalizeApiBaseUrl(DEFAULT_API_BASE_URL);
  }
  return API_BASE_URL;
}

export async function saveApiBaseUrl(value: string) {
  const normalized = normalizeApiBaseUrl(value);
  if (!API_SETTINGS_FILE) throw new Error('当前设备无法保存后端地址');
  await FileSystem.writeAsStringAsync(
    API_SETTINGS_FILE,
    JSON.stringify({ apiBaseUrl: normalized }),
  );
  API_BASE_URL = normalized;
  return normalized;
}

async function request<T>(
  path: string,
  options: RequestInit = {},
  token?: string,
): Promise<T> {
  const url = `${API_BASE_URL}${path}`;
  let response: Response;
  try {
    const isFormData = typeof FormData !== 'undefined' && options.body instanceof FormData;
    response = await fetch(url, {
      ...options,
      headers: {
        ...(!isFormData ? { 'content-type': 'application/json' } : {}),
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

  listAlarms: (token: string, deviceId: string) =>
    request<Alarm[]>(
      `/alarms?deviceId=${encodeURIComponent(deviceId)}`,
      {},
      token,
    ),

  createAlarm: (token: string, body: AlarmInput) =>
    request<Alarm>(
      '/alarms',
      { method: 'POST', body: JSON.stringify(body) },
      token,
    ),

  updateAlarm: (
    token: string,
    alarmId: string,
    body: Partial<Omit<AlarmInput, 'deviceId'>>,
  ) =>
    request<Alarm>(
      `/alarms/${alarmId}`,
      { method: 'PATCH', body: JSON.stringify(body) },
      token,
    ),

  deleteAlarm: (token: string, alarmId: string) =>
    request<{ deleted: boolean; alarmId: string }>(
      `/alarms/${alarmId}`,
      { method: 'DELETE' },
      token,
    ),

  snoozeAlarm: (token: string, alarmId: string) =>
    request<Alarm>(
      `/alarms/${alarmId}/snooze`,
      { method: 'POST' },
      token,
    ),

  dismissAlarm: (token: string, alarmId: string) =>
    request<Alarm>(
      `/alarms/${alarmId}/dismiss`,
      { method: 'POST' },
      token,
    ),

  listAlarmSounds: (token: string) =>
    request<AlarmSound[]>('/alarm-sounds', {}, token),

  uploadAlarmSound: (
    token: string,
    file: { uri: string; name: string; type: string },
    title?: string,
  ) => {
    const body = new FormData();
    body.append('file', file as unknown as Blob);
    if (title) body.append('title', title);
    return request<AlarmSound>(
      '/alarm-sounds/upload',
      { method: 'POST', body },
      token,
    );
  },

  synthesizeAlarmSound: (
    token: string,
    sourceId: string,
    body: { title: string; text: string; backgroundMusicId?: string | null },
  ) =>
    request<AlarmSound>(
      `/alarm-sounds/${sourceId}/synthesize`,
      { method: 'POST', body: JSON.stringify(body) },
      token,
    ),

  deleteAlarmSound: (token: string, soundId: string) =>
    request<{ deleted: boolean; soundId: string; deletedAlarms: number }>(
      `/alarm-sounds/${soundId}`,
      { method: 'DELETE' },
      token,
    ),

  alarmSoundAudioUrl: (soundId: string) =>
    `${API_BASE_URL}/alarm-sounds/${encodeURIComponent(soundId)}/audio`,

  alarmSoundPlaybackUrl: (token: string, soundId: string) =>
    request<{ url: string; expiresAt: string }>(
      `/alarm-sounds/${encodeURIComponent(soundId)}/playback-url`,
      {},
      token,
    ),
};
