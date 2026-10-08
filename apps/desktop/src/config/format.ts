import { t, getLanguageSnapshot, getLocale } from '../i18n';
import type {
  AudioCatalog,
  AudioSelection,
  Container,
  Hotkey,
  MonitorChoice,
} from './model';

export function formatHotkey(hotkey?: Hotkey | null): string {
  if (!hotkey) return t('Не задано');

  const parts: string[] = [];
  if (hotkey.ctrl) parts.push('Ctrl');
  if (hotkey.alt) parts.push('Alt');
  if (hotkey.shift) parts.push('Shift');
  if (hotkey.win) parts.push('Win');

  if (hotkey.key >= 0x70 && hotkey.key <= 0x87) {
    parts.push(`F${hotkey.key - 0x70 + 1}`);
  } else if (
    (hotkey.key >= 0x30 && hotkey.key <= 0x39) ||
    (hotkey.key >= 0x41 && hotkey.key <= 0x5a)
  ) {
    parts.push(String.fromCharCode(hotkey.key));
  } else {
    const names: Record<number, string> = {
      0x09: 'Tab',
      0x1b: 'Esc',
      0x20: t('Пробел'),
      0x2e: 'Delete',
    };
    parts.push(names[hotkey.key] ?? `0x${hotkey.key.toString(16).toUpperCase()}`);
  }

  return parts.join(' + ');
}

export function formatSeconds(seconds?: number | null): string {
  if (seconds == null || !Number.isFinite(seconds)) return t('Не задано');
  const value = Math.round(seconds);
  const absolute = Math.abs(value);
  if (getLanguageSnapshot().language === 'en') {
    return `${value.toLocaleString(getLocale())} ${absolute === 1 ? 'second' : 'seconds'}`;
  }
  const lastTwo = absolute % 100;
  const last = absolute % 10;
  const unit =
    lastTwo >= 11 && lastTwo <= 14
      ? t('секунд')
      : last === 1
        ? t('секунда')
        : last >= 2 && last <= 4
          ? t('секунды')
          : t('секунд');
  return `${value} ${unit}`;
}

export function formatReplaySaveLabel(seconds?: number | null): string {
  if (seconds == null || !Number.isFinite(seconds)) return t('Сохранить Replay');
  const value = Math.round(seconds);
  const absolute = Math.abs(value);
  if (getLanguageSnapshot().language === 'en') {
    return `Save the last ${value.toLocaleString(getLocale())} ${absolute === 1 ? 'second' : 'seconds'}`;
  }
  const lastTwo = absolute % 100;
  const last = absolute % 10;
  if (!(lastTwo >= 11 && lastTwo <= 14) && last === 1) {
    return t("Сохранить последнюю {0} секунду", value);
  }
  const unit = !(lastTwo >= 11 && lastTwo <= 14) && last >= 2 && last <= 4
    ? t('секунды')
    : t('секунд');
  return t("Сохранить последние {0} {1}", value, unit);
}

export function formatBitrate(bitrate?: number | null): string {
  if (bitrate == null || !Number.isFinite(bitrate)) return t('Не задано');
  const value = bitrate / 1_000_000;
  const formatted = value.toLocaleString(getLocale(), { maximumFractionDigits: 1, useGrouping: false });
  return t("{0} Мбит/с", formatted);
}

export function formatResolution(width?: number | null, height?: number | null): string {
  if (!width || !height) return t('Не задано');
  return `${width} × ${height}`;
}

export function formatVideoMode(
  width?: number | null,
  height?: number | null,
  fps?: number | null,
  bitrate?: number | null
): string {
  const resolution = formatResolution(width, height);
  const frameRate = fps == null ? t('частота не задана') : t("{0} кадров/с", fps);
  return `${resolution} · ${frameRate} · ${formatBitrate(bitrate)}`;
}

export function formatContainer(container?: Container | null): string {
  if (!container) return t('Не задан');
  return container.toUpperCase();
}

export function estimateReplayMegabytes(
  bitrate?: number | null,
  seconds?: number | null
): number | null {
  if (!bitrate || !seconds || bitrate <= 0 || seconds <= 0) return null;
  return Math.max(1, Math.round((bitrate / 8 / 1_000_000) * seconds));
}

export function friendlyMonitorName(name: string): string {
  const display = name.match(/DISPLAY(\d+)$/i);
  return display ? t("Экран {0}", Number(display[1])) : name;
}

export function findMonitorName(
  monitorId: string | null | undefined,
  monitors: MonitorChoice[]
): string {
  if (!monitorId) return t('Экран не выбран');
  const monitor = monitors.find((item) => item.id === monitorId);
  return monitor ? `${friendlyMonitorName(monitor.name)}${monitor.primary ? t(' · Основной') : ''}` : t('Сохранённый экран · Недоступен');
}

export function audioSelectionEndpoint(selection?: AudioSelection | null): string | null {
  return selection && typeof selection === 'object' ? selection.endpoint : null;
}

export function formatAudioSelection(
  selection: AudioSelection | null | undefined,
  choices: AudioCatalog['system_audio'] | AudioCatalog['microphones'] = []
): string {
  if (!selection || selection === 'disabled') return t('Не записывать');
  const choice = choices.find((item) => item.id === selection.endpoint);
  if (!choice) return t("{0} · Недоступно", selection.endpoint);
  return `${choice.name}${choice.is_default ? t(' · По умолчанию') : ''}${choice.available ? '' : t(' · Недоступно')}`;
}
