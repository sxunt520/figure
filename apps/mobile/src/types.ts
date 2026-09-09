export interface User {
  id: string;
  displayName: string;
}

export interface Character {
  id: string;
  name: string;
  description: string;
  accentColor: string;
  voiceId: string;
  greeting: string;
}

export interface Device {
  id: string;
  hardwareId: string;
  name: string;
  firmwareVersion: string;
  characterId: string | null;
  character: Character | null;
  status: 'online' | 'offline';
  lastSeenAt: string | null;
  volume: number;
}

export interface Reminder {
  id: string;
  deviceId: string;
  title: string;
  scheduledAt: string;
  repeat: 'none' | 'daily';
  enabled: boolean;
  lastTriggeredAt: string | null;
}

export type CommandType =
  | 'sync_character'
  | 'play_reminder'
  | 'speak_text'
  | 'set_volume';

export interface DeviceCommand {
  id: string;
  deviceId: string;
  type: CommandType;
  payload: Record<string, unknown>;
  createdAt: string;
  acknowledgedAt: string | null;
}

export interface DeviceEvent {
  id: string;
  deviceId: string;
  type: string;
  payload: Record<string, unknown>;
  createdAt: string;
}

export interface LoginResponse {
  accessToken: string;
  user: User;
}
