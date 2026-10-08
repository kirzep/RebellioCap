import recordingContract from '../../../../contracts/recording-settings.v1.json';
import type { Hotkey, OnboardingDraft } from './model';

export interface ValidationError {
  field: string;
  code: string;
  message: string;
}

export function isValidIdentity(value?: string | null): boolean {
  if (!value) return false;
  const trimmed = value.trim();
  if (trimmed.length === 0 || trimmed.length > 4096) return false;
  for (let i = 0; i < trimmed.length; i++) {
    const code = trimmed.charCodeAt(i);
    if (code < 32 || code === 127) return false;
  }
  return true;
}

export function isValidHotkey(hotkey?: Hotkey | null): boolean {
  if (!hotkey) return false;
  const { key, ctrl, alt, win } = hotkey;
  const isDigit = key >= 0x30 && key <= 0x39;
  const isAlpha = key >= 0x41 && key <= 0x5a;
  const isFn = key >= 0x70 && key <= 0x87;
  const isSpecial = key === 0x09 || key === 0x1b || key === 0x2e || key === 0x20;
  const supported = isDigit || isAlpha || isFn || isSpecial;

  const reserved =
    win ||
    (alt && (key === 0x09 || key === 0x20 || key === 0x73)) || // Tab, Space, F4
    (key === 0x1b && (ctrl || alt)) || // Esc with Ctrl or Alt
    (key === 0x2e && ctrl && alt) || // Ctrl+Alt+Del
    key === 0x7b; // F12 reserved by RegisterHotKey

  return supported && !reserved;
}

export function areHotkeysEqual(a: Hotkey, b: Hotkey): boolean {
  return (
    a.key === b.key &&
    a.ctrl === b.ctrl &&
    a.alt === b.alt &&
    a.shift === b.shift &&
    a.win === b.win
  );
}

export function isValidOutputPath(path?: string | null): boolean {
  if (!path) return false;
  const trimmed = path.trim().replace(/^\\\\\?\\UNC\\/, '\\\\').replace(/^\\\\\?\\/, '');
  if (
    trimmed.length === 0 ||
    trimmed.length > 4096 ||
    [...trimmed].some((character) => {
      const code = character.charCodeAt(0);
      return code < 32 || code === 127;
    })
  ) {
    return false;
  }
  const isWindowsAbsolute = /^[a-zA-Z]:[\\/]/.test(trimmed);
  const isUncAbsolute = /^\\\\[^\\/]+[\\/][^\\/]+/.test(trimmed);
  if (!isWindowsAbsolute && !isUncAbsolute) return false;
  const parts = trimmed.split(/[\\/]/).filter((p) => p !== '');
  if (parts.includes('..') || parts.includes('.')) return false;
  return true;
}

