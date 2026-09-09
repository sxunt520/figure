import { StatusBar } from 'expo-status-bar';
import React, { useCallback, useEffect, useMemo, useState } from 'react';
import {
  ActivityIndicator,
  Alert,
  Pressable,
  SafeAreaView,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { API_BASE_URL, api } from './src/api';
import {
  Character,
  Device,
  DeviceCommand,
  DeviceEvent,
  Reminder,
  User,
} from './src/types';

type Tab = 'home' | 'bind' | 'reminders';

const palette = {
  ink: '#211A2E',
  muted: '#766E83',
  primary: '#7656E8',
  primarySoft: '#EEE9FF',
  surface: '#FFFFFF',
  canvas: '#F7F5FC',
  border: '#E8E3F0',
  green: '#27A56B',
  red: '#D95555',
};

export default function App() {
  const [tab, setTab] = useState<Tab>('home');
  const [token, setToken] = useState('');
  const [user, setUser] = useState<User | null>(null);
  const [characters, setCharacters] = useState<Character[]>([]);
  const [devices, setDevices] = useState<Device[]>([]);
  const [reminders, setReminders] = useState<Reminder[]>([]);
  const [commands, setCommands] = useState<DeviceCommand[]>([]);
  const [events, setEvents] = useState<DeviceEvent[]>([]);
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState('');

  const device = devices[0] ?? null;

  const refresh = useCallback(
    async (activeToken = token) => {
      if (!activeToken) return;
      const [nextCharacters, nextDevices, nextReminders] = await Promise.all([
        api.listCharacters(activeToken),
        api.listDevices(activeToken),
        api.listReminders(activeToken),
      ]);
      setCharacters(nextCharacters);
      setDevices(nextDevices);
      setReminders(nextReminders);

      const firstDevice = nextDevices[0] ?? null;
      if (firstDevice) {
        const [nextCommands, nextEvents] = await Promise.all([
          api.listDeviceCommands(activeToken, firstDevice.id),
          api.listDeviceEvents(activeToken, firstDevice.id),
        ]);
        setCommands(nextCommands);
        setEvents(nextEvents);
      } else {
        setCommands([]);
        setEvents([]);
      }
    },
    [token],
  );

  useEffect(() => {
    void (async () => {
      try {
        const session = await api.loginDemo();
        setToken(session.accessToken);
        setUser(session.user);
        await refresh(session.accessToken);
      } catch (caught) {
        setError(caught instanceof Error ? caught.message : '初始化失败');
      } finally {
        setBusy(false);
      }
    })();
  }, []);

  useEffect(() => {
    if (!token) return;
    const timer = setInterval(() => {
      void refresh().catch(() => undefined);
    }, 5000);
    return () => clearInterval(timer);
  }, [refresh, token]);

  async function perform(action: () => Promise<unknown>, message?: string) {
    try {
      setBusy(true);
      setError('');
      await action();
      await refresh();
      if (message) Alert.alert('完成', message);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : '操作失败');
    } finally {
      setBusy(false);
    }
  }

  if (busy && !user) {
    return (
      <SafeAreaView style={styles.centered}>
        <ActivityIndicator color={palette.primary} size="large" />
        <Text style={styles.loadingText}>正在连接手办伙伴服务…</Text>
      </SafeAreaView>
    );
  }

  return (
    <SafeAreaView style={styles.safeArea}>
      <StatusBar style="dark" />
      <View style={styles.header}>
        <View>
          <Text style={styles.eyebrow}>FIGURE COMPANION</Text>
          <Text style={styles.title}>你好，{user?.displayName ?? '朋友'}</Text>
        </View>
        {busy ? <ActivityIndicator color={palette.primary} /> : null}
      </View>

      {error ? (
        <Pressable style={styles.errorBanner} onPress={() => setError('')}>
          <Text style={styles.errorText}>{error}</Text>
        </Pressable>
      ) : null}

      <ScrollView contentContainerStyle={styles.content}>
        {tab === 'home' ? (
          <HomeScreen
            token={token}
            device={device}
            characters={characters}
            commands={commands}
            events={events}
            onAction={perform}
            onNeedBind={() => setTab('bind')}
          />
        ) : null}
        {tab === 'bind' ? (
          <BindScreen
            token={token}
            device={device}
            characters={characters}
            onAction={perform}
            onBound={() => setTab('home')}
          />
        ) : null}
        {tab === 'reminders' ? (
          <ReminderScreen
            token={token}
            device={device}
            reminders={reminders}
            onAction={perform}
          />
        ) : null}

        <Text style={styles.endpoint}>API：{API_BASE_URL}</Text>
      </ScrollView>

      <View style={styles.tabBar}>
        <TabButton active={tab === 'home'} label="设备" onPress={() => setTab('home')} />
        <TabButton active={tab === 'bind'} label="绑定" onPress={() => setTab('bind')} />
        <TabButton
          active={tab === 'reminders'}
          label="提醒"
          onPress={() => setTab('reminders')}
        />
      </View>
    </SafeAreaView>
  );
}

