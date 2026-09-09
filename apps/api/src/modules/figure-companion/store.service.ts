import {
  BadRequestException,
  ConflictException,
  Injectable,
  Logger,
  NotFoundException,
  OnModuleDestroy,
  OnModuleInit,
} from '@nestjs/common';
import { InjectRepository } from '@nestjs/typeorm';
import { createHash, randomUUID } from 'crypto';
import { IsNull, LessThanOrEqual, Repository } from 'typeorm';
import { Character, DeviceView, User } from './contracts';
import {
  BindDeviceDto,
  CreateReminderDto,
  DeviceMessageDto,
  DeviceSessionDto,
  HeartbeatDto,
  UpdateReminderDto,
} from './dto';
import {
  CharacterEntity,
  DeviceCommandEntity,
  DeviceEntity,
  DeviceEventEntity,
  DeviceSessionEntity,
  ReminderEntity,
  UserEntity,
} from './entities';
import { TtsService } from './tts.service';

@Injectable()
export class StoreService implements OnModuleInit, OnModuleDestroy {
  private readonly logger = new Logger(StoreService.name);
  private readonly demoUser: User = {
    id: 'user-demo',
    displayName: '体验账号',
  };
  private readonly appTokens = new Map<string, string>([
    ['demo-app-token', this.demoUser.id],
  ]);
  private reminderTimer?: NodeJS.Timeout;
  private reminderTickRunning = false;
  private readonly reminderRetryAfter = new Map<string, number>();

  constructor(
    @InjectRepository(UserEntity)
    private readonly userRepository: Repository<UserEntity>,
    @InjectRepository(CharacterEntity)
    private readonly characterRepository: Repository<CharacterEntity>,
    @InjectRepository(DeviceEntity)
    private readonly deviceRepository: Repository<DeviceEntity>,
    @InjectRepository(DeviceSessionEntity)
    private readonly sessionRepository: Repository<DeviceSessionEntity>,
    @InjectRepository(ReminderEntity)
    private readonly reminderRepository: Repository<ReminderEntity>,
    @InjectRepository(DeviceCommandEntity)
    private readonly commandRepository: Repository<DeviceCommandEntity>,
    @InjectRepository(DeviceEventEntity)
    private readonly eventRepository: Repository<DeviceEventEntity>,
    private readonly tts: TtsService,
  ) {}

  async onModuleInit() {
    await this.seedDevelopmentData();
    this.reminderTimer = setInterval(() => {
      void this.triggerDueReminders();
    }, 1000);
  }

  onModuleDestroy() {
    if (this.reminderTimer) clearInterval(this.reminderTimer);
  }

  loginDemo() {
    return { accessToken: 'demo-app-token', user: this.demoUser };
  }

  getUserForToken(token: string | null): User | null {
    if (!token) return null;
    const userId = this.appTokens.get(token);
    return userId === this.demoUser.id ? this.demoUser : null;
  }

  async getDeviceForToken(token: string | null): Promise<DeviceEntity | null> {
    if (!token) return null;
    const session = await this.sessionRepository.findOne({
      where: { tokenHash: this.hash(token) },
    });
    if (!session || session.expiresAt.getTime() <= Date.now()) return null;
    return this.deviceRepository.findOne({ where: { id: session.deviceId } });
  }

  async listCharacters(): Promise<Character[]> {
    const characters = await this.characterRepository.find({
      order: { createdAt: 'ASC' },
    });
    return characters.map((item) => this.toCharacter(item));
  }

  async listDevices(userId: string): Promise<DeviceView[]> {
    const devices = await this.deviceRepository.find({
      where: { ownerUserId: userId },
      order: { createdAt: 'ASC' },
    });
    const characters = await this.characterRepository.find();
    return devices.map((device) => this.toDeviceView(device, characters));
  }

  async getDevice(userId: string, deviceId: string): Promise<DeviceView> {
    const device = await this.requireOwnedDevice(userId, deviceId);
    return this.toDeviceView(device, await this.characterRepository.find());
  }

  async bindDevice(userId: string, dto: BindDeviceDto): Promise<DeviceView> {
    const pairingCode = dto.pairingCode.trim().toUpperCase();
    const device = await this.deviceRepository.findOne({ where: { pairingCode } });
    if (!device) throw new NotFoundException('绑定码不存在');
    if (device.ownerUserId && device.ownerUserId !== userId) {
      throw new ConflictException('该设备已经绑定其他账号');
    }
    const character = await this.requireCharacter(dto.characterId);
    device.ownerUserId = userId;
    device.characterId = character.id;
    if (dto.name?.trim()) device.name = dto.name.trim();
    await this.deviceRepository.save(device);
    await this.enqueueCommand(device.id, 'sync_character', {
      character: this.toCharacter(character),
    });
    return this.toDeviceView(device, await this.characterRepository.find());
  }

