import { Type } from 'class-transformer';
import {
  IsBoolean,
  IsIn,
  IsInt,
  IsISO8601,
  IsNotEmpty,
  IsObject,
  IsOptional,
  IsString,
  Max,
  MaxLength,
  Min,
} from 'class-validator';
import { ReminderRepeat } from './contracts';

export class BindDeviceDto {
  @IsString()
  @IsNotEmpty()
  pairingCode: string;

  @IsString()
  @IsNotEmpty()
  characterId: string;

  @IsOptional()
  @IsString()
  @MaxLength(40)
  name?: string;
}

export class UpdateCharacterDto {
  @IsString()
  @IsNotEmpty()
  characterId: string;
}

export class UpdateDeviceNameDto {
  @IsString()
  @IsNotEmpty()
  @MaxLength(40)
  name: string;
}

export class UpdateVolumeDto {
  @Type(() => Number)
  @IsInt()
  @Min(0)
  @Max(100)
  volume: number;
}

export class CreateReminderDto {
  @IsString()
  @IsNotEmpty()
  deviceId: string;

  @IsString()
  @IsNotEmpty()
  @MaxLength(80)
  title: string;

  @IsISO8601()
  scheduledAt: string;

  @IsOptional()
  @IsIn(['none', 'daily'])
  repeat?: ReminderRepeat;
}

export class UpdateReminderDto {
  @IsOptional()
  @IsString()
  @MaxLength(80)
  title?: string;

  @IsOptional()
  @IsISO8601()
  scheduledAt?: string;

  @IsOptional()
  @IsIn(['none', 'daily'])
  repeat?: ReminderRepeat;

  @IsOptional()
  @IsBoolean()
  enabled?: boolean;
}

export class DeviceSessionDto {
  @IsString()
  @IsNotEmpty()
  hardwareId: string;

  @IsString()
  @IsNotEmpty()
  deviceSecret: string;
}

export class HeartbeatDto {
  @IsOptional()
  @IsString()
  firmwareVersion?: string;

  @IsOptional()
  @Type(() => Number)
  @IsInt()
  @Min(0)
  @Max(100)
  volume?: number;
}

export class DeviceEventDto {
  @IsString()
  @IsNotEmpty()
  type: string;

  @IsOptional()
  @IsObject()
  payload?: Record<string, unknown>;
}

export class DeviceMessageDto {
  @IsString()
  @IsNotEmpty()
  @MaxLength(2000)
  text: string;
}

export class SpeakTextDto {
  @IsString()
  @IsNotEmpty()
  @MaxLength(500)
  text: string;
}
