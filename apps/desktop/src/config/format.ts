import type {
  AudioCatalog,
  AudioSelection,
  Container,
  Hotkey,
  MonitorChoice,
} from './model';

export function formatHotkey(hotkey?: Hotkey | null): string {
  if (!hotkey) return 'Не задано';

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
      0x20: 'Пробел',
      0x2e: 'Delete',
    };
    parts.push(names[hotkey.key] ?? `0x${hotkey.key.toString(16).toUpperCase()}`);
  }

  return parts.join(' + ');
}

export function formatSeconds(seconds?: number | null): string {
  if (seconds == null || !Number.isFinite(seconds)) return 'Не задано';
  const value = Math.round(seconds);
  const absolute = Math.abs(value);
  const lastTwo = absolute % 100;
  const last = absolute % 10;
  const unit =
    lastTwo >= 11 && lastTwo <= 14
      ? 'секунд'
      : last === 1
        ? 'секунда'
        : last >= 2 && last <= 4
          ? 'секунды'
          : 'секунд';
  return `${value} ${unit}`;
}

export function formatReplaySaveLabel(seconds?: number | null): string {
  if (seconds == null || !Number.isFinite(seconds)) return 'Сохранить Replay';
  const value = Math.round(seconds);
  const absolute = Math.abs(value);
  const lastTwo = absolute % 100;
  const last = absolute % 10;
  if (!(lastTwo >= 11 && lastTwo <= 14) && last === 1) {
    return `Сохранить последнюю ${value} секунду`;
  }
  const unit = !(lastTwo >= 11 && lastTwo <= 14) && last >= 2 && last <= 4
    ? 'секунды'
    : 'секунд';
  return `Сохранить последние ${value} ${unit}`;
}

export function formatBitrate(bitrate?: number | null): string {
  if (bitrate == null || !Number.isFinite(bitrate)) return 'Не задано';
  const value = bitrate / 1_000_000;
  const formatted = Number.isInteger(value) ? String(value) : value.toFixed(1).replace('.', ',');
  return `${formatted} Мбит/с`;
}

export function formatResolution(width?: number | null, height?: number | null): string {
  if (!width || !height) return 'Не задано';
  return `${width} × ${height}`;
}

export function formatVideoMode(
  width?: number | null,
  height?: number | null,
  fps?: number | null,
  bitrate?: number | null
): string {
  const resolution = formatResolution(width, height);
  const frameRate = fps == null ? 'частота не задана' : `${fps} кадров/с`;
  return `${resolution} · ${frameRate} · ${formatBitrate(bitrate)}`;
}

export function formatContainer(container?: Container | null): string {
  if (!container) return 'Не задан';
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
  return display ? `Экран ${Number(display[1])}` : name;
}

export function findMonitorName(
  monitorId: string | null | undefined,
  monitors: MonitorChoice[]
): string {
  if (!monitorId) return 'Экран не выбран';
  const monitor = monitors.find((item) => item.id === monitorId);
  return monitor ? `${friendlyMonitorName(monitor.name)}${monitor.primary ? ' · Основной' : ''}` : 'Сохранённый экран · Недоступен';
}

export function audioSelectionEndpoint(selection?: AudioSelection | null): string | null {
  return selection && typeof selection === 'object' ? selection.endpoint : null;
}

export function formatAudioSelection(
  selection: AudioSelection | null | undefined,
  choices: AudioCatalog['system_audio'] | AudioCatalog['microphones'] = []
): string {
  if (!selection || selection === 'disabled') return 'Не записывать';
  const choice = choices.find((item) => item.id === selection.endpoint);
  if (!choice) return `${selection.endpoint} · Недоступно`;
  return `${choice.name}${choice.is_default ? ' · По умолчанию' : ''}${choice.available ? '' : ' · Недоступно'}`;
}