function HomeScreen({
  token,
  device,
  characters,
  commands,
  events,
  onAction,
  onNeedBind,
}: {
  token: string;
  device: Device | null;
  characters: Character[];
  commands: DeviceCommand[];
  events: DeviceEvent[];
  onAction: (action: () => Promise<unknown>, message?: string) => Promise<void>;
  onNeedBind: () => void;
}) {
  const [speech, setSpeech] = useState('该起床啦，今天也要元气满满！');
  const [nameDraft, setNameDraft] = useState(device?.name ?? '');
  const lastSeenText = formatLastSeen(device?.lastSeenAt ?? null);

  useEffect(() => {
    setNameDraft(device?.name ?? '');
  }, [device?.id, device?.name]);

  if (!device) {
    return (
      <EmptyCard
        title="还没有绑定底座"
        body="硬件到达前可以使用模拟设备。测试绑定码是 FIGURE-0001。"
        action="开始绑定"
        onPress={onNeedBind}
      />
    );
  }

  return (
    <>
      <View style={styles.heroCard}>
        <View style={styles.rowBetween}>
          <View style={styles.deviceIcon}><Text style={styles.deviceIconText}>◉</Text></View>
          <View
            style={[
              styles.statusPill,
              device.status === 'online' ? styles.onlinePill : styles.offlinePill,
            ]}
          >
            <Text style={styles.statusText}>
              {device.status === 'online' ? '在线' : '离线'}
            </Text>
          </View>
        </View>
        <Text style={styles.cardTitle}>{device.name}</Text>
        <Text style={styles.cardBody}>
          {device.character
            ? `当前角色：${device.character.name} · 音量 ${device.volume}%`
            : '尚未选择角色'}
        </Text>
        <View style={styles.deviceFacts}>
          <Text style={styles.deviceFact}>硬件：{device.hardwareId}</Text>
          <Text style={styles.deviceFact}>固件：{device.firmwareVersion}</Text>
          <Text style={styles.deviceFact}>最近心跳：{lastSeenText}</Text>
        </View>
      </View>

      <SectionTitle title="设备设置" subtitle="先把 APP 控制链路跑顺" />
      <View style={styles.panel}>
        <FieldLabel label="设备名称" />
        <View style={styles.inlineForm}>
          <TextInput
            style={[styles.input, styles.inlineInput]}
            value={nameDraft}
            onChangeText={setNameDraft}
            placeholder="给底座起个名字"
          />
          <Pressable
            style={styles.compactButton}
            onPress={() =>
              void onAction(
                () => api.updateDeviceName(token, device.id, nameDraft.trim()),
                '设备名称已更新',
              )
            }
          >
            <Text style={styles.compactButtonText}>保存</Text>
          </Pressable>
        </View>
      </View>

      <SectionTitle title="切换角色" subtitle="选择后会生成一条同步指令发送给底座" />
      <View style={styles.stack}>
        {characters.map((character) => (
          <CharacterCard
            key={character.id}
            character={character}
            selected={device.characterId === character.id}
            onPress={() =>
              void onAction(() =>
                api.updateCharacter(token, device.id, character.id),
              )
            }
          />
        ))}
      </View>

      <SectionTitle title="让角色说一句" subtitle="用于验证 APP → 后端 → 设备联动" />
      <View style={styles.panel}>
        <TextInput
          style={styles.input}
          value={speech}
          onChangeText={setSpeech}
          multiline
          placeholder="输入想让角色说的话"
        />
        <PrimaryButton
          label="发送到手办底座"
          disabled={!speech.trim()}
          onPress={() =>
            void onAction(
              () => api.speakText(token, device.id, speech.trim()),
              '播报指令已经进入设备队列',
            )
          }
        />
      </View>

      <SectionTitle title="音量" />
      <View style={[styles.panel, styles.volumeRow]}>
        <SecondaryButton
          label="－"
          onPress={() =>
            void onAction(() =>
              api.updateVolume(token, device.id, Math.max(0, device.volume - 10)),
            )
          }
        />
        <Text style={styles.volumeText}>{device.volume}%</Text>
        <SecondaryButton
          label="＋"
          onPress={() =>
            void onAction(() =>
              api.updateVolume(token, device.id, Math.min(100, device.volume + 10)),
            )
          }
        />
      </View>

      <SectionTitle
        title="设备指令队列"
        subtitle="最近 20 条，已完成代表 ESP32 已 ACK"
      />
      <View style={styles.stack}>
        {commands.length === 0 ? (
          <Text style={styles.emptyText}>暂无指令</Text>
        ) : (
          commands.slice(0, 6).map((command) => (
            <TimelineCard
              key={command.id}
              title={formatCommandType(command.type)}
              subtitle={`${formatCommandPayload(command.payload)} · ${new Date(
                command.createdAt,
              ).toLocaleTimeString()}`}
              status={command.acknowledgedAt ? '已完成' : '等待设备'}
              tone={command.acknowledgedAt ? 'success' : 'pending'}
            />
          ))
        )}
      </View>

      <SectionTitle title="最近设备事件" subtitle="ESP32 主动上报的日志" />
      <View style={styles.stack}>
        {events.length === 0 ? (
          <Text style={styles.emptyText}>暂无事件</Text>
        ) : (
          events.slice(0, 6).map((event) => (
            <TimelineCard
              key={event.id}
              title={event.type}
              subtitle={`${formatEventPayload(event.payload)} · ${new Date(
                event.createdAt,
              ).toLocaleTimeString()}`}
              status="已接收"
              tone="muted"
            />
          ))
        )}
      </View>
    </>
  );
}

