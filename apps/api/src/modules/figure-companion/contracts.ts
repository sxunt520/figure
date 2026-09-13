export type DeviceStatus = 'online' | 'offline';
export type ReminderRepeat = 'none' | 'daily';
export type ReminderKind = 'reminder' | 'alarm';
export type CommandType =
  | 'sync_character'
  | 'play_reminder'
  | 'speak_text'
  | 'start_listening'
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
  ttsModel: string;
  greeting: string;
  prompt: string;
  nfcTagUid: string | null;
}

export interface ConversationMessage {
  id: string;
  deviceId: string;
  characterId: string;
  role: 'user' | 'assistant';
  content: string;
  source: string;
  createdAt: string;
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
  nfcTag: {
    uid: string;
    lastSeenAt: string;
    matched: boolean;
    characterId: string | null;
    characterName: string | null;
  } | null;
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
  kind: ReminderKind;
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
