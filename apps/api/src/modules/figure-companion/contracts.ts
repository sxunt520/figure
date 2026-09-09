export type DeviceStatus = 'online' | 'offline';
export type ReminderRepeat = 'none' | 'daily';
export type CommandType =
  | 'sync_character'
  | 'play_reminder'
  | 'speak_text'
  | 'set_volume';

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

export interface DeviceRecord {
  id: string;
  hardwareId: string;
  deviceSecret: string;
  pairingCode: string;
  name: string;
  firmwareVersion: string;
  ownerUserId: string | null;
  characterId: string | null;
  lastSeenAt: string | null;
  volume: number;
}

export interface DeviceView {
  id: string;
  hardwareId: string;
  name: string;
  firmwareVersion: string;
  characterId: string | null;
  character: Character | null;
  status: DeviceStatus;
  lastSeenAt: string | null;
  volume: number;
}

export interface Reminder {
  id: string;
  userId: string;
  deviceId: string;
  title: string;
  scheduledAt: string;
  repeat: ReminderRepeat;
  enabled: boolean;
  lastTriggeredAt: string | null;
}

export interface DeviceCommand {
  id: string;
  deviceId: string;
  type: CommandType;
  payload: Record<string, unknown>;
  createdAt: string;
  acknowledgedAt: string | null;
}