function BindScreen({
  token,
  device,
  characters,
  onAction,
  onBound,
}: {
  token: string;
  device: Device | null;
  characters: Character[];
  onAction: (action: () => Promise<unknown>, message?: string) => Promise<void>;
  onBound: () => void;
}) {
  const [pairingCode, setPairingCode] = useState('FIGURE-0001');
  const [name, setName] = useState('我的手办底座');
  const [characterId, setCharacterId] = useState(characters[0]?.id ?? '');

  useEffect(() => {
    if (!characterId && characters[0]) setCharacterId(characters[0].id);
  }, [characters, characterId]);

  if (device) {
    return (
      <EmptyCard
        title="设备已经绑定"
        body={`${device.name} 已属于当前账号。第一版暂不提供解绑，避免误操作。`}
        action="查看设备"
        onPress={onBound}
      />
    );
  }

  return (
    <View style={styles.panel}>
      <Text style={styles.formTitle}>绑定手办底座</Text>
      <Text style={styles.formHelp}>
        正式硬件会在屏幕上显示短码。现在可以用预置模拟码完成测试。
      </Text>
      <FieldLabel label="绑定码" />
      <TextInput
        autoCapitalize="characters"
        style={styles.input}
        value={pairingCode}
        onChangeText={setPairingCode}
      />
      <FieldLabel label="设备名称" />
      <TextInput style={styles.input} value={name} onChangeText={setName} />
      <FieldLabel label="初始角色" />
      <View style={styles.stack}>
        {characters.map((character) => (
          <CharacterCard
            key={character.id}
            character={character}
            selected={characterId === character.id}
            onPress={() => setCharacterId(character.id)}
          />
        ))}
      </View>
      <PrimaryButton
        label="确认绑定"
        disabled={!pairingCode.trim() || !characterId}
        onPress={() =>
          void onAction(async () => {
            await api.bindDevice(token, {
              pairingCode: pairingCode.trim(),
              characterId,
              name: name.trim(),
            });
            onBound();
          }, '账号、设备和角色已经关联')
        }
      />
    </View>
  );
}