export function validateDraftStep(
  step: number,
  draft: OnboardingDraft
): ValidationError[] {
  const errors: ValidationError[] = [];

  switch (step) {
    case 1: {
      // Video
      if (!isValidIdentity(draft.monitor_id)) {
        errors.push({ field: 'monitor_id', code: 'required', message: 'Выберите монитор' });
      }
      if (
        draft.width == null ||
        !Number.isInteger(draft.width) ||
        draft.width < recordingContract.ranges.width.min ||
        draft.width > recordingContract.ranges.width.max ||
        draft.width % 2 !== 0
      ) {
        errors.push({ field: 'width', code: 'invalid', message: recordingContract.messages.width });
      }
      if (
        draft.height == null ||
        !Number.isInteger(draft.height) ||
        draft.height < recordingContract.ranges.height.min ||
        draft.height > recordingContract.ranges.height.max ||
        draft.height % 2 !== 0
      ) {
        errors.push({ field: 'height', code: 'invalid', message: recordingContract.messages.height });
      }
      if (
        draft.fps == null ||
        !Number.isInteger(draft.fps) ||
        draft.fps < recordingContract.ranges.fps.min ||
        draft.fps > recordingContract.ranges.fps.max
      ) {
        errors.push({ field: 'fps', code: 'invalid', message: recordingContract.messages.fps });
      }
      if (
        draft.bitrate == null ||
        !Number.isInteger(draft.bitrate) ||
        draft.bitrate < recordingContract.ranges.bitrate.min ||
        draft.bitrate > recordingContract.ranges.bitrate.max
      ) {
        errors.push({ field: 'bitrate', code: 'invalid', message: recordingContract.messages.bitrate });
      }
      break;
    }
    case 2: {
      // Audio
      if (!draft.system_audio) {
        errors.push({ field: 'system_audio', code: 'required', message: 'Выберите системное аудио' });
      } else if (typeof draft.system_audio === 'object' && !isValidIdentity(draft.system_audio.endpoint)) {
        errors.push({ field: 'system_audio', code: 'invalid', message: 'Некорректное устройство системного аудио' });
      }

      if (!draft.microphone) {
        errors.push({ field: 'microphone', code: 'required', message: 'Выберите микрофон или отключите его' });
      } else if (typeof draft.microphone === 'object' && !isValidIdentity(draft.microphone.endpoint)) {
        errors.push({ field: 'microphone', code: 'invalid', message: 'Некорректное устройство микрофона' });
      }
      break;
    }
    case 3: {
      // Replay and Storage
      if (
        draft.replay_seconds == null ||
        !Number.isInteger(draft.replay_seconds) ||
        draft.replay_seconds < recordingContract.ranges.replay_seconds.min ||
        draft.replay_seconds > recordingContract.ranges.replay_seconds.max
      ) {
        errors.push({ field: 'replay_seconds', code: 'invalid', message: recordingContract.messages.replay_seconds });
      }
      const memoryLimit = draft.replay_memory_limit_mb;
      if (memoryLimit !== undefined && (memoryLimit === null || !Number.isInteger(memoryLimit) ||
          (memoryLimit !== 0 && (memoryLimit < recordingContract.ranges.replay_memory_limit_mb.min || memoryLimit > recordingContract.ranges.replay_memory_limit_mb.max)))) {
        errors.push({ field: 'replay_memory_limit_mb', code: 'invalid', message: recordingContract.messages.replay_memory_limit_mb });
      }
      if (draft.replay_mode !== 'ram') {
        errors.push({ field: 'replay_mode', code: 'unsupported', message: 'Replay может храниться только в оперативной памяти' });
      }
      if (!draft.container || (draft.container !== 'mp4' && draft.container !== 'mkv')) {
        errors.push({ field: 'container', code: 'required', message: 'Выберите формат контейнера' });
      }
      if (!isValidOutputPath(draft.output_directory)) {
        errors.push({ field: 'output_directory', code: 'invalid', message: 'Выберите папку для сохранения клипов.' });
      }
      break;
    }
    case 4: {
      // Hotkeys
      if (!isValidHotkey(draft.save_replay_hotkey)) {
        errors.push({ field: 'save_replay_hotkey', code: 'invalid', message: 'Недопустимая горячая клавиша Replay' });
      }
      if (!isValidHotkey(draft.toggle_recording_hotkey)) {
        errors.push({ field: 'toggle_recording_hotkey', code: 'invalid', message: 'Недопустимая горячая клавиша записи' });
      }
      if (
        draft.save_replay_hotkey &&
        draft.toggle_recording_hotkey &&
        areHotkeysEqual(draft.save_replay_hotkey, draft.toggle_recording_hotkey)
      ) {
        errors.push({ field: 'duplicate_hotkeys', code: 'duplicate', message: 'Горячие клавиши не должны совпадать' });
      }
      if (typeof draft.continuous_recording_enabled !== 'boolean') {
        errors.push({
          field: 'continuous_recording_enabled',
          code: 'required',
          message: 'Укажите, разрешена ли обычная запись',
        });
      }
      break;
    }
    case 5: {
      // Summary and final defaults
      if (!draft.preferences) {
        errors.push({ field: 'preferences', code: 'required', message: 'Настройки обязательны' });
      }
      break;
    }
    case 6: {
      // Test
      break;
    }
  }

  return errors;
}

export function validateCompleteDraft(draft: OnboardingDraft): ValidationError[] {
  const errors: ValidationError[] = [];
  for (let step = 1; step <= 5; step += 1) {
    errors.push(...validateDraftStep(step, draft));
  }
  return errors;
}