  async updateCharacter(userId: string, deviceId: string, characterId: string) {
    const [device, character] = await Promise.all([
      this.requireOwnedDevice(userId, deviceId),
      this.requireCharacter(characterId),
    ]);
    device.characterId = character.id;
    await this.deviceRepository.save(device);
    await this.enqueueCommand(device.id, 'sync_character', {
      character: this.toCharacter(character),
    });
    return this.toDeviceView(device, await this.characterRepository.find());
  }

  async updateDeviceName(userId: string, deviceId: string, name: string) {
    const nextName = name.trim();
    if (!nextName) throw new BadRequestException('设备名称不能为空');
    const device = await this.requireOwnedDevice(userId, deviceId);
    device.name = nextName;
    await this.deviceRepository.save(device);
    return this.toDeviceView(device, await this.characterRepository.find());
  }

  async updateVolume(userId: string, deviceId: string, volume: number) {
    const device = await this.requireOwnedDevice(userId, deviceId);
    device.volume = volume;
    await this.deviceRepository.save(device);
    await this.enqueueCommand(device.id, 'set_volume', { volume });
    return this.toDeviceView(device, await this.characterRepository.find());
  }

  async speakText(userId: string, deviceId: string, text: string) {
    const device = await this.requireOwnedDevice(userId, deviceId);
    const character = device.characterId
      ? await this.requireCharacter(device.characterId)
      : null;
    const speech = await this.tts.synthesize(text, character?.voiceId);
    return this.enqueueCommand(device.id, 'speak_text', {
      text,
      characterId: character?.id ?? null,
      voiceId: speech.voice,
      audioPath: speech.audioPath,
      audioFormat: speech.format,
      sampleRate: speech.sampleRate,
      provider: speech.provider,
    });
  }

  async listDeviceCommands(userId: string, deviceId: string) {
    await this.requireOwnedDevice(userId, deviceId);
    return this.commandRepository.find({
      where: { deviceId },
      order: { createdAt: 'DESC' },
      take: 20,
    });
  }

  async listDeviceEvents(userId: string, deviceId: string) {
    await this.requireOwnedDevice(userId, deviceId);
    return this.eventRepository.find({
      where: { deviceId },
      order: { createdAt: 'DESC' },
      take: 20,
    });
  }

  listReminders(userId: string, deviceId?: string) {
    return this.reminderRepository.find({
      where: deviceId ? { userId, deviceId } : { userId },
      order: { scheduledAt: 'ASC' },
    });
  }

  async createReminder(userId: string, dto: CreateReminderDto) {
    await this.requireOwnedDevice(userId, dto.deviceId);
    const scheduledAt = new Date(dto.scheduledAt);
    if (Number.isNaN(scheduledAt.getTime())) {
      throw new BadRequestException('提醒时间格式不正确');
    }
    return this.reminderRepository.save(
      this.reminderRepository.create({
        userId,
        deviceId: dto.deviceId,
        title: dto.title.trim(),
        scheduledAt,
        repeat: dto.repeat ?? 'none',
        enabled: true,
        lastTriggeredAt: null,
      }),
    );
  }

  async updateReminder(userId: string, reminderId: string, dto: UpdateReminderDto) {
    const reminder = await this.requireReminder(userId, reminderId);
    if (dto.title !== undefined) reminder.title = dto.title.trim();
    if (dto.scheduledAt !== undefined) {
      reminder.scheduledAt = new Date(dto.scheduledAt);
      reminder.lastTriggeredAt = null;
    }
    if (dto.repeat !== undefined) reminder.repeat = dto.repeat;
    if (dto.enabled !== undefined) reminder.enabled = dto.enabled;
    return this.reminderRepository.save(reminder);
  }

  async deleteReminder(userId: string, reminderId: string) {
    const reminder = await this.requireReminder(userId, reminderId);
    await this.reminderRepository.remove(reminder);
    return { deleted: true };
  }