function ReminderScreen({
  token,
  device,
  reminders,
  onAction,
}: {
  token: string;
  device: Device | null;
  reminders: Reminder[];
  onAction: (action: () => Promise<unknown>, message?: string) => Promise<void>;
}) {
  const [title, setTitle] = useState('喝水休息一下');
  const [delayMinutes, setDelayMinutes] = useState(1);
  const [repeat, setRepeat] = useState<'none' | 'daily'>('none');
  const ordered = useMemo(
    () => [...reminders].sort((a, b) => a.scheduledAt.localeCompare(b.scheduledAt)),
    [reminders],
  );

  if (!device) {
    return (
      <EmptyCard
        title="请先绑定设备"
        body="提醒必须关联到一台手办底座。"
        action="暂无设备"
      />
    );
  }

  return (
    <>
      <View style={styles.panel}>
        <Text style={styles.formTitle}>创建语音提醒</Text>
        <FieldLabel label="提醒内容" />
        <TextInput style={styles.input} value={title} onChangeText={setTitle} />
        <FieldLabel label="测试触发时间" />
        <View style={styles.chipRow}>
          {[1, 5, 10].map((minutes) => (
            <Chip
              key={minutes}
              label={`${minutes} 分钟后`}
              selected={delayMinutes === minutes}
              onPress={() => setDelayMinutes(minutes)}
            />
          ))}
        </View>
        <FieldLabel label="重复" />
        <View style={styles.chipRow}>
          <Chip label="仅一次" selected={repeat === 'none'} onPress={() => setRepeat('none')} />
          <Chip label="每天" selected={repeat === 'daily'} onPress={() => setRepeat('daily')} />
        </View>
        <PrimaryButton
          label="保存提醒"
          disabled={!title.trim()}
          onPress={() =>
            void onAction(
              () =>
                api.createReminder(token, {
                  deviceId: device.id,
                  title: title.trim(),
                  scheduledAt: new Date(Date.now() + delayMinutes * 60_000).toISOString(),
                  repeat,
                }),
              '提醒已经创建',
            )
          }
        />
      </View>

      <SectionTitle title="提醒列表" subtitle={`${ordered.length} 条`} />
      <View style={styles.stack}>
        {ordered.length === 0 ? (
          <Text style={styles.emptyText}>还没有提醒</Text>
        ) : (
          ordered.map((reminder) => (
            <View key={reminder.id} style={styles.reminderCard}>
              <View style={styles.reminderContent}>
                <Text style={styles.reminderTitle}>{reminder.title}</Text>
                <Text style={styles.meta}>
                  {new Date(reminder.scheduledAt).toLocaleString()} ·{' '}
                  {reminder.repeat === 'daily' ? '每天' : '单次'}
                </Text>
              </View>
              <Pressable
                style={[styles.smallButton, !reminder.enabled && styles.smallButtonMuted]}
                onPress={() =>
                  void onAction(() =>
                    api.toggleReminder(token, reminder.id, !reminder.enabled),
                  )
                }
              >
                <Text style={styles.smallButtonText}>{reminder.enabled ? '暂停' : '启用'}</Text>
              </Pressable>
              <Pressable
                style={styles.deleteButton}
                onPress={() =>
                  Alert.alert('删除提醒', `确定删除“${reminder.title}”吗？`, [
                    { text: '取消', style: 'cancel' },
                    {
                      text: '删除',
                      style: 'destructive',
                      onPress: () =>
                        void onAction(() => api.deleteReminder(token, reminder.id)),
                    },
                  ])
                }
              >
                <Text style={styles.deleteText}>删除</Text>
              </Pressable>
            </View>
          ))
        )}
      </View>
    </>
  );
}

function CharacterCard({
  character,
  selected,
  onPress,
}: {
  character: Character;
  selected: boolean;
  onPress: () => void;
}) {
  return (
    <Pressable
      onPress={onPress}
      style={[styles.characterCard, selected && styles.characterCardSelected]}
    >
      <View style={[styles.avatar, { backgroundColor: character.accentColor }]}>
        <Text style={styles.avatarText}>{character.name.slice(0, 1)}</Text>
      </View>
      <View style={styles.characterContent}>
        <View style={styles.rowBetween}>
          <Text style={styles.characterName}>{character.name}</Text>
          {selected ? <Text style={styles.selectedMark}>已选择</Text> : null}
        </View>
        <Text style={styles.characterDescription}>{character.description}</Text>
      </View>
    </Pressable>
  );
}

