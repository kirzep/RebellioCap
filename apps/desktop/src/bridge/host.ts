import { convertFileSrc, invoke } from '@tauri-apps/api/core';
import type {
  ActiveConfig,
  AudioCatalog,
  AudioMeasurement,
  Bootstrap,
  EngineSnapshot,
  MonitorChoice,
  OnboardingDraft,
  RecordingTestSummary,
  StoredState,
  SystemCheckResult,
} from '../config/model';
import type { AppAction, AppError, AppSubsystem, ClipPlayback, HostBridge } from './contracts';

const subsystems: AppSubsystem[] = [
  'config',
  'system',
  'capture',
  'audio',
  'engine',
  'filesystem',
];

const appActions: AppAction[] = ['retry', 'diagnostics', 'settings', 'choose-folder'];

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function readableUnknown(value: unknown): string {
  if (typeof value === 'string') return value;
  if (value instanceof Error) return value.message;
  if (isRecord(value)) {
    const direct = value.summary ?? value.message ?? value.technicalCause ?? value.technical_cause;
    if (typeof direct === 'string') return direct;
    return 'Служба вернула ошибку без текстового описания';
  }
  return String(value ?? 'Неизвестная ошибка');
}

function codeFromMessage(message: string): string {
  const explicit = message.match(/\b(?:[a-z][a-z0-9_]*\.)+[a-z0-9_]+\b/i)?.[0];
  if (explicit) return explicit;
  if (/invalid configuration field/i.test(message)) return 'config.invalid';
  if (/unsupported configuration schema/i.test(message)) return 'config.schema_unsupported';
  if (/configuration JSON is invalid/i.test(message)) return 'config.json_invalid';
  if (/configuration I\/O failed/i.test(message)) return 'config.io_failed';
  if (/successful recording test.*required/i.test(message)) {
    return 'config.completion_required';
  }
  return 'engine.unknown';
}

function friendlySummary(message: string, subsystem: AppSubsystem): string {
  if (/[А-Яа-яЁё]/.test(message)) return message;

  const hint = message.toLowerCase();
  if (/dialog\.|dialog thread/.test(hint)) {
    return 'Не удалось открыть системный выбор папки.';
  }
  if (/shell\.|open_failed|failed to open/.test(hint)) {
    return 'Windows не удалось открыть файл.';
  }
  if (/output_directory|output directory|\bdirectory\b|\bpath\b/.test(hint)) {
    return 'Папка клипов недоступна или указана неверно.';
  }
  if (/monitor_unavailable|\bmonitor\b/.test(hint)) {
    return 'Выбранный экран сейчас недоступен.';
  }
  if (/microphone_unavailable|\bmicrophone\b/.test(hint)) {
    return 'Выбранный микрофон сейчас недоступен.';
  }
  if (/system_audio_unavailable|system audio|audio endpoint/.test(hint)) {
    return 'Выбранное устройство звука компьютера сейчас недоступно.';
  }
  if (/duplicate_hotkeys/.test(hint)) return 'Горячие клавиши не должны совпадать.';
  if (/\bhotkey\b/.test(hint)) return 'Выбранное сочетание клавиш недоступно.';
  if (/\b(width|height|resolution|fps|bitrate|video mode)\b/.test(hint)) {
    return 'Параметры видео не поддерживаются или указаны неверно.';
  }
  if (/unsupported configuration schema/.test(hint)) {
    return 'Настройки созданы более новой версией RebellioCap и не были изменены.';
  }
  if (/configuration json is invalid/.test(hint)) {
    return 'Файл настроек повреждён или имеет неверный формат. Он не был сброшен.';
  }
  if (/successful recording test.*required/.test(hint)) {
    return 'Сначала нужно успешно создать тестовый клип.';
  }
  if (/engine\.not_running|engine is not running/.test(hint)) {
    return 'Служба записи сейчас не запущена.';
  }
  if (/timeout|timed out/.test(hint)) {
    return 'Служба записи не ответила вовремя.';
  }
  if (/spawn_failed|engine presence|executable/.test(hint)) {
    return 'Не удалось запустить компонент записи.';
  }

  switch (subsystem) {
    case 'filesystem':
      return 'Не удалось обратиться к папке клипов.';
    case 'capture':
      return 'Не удалось использовать выбранный экран или режим записи.';
    case 'audio':
      return 'Не удалось использовать выбранное аудиоустройство.';
    case 'config':
      return 'Не удалось прочитать или сохранить настройки.';
    case 'system':
      return 'Проверка компьютера не завершилась.';
    case 'engine':
    default:
      return 'Служба записи не выполнила команду.';
  }
}

