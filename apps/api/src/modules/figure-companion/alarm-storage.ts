export const ALARM_COS_PREFIX = 'jh_chat/alarms';

function pad(value: number) {
  return String(value).padStart(2, '0');
}

function sanitizeAudioName(name: string) {
  return name
    .trim()
    .replace(/\.[^.]+$/, '')
    .replace(/[\\/:*?"<>|\s]+/g, '_')
    .replace(/^_+|_+$/g, '') || 'alarm';
}

/**
 * 闹钟音频在 COS 中的固定对象路径：
 * jh_chat/alarms/年月日/音频名字.扩展名
 */
export function buildAlarmCosObjectKey(
  audioName: string,
  extension = 'wav',
  now = new Date(),
) {
  const date = `${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}`;
  const safeName = sanitizeAudioName(audioName);
  const safeExtension = extension.replace(/^\.+/, '').toLowerCase() || 'wav';

  return `${ALARM_COS_PREFIX}/${date}/${safeName}.${safeExtension}`;
}
