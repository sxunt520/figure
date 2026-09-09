import {
  Character,
  Device,
  DeviceCommand,
  DeviceEvent,
  LoginResponse,
  Reminder,
} from './types';

export const API_BASE_URL =
  process.env.EXPO_PUBLIC_API_BASE_URL ?? 'http://192.168.18.225:3000/v1';

async function request<T>(
  path: string,
  options: RequestInit = {},
  token?: string,
): Promise<T> {
  const response = await fetch(`${API_BASE_URL}${path}`, {
    ...options,
    headers: {
      'content-type': 'application/json',
      ...(token ? { authorization: `Bearer ${token}` } : {}),
      ...(options.headers ?? {}),
    },
  });
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

  listDevices: (token: string) => request<Device[]>('/devices', {}, token),

  bindDevice: (
    token: string,
    body: { pairingCode: string; characterId: string; name?: string },
  ) =>
    request<Device>(
      '/devices/bind',
      { method: 'POST', body: JSON.stringify(body) },
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

  listDeviceCommands: (token: string, deviceId: string) =>
    request<DeviceCommand[]>(`/devices/${deviceId}/commands`, {}, token),

  listDeviceEvents: (token: string, deviceId: string) =>
    request<DeviceEvent[]>(`/devices/${deviceId}/events`, {}, token),

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
