import {
  Column,
  CreateDateColumn,
  Entity,
  Index,
  PrimaryColumn,
  PrimaryGeneratedColumn,
  UpdateDateColumn,
} from 'typeorm';
import { CommandType, ReminderRepeat } from './contracts';

@Entity('figure_users')
export class UserEntity {
  @PrimaryColumn({ type: 'varchar', length: 64 })
  id: string;

  @Column({ type: 'varchar', length: 80 })
  displayName: string;

  @CreateDateColumn({ type: 'datetime', precision: 3 })
  createdAt: Date;
}

@Entity('figure_characters')
export class CharacterEntity {
  @PrimaryColumn({ type: 'varchar', length: 64 })
  id: string;

  @Column({ type: 'varchar', length: 80 })
  name: string;

  @Column({ type: 'varchar', length: 500 })
  description: string;

  @Column({ type: 'varchar', length: 16 })
  accentColor: string;

  @Column({ type: 'varchar', length: 160 })
  voiceId: string;

  @Column({ type: 'varchar', length: 500 })
  greeting: string;

  @CreateDateColumn({ type: 'datetime', precision: 3 })
  createdAt: Date;

  @UpdateDateColumn({ type: 'datetime', precision: 3 })
  updatedAt: Date;
}

@Entity('figure_devices')
export class DeviceEntity {
  @PrimaryGeneratedColumn('uuid')
  id: string;

  @Index({ unique: true })
  @Column({ type: 'varchar', length: 100 })
  hardwareId: string;

  @Column({ type: 'char', length: 64 })
  deviceSecretHash: string;

  @Index({ unique: true })
  @Column({ type: 'varchar', length: 32 })
  pairingCode: string;

  @Column({ type: 'varchar', length: 80 })
  name: string;

  @Column({ type: 'varchar', length: 64, default: 'unknown' })
  firmwareVersion: string;

  @Index()
  @Column({ type: 'varchar', length: 64, nullable: true })
  ownerUserId: string | null;

  @Column({ type: 'varchar', length: 64, nullable: true })
  characterId: string | null;

  @Column({ type: 'datetime', precision: 3, nullable: true })
  lastSeenAt: Date | null;

  @Column({ type: 'tinyint', unsigned: true, default: 60 })
  volume: number;

  @CreateDateColumn({ type: 'datetime', precision: 3 })
  createdAt: Date;

  @UpdateDateColumn({ type: 'datetime', precision: 3 })
  updatedAt: Date;
}

@Entity('figure_device_sessions')
export class DeviceSessionEntity {
  @PrimaryGeneratedColumn('uuid')
  id: string;

  @Index({ unique: true })
  @Column({ type: 'char', length: 64 })
  tokenHash: string;

  @Index()
  @Column({ type: 'char', length: 36 })
  deviceId: string;

  @Column({ type: 'datetime', precision: 3 })
  expiresAt: Date;

  @CreateDateColumn({ type: 'datetime', precision: 3 })
  createdAt: Date;
}

@Entity('figure_reminders')
export class ReminderEntity {
  @PrimaryGeneratedColumn('uuid')
  id: string;

  @Index()
  @Column({ type: 'varchar', length: 64 })
  userId: string;

  @Index()
  @Column({ type: 'char', length: 36 })
  deviceId: string;

  @Column({ type: 'varchar', length: 80 })
  title: string;

  @Index()
  @Column({ type: 'datetime', precision: 3 })
  scheduledAt: Date;

  @Column({ type: 'enum', enum: ['none', 'daily'], default: 'none' })
  repeat: ReminderRepeat;

  @Index()
  @Column({ type: 'boolean', default: true })
  enabled: boolean;

  @Column({ type: 'datetime', precision: 3, nullable: true })
  lastTriggeredAt: Date | null;

  @CreateDateColumn({ type: 'datetime', precision: 3 })
  createdAt: Date;

  @UpdateDateColumn({ type: 'datetime', precision: 3 })
  updatedAt: Date;
}

@Entity('figure_device_commands')
export class DeviceCommandEntity {
  @PrimaryGeneratedColumn('uuid')
  id: string;

  @Index()
  @Column({ type: 'char', length: 36 })
  deviceId: string;

  @Column({
    type: 'enum',
    enum: ['sync_character', 'play_reminder', 'speak_text', 'set_volume'],
  })
  type: CommandType;

  // MariaDB 10.1 has no native JSON column. TypeORM serializes this to text.
  @Column({ type: 'simple-json' })
  payload: Record<string, unknown>;

  @Column({ type: 'datetime', precision: 3, nullable: true })
  acknowledgedAt: Date | null;

  @CreateDateColumn({ type: 'datetime', precision: 3 })
  createdAt: Date;
}

@Entity('figure_device_events')
export class DeviceEventEntity {
  @PrimaryGeneratedColumn('uuid')
  id: string;

  @Index()
  @Column({ type: 'char', length: 36 })
  deviceId: string;

  @Index()
  @Column({ type: 'varchar', length: 80 })
  type: string;

  @Column({ type: 'simple-json' })
  payload: Record<string, unknown>;

  @CreateDateColumn({ type: 'datetime', precision: 3 })
  createdAt: Date;
}

export const databaseEntities = [
  UserEntity,
  CharacterEntity,
  DeviceEntity,
  DeviceSessionEntity,
  ReminderEntity,
  DeviceCommandEntity,
  DeviceEventEntity,
];