  async createDeviceSession(dto: DeviceSessionDto) {
    const device = await this.deviceRepository.findOne({
      where: { hardwareId: dto.hardwareId },
    });
    if (!device || device.deviceSecretHash !== this.hash(dto.deviceSecret)) {
      throw new NotFoundException('未登记的硬件或密钥错误');
    }
    await this.sessionRepository.delete({ expiresAt: LessThanOrEqual(new Date()) });
    const token = randomUUID();
    await this.sessionRepository.save(
      this.sessionRepository.create({
        tokenHash: this.hash(token),
        deviceId: device.id,
        expiresAt: new Date(Date.now() + 86_400_000),
      }),
    );
    return {
      accessToken: token,
      expiresInSeconds: 86400,
      device: {
        id: device.id,
        pairingCode: device.pairingCode,
        bound: Boolean(device.ownerUserId),
      },
    };
  }

  async heartbeat(device: DeviceEntity, dto: HeartbeatDto) {
    device.lastSeenAt = new Date();
    if (dto.firmwareVersion) device.firmwareVersion = dto.firmwareVersion;
    if (dto.volume !== undefined) device.volume = dto.volume;
    await this.deviceRepository.save(device);
    return {
      serverTime: new Date().toISOString(),
      bound: Boolean(device.ownerUserId),
      characterId: device.characterId,
      pendingCommandCount: (await this.getPendingCommands(device)).length,
    };
  }

  getPendingCommands(device: DeviceEntity) {
    return this.commandRepository.find({
      where: { deviceId: device.id, acknowledgedAt: IsNull() },
      order: { createdAt: 'ASC' },
      take: 5,
    });
  }

  async acknowledgeCommand(device: DeviceEntity, commandId: string) {
    const command = await this.commandRepository.findOne({
      where: { id: commandId, deviceId: device.id },
    });
    if (!command) throw new NotFoundException('设备指令不存在');
    command.acknowledgedAt = new Date();
    await this.commandRepository.save(command);
    return { acknowledged: true };
  }

  async receiveDeviceEvent(
    device: DeviceEntity,
    type: string,
    payload: Record<string, unknown> = {},
  ) {
    device.lastSeenAt = new Date();
    const event = this.eventRepository.create({ deviceId: device.id, type, payload });
    const [, savedEvent] = await Promise.all([
      this.deviceRepository.save(device),
      this.eventRepository.save(event),
    ]);
    return {
      accepted: true,
      event: { type, payload, receivedAt: savedEvent.createdAt },
    };
  }

  async replyToDeviceMessage(device: DeviceEntity, dto: DeviceMessageDto) {
    device.lastSeenAt = new Date();
    await this.deviceRepository.save(device);
    const character = device.characterId
      ? await this.requireCharacter(device.characterId)
      : null;
    const reply = character
      ? `${character.name}已收到：“${dto.text}”。目前这是联调回复，下一阶段会接入正式 AI 对话和阿里云角色音色。`
      : `设备尚未绑定角色。收到：“${dto.text}”。`;
    return {
      conversationId: `conversation-${device.id}`,
      text: reply,
      characterId: character?.id ?? null,
      voiceId: character?.voiceId ?? null,
      audioUrl: null,
      provider: 'mock',
    };
  }

  private enqueueCommand(
    deviceId: string,
    type: DeviceCommandEntity['type'],
    payload: Record<string, unknown>,
  ) {
    return this.commandRepository.save(
      this.commandRepository.create({
        deviceId,
        type,
        payload,
        acknowledgedAt: null,
      }),
    );
  }

  private async triggerDueReminders() {
    if (this.reminderTickRunning) return;
    this.reminderTickRunning = true;
    try {
      const now = new Date();
      const reminders = await this.reminderRepository.find({
        where: { enabled: true, scheduledAt: LessThanOrEqual(now) },
      });
      for (const reminder of reminders) {
        if ((this.reminderRetryAfter.get(reminder.id) ?? 0) > Date.now()) continue;
        const device = await this.deviceRepository.findOne({
          where: { id: reminder.deviceId },
        });
        if (!device) continue;
        const character = device.characterId
          ? await this.characterRepository.findOne({ where: { id: device.characterId } })
          : null;
        try {
          const speech = await this.tts.synthesize(
            `提醒时间到了，${reminder.title}`,
            character?.voiceId,
          );
          await this.enqueueCommand(device.id, 'play_reminder', {
            reminderId: reminder.id,
            title: reminder.title,
            characterId: character?.id ?? null,
            voiceId: speech.voice,
            audioPath: speech.audioPath,
            audioFormat: speech.format,
            sampleRate: speech.sampleRate,
            provider: speech.provider,
          });
          reminder.lastTriggeredAt = now;
          if (reminder.repeat === 'daily') {
            const next = new Date(reminder.scheduledAt);
            do next.setUTCDate(next.getUTCDate() + 1);
            while (next.getTime() <= now.getTime());
            reminder.scheduledAt = next;
          } else {
            reminder.enabled = false;
          }
          await this.reminderRepository.save(reminder);
          this.reminderRetryAfter.delete(reminder.id);
        } catch (error) {
          this.reminderRetryAfter.set(reminder.id, Date.now() + 30_000);
          this.logger.error(
            `Reminder TTS failed reminder=${reminder.id}; retrying in 30 seconds: ${
              error instanceof Error ? error.message : 'unknown error'
            }`,
          );
        }
      }
    } finally {
      this.reminderTickRunning = false;
    }
  }