function EmptyCard({
  title,
  body,
  action,
  onPress,
}: {
  title: string;
  body: string;
  action: string;
  onPress?: () => void;
}) {
  return (
    <View style={styles.emptyCard}>
      <Text style={styles.emptySymbol}>⌁</Text>
      <Text style={styles.cardTitle}>{title}</Text>
      <Text style={[styles.cardBody, styles.emptyBody]}>{body}</Text>
      {onPress ? <PrimaryButton label={action} onPress={onPress} /> : null}
    </View>
  );
}

function SectionTitle({ title, subtitle }: { title: string; subtitle?: string }) {
  return (
    <View style={styles.sectionHeader}>
      <Text style={styles.sectionTitle}>{title}</Text>
      {subtitle ? <Text style={styles.sectionSubtitle}>{subtitle}</Text> : null}
    </View>
  );
}

function FieldLabel({ label }: { label: string }) {
  return <Text style={styles.fieldLabel}>{label}</Text>;
}

function PrimaryButton({
  label,
  onPress,
  disabled = false,
}: {
  label: string;
  onPress: () => void;
  disabled?: boolean;
}) {
  return (
    <Pressable
      disabled={disabled}
      onPress={onPress}
      style={[styles.primaryButton, disabled && styles.buttonDisabled]}
    >
      <Text style={styles.primaryButtonText}>{label}</Text>
    </Pressable>
  );
}

function SecondaryButton({ label, onPress }: { label: string; onPress: () => void }) {
  return (
    <Pressable onPress={onPress} style={styles.secondaryButton}>
      <Text style={styles.secondaryButtonText}>{label}</Text>
    </Pressable>
  );
}

function Chip({
  label,
  selected,
  onPress,
}: {
  label: string;
  selected: boolean;
  onPress: () => void;
}) {
  return (
    <Pressable onPress={onPress} style={[styles.chip, selected && styles.chipSelected]}>
      <Text style={[styles.chipText, selected && styles.chipTextSelected]}>{label}</Text>
    </Pressable>
  );
}

function TabButton({
  active,
  label,
  onPress,
}: {
  active: boolean;
  label: string;
  onPress: () => void;
}) {
  return (
    <Pressable onPress={onPress} style={styles.tabButton}>
      <View style={[styles.tabDot, active && styles.tabDotActive]} />
      <Text style={[styles.tabLabel, active && styles.tabLabelActive]}>{label}</Text>
    </Pressable>
  );
}

function TimelineCard({
  title,
  subtitle,
  status,
  tone,
}: {
  title: string;
  subtitle: string;
  status: string;
  tone: 'success' | 'pending' | 'muted';
}) {
  return (
    <View style={styles.timelineCard}>
      <View style={styles.reminderContent}>
        <Text style={styles.reminderTitle}>{title}</Text>
        <Text style={styles.meta}>{subtitle}</Text>
      </View>
      <View
        style={[
          styles.stateBadge,
          tone === 'success' && styles.stateBadgeSuccess,
          tone === 'pending' && styles.stateBadgePending,
        ]}
      >
        <Text
          style={[
            styles.stateBadgeText,
            tone === 'success' && styles.stateBadgeTextSuccess,
            tone === 'pending' && styles.stateBadgeTextPending,
          ]}
        >
          {status}
        </Text>
      </View>
    </View>
  );
}