function inferSubsystem(code: string, message: string): AppSubsystem {
  const hint = `${code} ${message}`.toLowerCase();
  if (code.startsWith('dialog.')) return 'filesystem';
  if (/output_directory|output directory|filesystem|directory|\bpath\b/.test(hint)) {
    return 'filesystem';
  }
  if (/system_audio|microphone|audio endpoint|wasapi/.test(hint)) return 'audio';
  if (/monitor|resolution|video mode|capture|dxgi|nvenc/.test(hint)) return 'capture';
  if (code.startsWith('config.')) return 'config';
  if (code.startsWith('system.') || code.startsWith('doctor.')) return 'system';
  if (code.startsWith('audio.')) return 'audio';
  if (code.startsWith('output.') || code.startsWith('fs.') || code.startsWith('shell.')) {
    return 'filesystem';
  }
  return 'engine';
}

export function normalizeError(err: unknown): AppError {
  const record = isRecord(err) ? err : null;
  const message = readableUnknown(err);
  const rawCode = record?.code;
  const code = typeof rawCode === 'string' && rawCode.trim()
    ? rawCode.trim()
    : codeFromMessage(message);

  const rawSubsystem = record?.subsystem;
  const subsystem =
    typeof rawSubsystem === 'string' && subsystems.includes(rawSubsystem as AppSubsystem)
      ? (rawSubsystem as AppSubsystem)
      : inferSubsystem(code, message);

  const nonRetryable = /unsupported|invalid|validation|reserved|schema_newer|replay_memory_unavailable/i.test(code);
  const retryable = typeof record?.retryable === 'boolean' ? record.retryable : !nonRetryable;

  const rawActions = Array.isArray(record?.actions)
    ? record.actions.filter(
        (action): action is AppAction =>
          typeof action === 'string' && appActions.includes(action as AppAction)
      )
    : null;

  const actions: AppAction[] = rawActions ? [...rawActions] : [];
  if (!rawActions) {
    if (retryable) actions.push('retry');
    if (subsystem === 'filesystem') actions.push('choose-folder');
    if (subsystem === 'config' || subsystem === 'capture' || subsystem === 'audio' || code === 'replay.memory_budget_exceeded') {
      actions.push('settings');
    }
    actions.push('diagnostics');
  }

  const summary =
    code === 'config.replay_memory_unavailable'
      ? (/[А-Яа-яЁё]/.test(message) ? message.replace(/^config\.replay_memory_unavailable:\s*/, '').split('; rollback:')[0] : 'Выбранный лимит Replay слишком велик для доступной RAM. Уменьшите лимит или выберите «Авто».')
      : code === 'replay.memory_budget_exceeded'
      ? 'Достигнут лимит памяти Replay. Дождитесь завершения сохранений или уменьшите длительность буфера.'
      : code === 'recording.memory_budget_exceeded'
        ? 'Запись остановлена из-за лимита памяти. Проверьте скорость и доступность диска.'
      : typeof record?.summary === 'string' && record.summary.trim()
      ? record.summary.trim()
      : friendlySummary(message, subsystem);
  const technicalCause =
    typeof record?.technicalCause === 'string' && record.technicalCause.trim()
      ? record.technicalCause.trim()
      : typeof record?.technical_cause === 'string' && record.technical_cause.trim()
        ? record.technical_cause.trim()
        : message;

  return {
    code,
    summary,
    subsystem,
    technicalCause,
    retryable,
    actions: [...new Set(actions)],
  };
}

export function errorSummary(err: unknown): string {
  return normalizeError(err).summary;
}

function hasTauriRuntime(): boolean {
  return typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window;
}

function hostUnavailableError(): AppError {
  return {
    code: 'host.unavailable',
    summary: 'Не удалось связаться с desktop-службой RebellioCap.',
    subsystem: 'engine',
    technicalCause: 'Tauri IPC is unavailable in the current runtime.',
    retryable: true,
    actions: ['retry'],
  };
}

async function invokeHost<T>(
  command: string,
  args?: Record<string, unknown>
): Promise<T> {
  if (!hasTauriRuntime()) throw hostUnavailableError();
  try {
    return await invoke<T>(command, args);
  } catch (cause) {
    throw normalizeError(cause);
  }
}