  private async requireCharacter(characterId: string) {
    const character = await this.characterRepository.findOne({ where: { id: characterId } });
    if (!character) throw new NotFoundException('角色不存在');
    return character;
  }

  private async requireOwnedDevice(userId: string, deviceId: string) {
    const device = await this.deviceRepository.findOne({
      where: { id: deviceId, ownerUserId: userId },
    });
    if (!device) throw new NotFoundException('设备不存在或尚未绑定');
    return device;
  }

  private async requireReminder(userId: string, reminderId: string) {
    const reminder = await this.reminderRepository.findOne({
      where: { id: reminderId, userId },
    });
    if (!reminder) throw new NotFoundException('提醒不存在');
    return reminder;
  }

  private toCharacter(character: CharacterEntity): Character {
    return {
      id: character.id,
      name: character.name,
      description: character.description,
      accentColor: character.accentColor,
      voiceId: character.voiceId,
      greeting: character.greeting,
    };
  }

  private toDeviceView(device: DeviceEntity, characters: CharacterEntity[]): DeviceView {
    const onlineWindowSeconds = Number(
      process.env.DEVICE_ONLINE_WINDOW_SECONDS ?? 45,
    );
    const isOnline =
      device.lastSeenAt !== null &&
      Date.now() - device.lastSeenAt.getTime() < onlineWindowSeconds * 1000;
    const character = characters.find((item) => item.id === device.characterId) ?? null;
    return {
      id: device.id,
      hardwareId: device.hardwareId,
      name: device.name,
      firmwareVersion: device.firmwareVersion,
      characterId: device.characterId,
      character: character ? this.toCharacter(character) : null,
      status: isOnline ? 'online' : 'offline',
      lastSeenAt: device.lastSeenAt?.toISOString() ?? null,
      volume: device.volume,
    };
  }

  private async seedDevelopmentData() {
    await this.userRepository.upsert(
      { id: this.demoUser.id, displayName: this.demoUser.displayName },
      ['id'],
    );
    const characterSeeds: Array<Partial<CharacterEntity> & Pick<CharacterEntity, 'id'>> = [
      {
        id: 'character-xiaoyu',
        name: '小羽',
        description: '温柔、活泼，适合作为日常陪伴角色。',
        accentColor: '#8B5CF6',
        voiceId: 'aliyun-voice-placeholder-xiaoyu',
        greeting: '你回来啦，今天过得怎么样？',
      },
      {
        id: 'character-captain',
        name: '队长',
        description: '干练可靠，适合提醒、计划和早起任务。',
        accentColor: '#0EA5E9',
        voiceId: 'aliyun-voice-placeholder-captain',
        greeting: '状态确认完毕，今天也一起完成计划吧。',
      },
      {
        id: 'character-momo',
        name: '默默',
        description: '安静治愈，偏简短、不打扰的陪伴方式。',
        accentColor: '#F97316',
        voiceId: 'aliyun-voice-placeholder-momo',
        greeting: '我在这里，想说什么都可以。',
      },
    ];
    for (const seed of characterSeeds) {
      const exists = await this.characterRepository.exist({ where: { id: seed.id } });
      if (!exists) {
        await this.characterRepository.save(this.characterRepository.create(seed));
      }
    }
    const deviceExists = await this.deviceRepository.exist({
      where: { hardwareId: 'ESP32S3-DEMO-001' },
    });
    if (!deviceExists) {
      await this.deviceRepository.save(
        this.deviceRepository.create({
          hardwareId: 'ESP32S3-DEMO-001',
          deviceSecretHash: this.hash('figure-dev-secret-001'),
          pairingCode: 'FIGURE-0001',
          name: '我的手办底座',
          firmwareVersion: 'simulator-0.1.0',
          ownerUserId: null,
          characterId: null,
          lastSeenAt: null,
          volume: 60,
        }),
      );
    }
  }

  private hash(value: string): string {
    return createHash('sha256').update(value).digest('hex');
  }
}
