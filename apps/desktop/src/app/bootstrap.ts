import type { HostBridge } from '../bridge/contracts';
import { normalizeError } from '../bridge/host';
import type { Bootstrap, EngineSnapshot, OnboardingDraft, StoredState } from '../config/model';
import { validateDraftStep } from '../config/validation';
import { repairSectionForIssue, type Route } from './route';

export interface BootstrapResult {
  route: Route;
  state: StoredState;
  snapshot: EngineSnapshot;
}

function hasPersistedDraft(draft: OnboardingDraft): boolean {
  return Object.values(draft).some((value) => value !== null && value !== undefined);
}

function firstInvalidCompletedStep(state: StoredState): number | null {
  const completedThrough = Math.min(Math.max(state.onboarding.last_completed_step, 0), 5);
  for (let step = 1; step <= completedThrough; step += 1) {
    if (validateDraftStep(step, state.draft).length > 0) return step;
  }
  return null;
}

async function firstUnavailableDeviceStep(
  host: HostBridge,
  state: StoredState
): Promise<number | null> {
  const [monitorResult, audioResult] = await Promise.allSettled([
    host.listMonitors(),
    host.listAudioEndpoints(),
  ]);

  if (monitorResult.status === 'fulfilled' && state.draft.monitor_id) {
    const monitorAvailable = monitorResult.value.some(
      (monitor) => monitor.id === state.draft.monitor_id
    );
    if (!monitorAvailable) return 1;
  }

  if (audioResult.status === 'fulfilled') {
    const systemEndpoint =
      state.draft.system_audio && typeof state.draft.system_audio === 'object'
        ? state.draft.system_audio.endpoint
        : null;
    const microphoneEndpoint =
      state.draft.microphone && typeof state.draft.microphone === 'object'
        ? state.draft.microphone.endpoint
        : null;
    const systemAvailable =
      !systemEndpoint ||
      audioResult.value.system_audio.some(
        (choice) => choice.id === systemEndpoint && choice.available
      );
    const microphoneAvailable =
      !microphoneEndpoint ||
      audioResult.value.microphones.some(
        (choice) => choice.id === microphoneEndpoint && choice.available
      );

    if (!systemAvailable || !microphoneAvailable) return 2;
  }

  return null;
}

export async function resolveBootstrapResult(
  host: HostBridge,
  result: Bootstrap
): Promise<BootstrapResult> {
  const { state, snapshot } = result;

  if (state.onboarding.completed) {
    if (!state.active) {
      const technicalCause =
        snapshot.lastError?.message ||
        'Флаг завершения настройки установлен, но действующая конфигурация отсутствует.';
      return {
        route: {
          kind: 'blocked',
          error: {
            code: snapshot.lastError?.code || 'config.active_missing',
            summary: 'Сохранённая конфигурация недоступна. Файл не был сброшен.',
            subsystem: 'config',
            technicalCause,
            retryable: false,
            actions: ['diagnostics'],
          },
        },
        state,
        snapshot,
      };
    }

    if (
      snapshot.lastError &&
      ['blocked', 'failed', 'degraded'].includes(snapshot.lifecycle)
    ) {
      const startupError = normalizeError({
        code: snapshot.lastError.code,
        message: snapshot.lastError.message,
      });
      return {
        route: {
          kind: 'home',
          repair: {
            section: repairSectionForIssue(startupError),
            error: startupError,
          },
        },
        state,
        snapshot,
      };
    }

    return {
      route: { kind: 'home' },
      state,
      snapshot,
    };
  }

  const lastStep = state.onboarding.last_completed_step;
  if (lastStep > 0 || hasPersistedDraft(state.draft)) {
    const invalidStep = firstInvalidCompletedStep(state);
    const resumeStep = Math.min(Math.max(lastStep + 1, 1), 6);
    const unavailableStep = invalidStep
      ? null
      : await firstUnavailableDeviceStep(host, state);
    return {
      route: {
        kind: 'onboarding',
        step: invalidStep ?? Math.min(unavailableStep ?? resumeStep, resumeStep),
      },
      state,
      snapshot,
    };
  }

  return {
    route: { kind: 'start' },
    state,
    snapshot,
  };
}

export async function bootstrap(host: HostBridge): Promise<BootstrapResult> {
  return resolveBootstrapResult(host, await host.bootstrapApp());
}