export class TauriHostBridge implements HostBridge {
  async beginClipCatalog(owner: string): Promise<void> { await invokeHost('begin_clip_catalog', { owner }); }
  async listClipPage(request: import('./contracts').ClipCatalogRequest): Promise<import('./contracts').ClipCatalogPage> { return invokeHost('list_clip_page', { request }); }
  async releaseClipCatalog(owner: string): Promise<void> { await invokeHost('release_clip_catalog', { owner }); }
  async listClips(): Promise<import('./contracts').SavedClip[]> {
    return invokeHost('list_clips');
  }
  async clipThumbnail(name: string): Promise<number[]> {
    return invoke('clip_thumbnail', { name });
  }
  async folderIcon(folder: string): Promise<number[]> {
    return invoke('folder_icon', { folder });
  }
  async readAudioPeak(endpointId: string): Promise<number> {
    return invoke('read_audio_peak', { endpointId });
  }
  async openClip(name: string): Promise<void> {
    await invokeHost('open_clip', { name });
  }
  async prepareClipPlayback(name: string): Promise<ClipPlayback> {
    const media = await invokeHost<ClipPlayback>('prepare_clip_playback', { name });
    return { ...media, video: convertFileSrc(media.video), tracks: media.tracks.map(track => ({ ...track, path: convertFileSrc(track.path) })) };
  }
  async releaseClipPlayback(id: string): Promise<void> {
    await invokeHost('release_clip_playback', { id });
  }
  async renameClip(name: string, newName: string): Promise<void> {
    return invoke('rename_clip', { name, newName });
  }
  async deleteClip(name: string): Promise<void> {
    return invoke('delete_clip', { name });
  }
  async bootstrapApp(): Promise<Bootstrap> {
    return invokeHost<Bootstrap>('bootstrap_app');
  }

  async getOnboardingState(): Promise<StoredState> {
    return invokeHost<StoredState>('get_onboarding_state');
  }

  async runSystemCheck(): Promise<SystemCheckResult> {
    return invokeHost<SystemCheckResult>('run_system_check');
  }

  async listMonitors(): Promise<MonitorChoice[]> {
    return invokeHost<MonitorChoice[]>('list_monitors');
  }

  async listAudioEndpoints(): Promise<AudioCatalog> {
    return invokeHost<AudioCatalog>('list_audio_endpoints');
  }

  async measureAudioLevel(endpointId: string | null): Promise<AudioMeasurement> {
    return invokeHost<AudioMeasurement>('measure_audio_level', {
      endpointId: endpointId ?? null,
    });
  }

  async chooseOutputDirectory(): Promise<string | null> {
    return invokeHost<string | null>('choose_output_directory');
  }

  async saveOnboardingDraft(draft: OnboardingDraft, lastCompletedStep: number): Promise<void> {
    await invokeHost<void>('save_onboarding_draft', {
      draft,
      lastCompletedStep,
    });
  }

  async runRecordingTest(): Promise<RecordingTestSummary> {
    return invokeHost<RecordingTestSummary>('run_recording_test');
  }

  async completeOnboarding(explicitCompletion: boolean): Promise<Bootstrap> {
    return invokeHost<Bootstrap>('complete_onboarding', {
      explicitCompletion,
    });
  }

  async getEngineSnapshot(): Promise<EngineSnapshot> {
    return invokeHost<EngineSnapshot>('get_engine_snapshot');
  }

  async startEngine(): Promise<EngineSnapshot> {
    return invokeHost<EngineSnapshot>('start_engine');
  }

  async stopEngine(): Promise<EngineSnapshot> {
    return invokeHost<EngineSnapshot>('stop_engine');
  }

  async saveReplay(): Promise<EngineSnapshot> {
    return invokeHost<EngineSnapshot>('save_replay');
  }

  async toggleRecording(): Promise<EngineSnapshot> {
    return invokeHost<EngineSnapshot>('toggle_recording');
  }
  async startReplay(): Promise<EngineSnapshot> {
    return invokeHost<EngineSnapshot>('start_replay');
  }
  async stopReplay(): Promise<EngineSnapshot> {
    return invokeHost<EngineSnapshot>('stop_replay');
  }

  async applyRuntimeSettings(candidate: ActiveConfig): Promise<EngineSnapshot> {
    return invokeHost<EngineSnapshot>('apply_runtime_settings', {
      candidate,
    });
  }

  async openDiagnostics(): Promise<void> {
    await invokeHost<void>('open_diagnostics');
  }

  async openTestClip(): Promise<void> {
    await invokeHost<void>('open_test_clip');
  }
}

export const host = new TauriHostBridge();