function formatLastSeen(value: string | null) {
  if (!value) return '暂无';
  const seenAt = new Date(value).getTime();
  if (Number.isNaN(seenAt)) return '时间未知';
  const seconds = Math.max(0, Math.round((Date.now() - seenAt) / 1000));
  if (seconds < 3) return '刚刚';
  if (seconds < 60) return `${seconds} 秒前`;
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${minutes} 分钟前`;
  return new Date(value).toLocaleString();
}

function formatCommandType(type: DeviceCommand['type']) {
  const labels: Record<DeviceCommand['type'], string> = {
    sync_character: '同步角色',
    play_reminder: '播放提醒',
    speak_text: '测试播报',
    set_volume: '设置音量',
  };
  return labels[type] ?? type;
}

function formatCommandPayload(payload: Record<string, unknown>) {
  if (typeof payload.text === 'string') return payload.text;
  if (typeof payload.title === 'string') return payload.title;
  if (typeof payload.volume === 'number') return `音量 ${payload.volume}%`;
  if (
    payload.character &&
    typeof payload.character === 'object' &&
    'name' in payload.character &&
    typeof payload.character.name === 'string'
  ) {
    return `角色 ${payload.character.name}`;
  }
  return '无附加内容';
}

function formatEventPayload(payload: Record<string, unknown>) {
  const keys = Object.keys(payload);
  if (keys.length === 0) return '无附加内容';
  if (typeof payload.message === 'string') return payload.message;
  if (typeof payload.reason === 'string') return payload.reason;
  return keys.slice(0, 3).join(' / ');
}

const styles = StyleSheet.create({
  safeArea: { flex: 1, backgroundColor: palette.canvas },
  centered: { flex: 1, alignItems: 'center', justifyContent: 'center', backgroundColor: palette.canvas },
  loadingText: { color: palette.muted, marginTop: 14 },
  header: { paddingHorizontal: 22, paddingTop: 16, paddingBottom: 14, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  eyebrow: { color: palette.primary, fontSize: 11, fontWeight: '800', letterSpacing: 1.4 },
  title: { color: palette.ink, fontSize: 27, fontWeight: '800', marginTop: 4 },
  errorBanner: { marginHorizontal: 18, marginBottom: 8, padding: 12, borderRadius: 12, backgroundColor: '#FDECEC' },
  errorText: { color: palette.red, fontSize: 13 },
  content: { paddingHorizontal: 18, paddingBottom: 26 },
  heroCard: { backgroundColor: palette.ink, borderRadius: 24, padding: 20, marginTop: 6 },
  rowBetween: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  deviceIcon: { width: 48, height: 48, borderRadius: 16, backgroundColor: '#3A304B', alignItems: 'center', justifyContent: 'center' },
  deviceIconText: { color: '#C9B9FF', fontSize: 25 },
  statusPill: { paddingHorizontal: 12, paddingVertical: 6, borderRadius: 999 },
  onlinePill: { backgroundColor: palette.green },
  offlinePill: { backgroundColor: '#625A6D' },
  statusText: { color: '#FFFFFF', fontSize: 12, fontWeight: '700' },
  cardTitle: { color: palette.ink, fontSize: 21, fontWeight: '800', marginTop: 18 },
  cardBody: { color: palette.muted, fontSize: 14, lineHeight: 21, marginTop: 7 },
  meta: { color: palette.muted, fontSize: 12, marginTop: 6 },
  deviceFacts: { borderTopWidth: 1, borderTopColor: '#3A304B', gap: 5, marginTop: 14, paddingTop: 12 },
  deviceFact: { color: '#CFC7DA', fontSize: 12, lineHeight: 17 },
  sectionHeader: { marginTop: 26, marginBottom: 11, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'flex-end' },
  sectionTitle: { color: palette.ink, fontSize: 18, fontWeight: '800' },
  sectionSubtitle: { color: palette.muted, fontSize: 12, flexShrink: 1, marginLeft: 10, textAlign: 'right' },
  stack: { gap: 10 },
  characterCard: { backgroundColor: palette.surface, borderWidth: 1, borderColor: palette.border, borderRadius: 18, padding: 14, flexDirection: 'row', alignItems: 'center' },
  characterCardSelected: { borderColor: palette.primary, backgroundColor: palette.primarySoft },
  avatar: { width: 46, height: 46, borderRadius: 15, alignItems: 'center', justifyContent: 'center' },
  avatarText: { color: '#FFFFFF', fontSize: 18, fontWeight: '800' },
  characterContent: { marginLeft: 12, flex: 1 },
  characterName: { color: palette.ink, fontSize: 16, fontWeight: '800' },
  characterDescription: { color: palette.muted, fontSize: 12, lineHeight: 18, marginTop: 4 },
  selectedMark: { color: palette.primary, fontSize: 11, fontWeight: '700' },
  panel: { backgroundColor: palette.surface, borderRadius: 20, padding: 16, borderWidth: 1, borderColor: palette.border, gap: 12 },
  inlineForm: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  inlineInput: { flex: 1 },
  compactButton: { minHeight: 48, borderRadius: 13, alignItems: 'center', justifyContent: 'center', backgroundColor: palette.ink, paddingHorizontal: 16 },
  compactButtonText: { color: '#FFFFFF', fontSize: 13, fontWeight: '800' },
  input: { backgroundColor: '#FAF9FD', borderWidth: 1, borderColor: palette.border, borderRadius: 13, minHeight: 48, paddingHorizontal: 14, paddingVertical: 12, color: palette.ink, fontSize: 15 },
  primaryButton: { minHeight: 50, borderRadius: 14, alignItems: 'center', justifyContent: 'center', backgroundColor: palette.primary, paddingHorizontal: 16, marginTop: 4 },
  primaryButtonText: { color: '#FFFFFF', fontWeight: '800', fontSize: 15 },
  buttonDisabled: { opacity: 0.42 },
  secondaryButton: { width: 48, height: 44, borderRadius: 13, backgroundColor: palette.primarySoft, alignItems: 'center', justifyContent: 'center' },
  secondaryButtonText: { color: palette.primary, fontSize: 23, fontWeight: '700' },
  volumeRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  volumeText: { color: palette.ink, fontSize: 22, fontWeight: '800' },
  formTitle: { color: palette.ink, fontSize: 22, fontWeight: '800' },
  formHelp: { color: palette.muted, fontSize: 13, lineHeight: 20, marginBottom: 4 },
  fieldLabel: { color: palette.ink, fontSize: 13, fontWeight: '700', marginTop: 5 },
  chipRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  chip: { paddingHorizontal: 13, paddingVertical: 9, borderRadius: 999, backgroundColor: '#F2EFF6' },
  chipSelected: { backgroundColor: palette.primary },
  chipText: { color: palette.muted, fontSize: 12, fontWeight: '700' },
  chipTextSelected: { color: '#FFFFFF' },
  reminderCard: { backgroundColor: palette.surface, borderRadius: 16, borderWidth: 1, borderColor: palette.border, padding: 14, flexDirection: 'row', alignItems: 'center', gap: 8 },
  timelineCard: { backgroundColor: palette.surface, borderRadius: 16, borderWidth: 1, borderColor: palette.border, padding: 14, flexDirection: 'row', alignItems: 'center', gap: 10 },
  reminderContent: { flex: 1 },
  reminderTitle: { color: palette.ink, fontSize: 15, fontWeight: '700' },
  stateBadge: { backgroundColor: '#F0EEF2', borderRadius: 999, paddingHorizontal: 9, paddingVertical: 6 },
  stateBadgeSuccess: { backgroundColor: '#E7F8EF' },
  stateBadgePending: { backgroundColor: '#FFF3D8' },
  stateBadgeText: { color: palette.muted, fontSize: 11, fontWeight: '800' },
  stateBadgeTextSuccess: { color: palette.green },
  stateBadgeTextPending: { color: '#B26A00' },
  smallButton: { backgroundColor: palette.primarySoft, borderRadius: 9, paddingHorizontal: 10, paddingVertical: 8 },
  smallButtonMuted: { backgroundColor: '#F0EEF2' },
  smallButtonText: { color: palette.primary, fontSize: 11, fontWeight: '700' },
  deleteButton: { paddingHorizontal: 4, paddingVertical: 8 },
  deleteText: { color: palette.red, fontSize: 11, fontWeight: '700' },
  emptyCard: { marginTop: 16, backgroundColor: palette.surface, borderRadius: 24, borderWidth: 1, borderColor: palette.border, padding: 24, alignItems: 'center' },
  emptySymbol: { color: palette.primary, fontSize: 46 },
  emptyBody: { textAlign: 'center', marginBottom: 14 },
  emptyText: { color: palette.muted, textAlign: 'center', paddingVertical: 24 },
  endpoint: { color: '#A19AAA', textAlign: 'center', fontSize: 10, marginTop: 28 },
  tabBar: { height: 68, backgroundColor: palette.surface, borderTopWidth: 1, borderTopColor: palette.border, flexDirection: 'row', justifyContent: 'space-around', paddingTop: 8 },
  tabButton: { width: 88, alignItems: 'center', gap: 5 },
  tabDot: { width: 6, height: 6, borderRadius: 3, backgroundColor: '#D8D3DD' },
  tabDotActive: { backgroundColor: palette.primary },
  tabLabel: { color: palette.muted, fontSize: 12, fontWeight: '600' },
  tabLabelActive: { color: palette.primary, fontWeight: '800' },
});
