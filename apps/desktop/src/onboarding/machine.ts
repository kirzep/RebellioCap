import type { HostBridge } from '../bridge/contracts';
import { createDraftDefaults } from '../config/defaults';
import type { OnboardingDraft } from '../config/model';
import {
  validateCompleteDraft,
  validateDraftStep,
  type ValidationError,
} from '../config/validation';

export interface OnboardingState {
  step: number;
  draft: OnboardingDraft;
  lastCompletedStep: number;
  isSaving: boolean;
  errors: ValidationError[];
}

export interface OnboardingMachine {
  snapshot(): OnboardingState;
  subscribe(listener: (state: OnboardingState) => void): () => void;
  updateDraft(patch: Partial<OnboardingDraft>): void;
  next(): Promise<void>;
  back(): Promise<void>;
  goToStep(step: number): Promise<void>;
}

const FIELD_STEP: Record<string, number> = {
  monitor_id: 1,
  width: 1,
  height: 1,
  fps: 1,
  bitrate: 1,
  system_audio: 2,
  microphone: 2,
  replay_seconds: 3,
  replay_memory_limit_mb: 3,
  replay_mode: 3,
  container: 3,
  output_directory: 3,
  save_replay_hotkey: 4,
  toggle_recording_hotkey: 4,
  duplicate_hotkeys: 4,
  preferences: 5,
  continuous_recording_enabled: 4,
};

function stepForValidationError(error: ValidationError): number {
  return FIELD_STEP[error.field] ?? 1;
}

export function createOnboardingMachine(
  host: HostBridge,
  initialDraft: OnboardingDraft = {},
  initialStep = 1,
  initialLastCompletedStep = 0
): OnboardingMachine {
  let state: OnboardingState = {
    step: Math.max(1, Math.min(6, initialStep)),
    draft: {
      ...createDraftDefaults(),
      ...Object.fromEntries(Object.entries(initialDraft).filter(([, value]) => value != null)),
    },
    lastCompletedStep: Math.max(0, Math.min(6, initialLastCompletedStep)),
    isSaving: false,
    errors: [],
  };

  const listeners = new Set<(s: OnboardingState) => void>();

  function notify() {
    for (const listener of listeners) {
      listener({ ...state });
    }
  }

  async function navigateTo(targetStep: number): Promise<void> {
    if (
      state.isSaving ||
      targetStep < 1 ||
      targetStep > 6 ||
      targetStep === state.step ||
      targetStep > state.lastCompletedStep + 1
    ) {
      return;
    }

    if (state.step === 6) {
      state = { ...state, step: targetStep, errors: [] };
      notify();
      return;
    }

    // Validation errors may belong to a step the user has just left. Keep the
    // edited draft in memory and let them move among already completed steps,
    // but do not send the invalid aggregate draft to the host.
    if (state.errors.length > 0 && targetStep !== 6) {
      state = { ...state, step: targetStep };
      notify();
      return;
    }

    const errors =
      targetStep === 6
        ? validateCompleteDraft(state.draft)
        : validateDraftStep(state.step, state.draft);
    if (errors.length > 0) {
      const canLeaveUnsaved =
        targetStep !== 6 && targetStep <= state.lastCompletedStep;
      state = {
        ...state,
        step:
          targetStep === 6
            ? stepForValidationError(errors[0])
            : canLeaveUnsaved
              ? targetStep
              : state.step,
        errors,
      };
      notify();
      if (canLeaveUnsaved) return;
      throw new Error(`validation_failed: ${errors.map((error) => error.field).join(', ')}`);
    }

    state = { ...state, isSaving: true, errors: [] };
    notify();

    try {
      await host.saveOnboardingDraft(state.draft, state.lastCompletedStep);
      state = { ...state, isSaving: false, step: targetStep };
      notify();
    } catch (error) {
      state = { ...state, isSaving: false };
      notify();
      throw error;
    }
  }

  return {
    snapshot() {
      return { ...state };
    },

    subscribe(listener) {
      listeners.add(listener);
      return () => {
        listeners.delete(listener);
      };
    },

    updateDraft(patch) {
      if (state.isSaving) return;
      const changedFields = new Set(Object.keys(patch));
      state = {
        ...state,
        draft: { ...state.draft, ...patch },
        errors: state.errors.filter((error) => {
          if (changedFields.has(error.field)) return false;
          if (
            error.field === 'duplicate_hotkeys' &&
            (changedFields.has('save_replay_hotkey') ||
              changedFields.has('toggle_recording_hotkey'))
          ) {
            return false;
          }
          return true;
        }),
      };
      notify();
    },

    async next() {
      if (state.isSaving) return;

      if (state.errors.length > 0) {
        const errorStep = stepForValidationError(state.errors[0]);
        state = { ...state, step: errorStep };
        notify();
        throw new Error(
          `validation_failed: ${state.errors.map((error) => error.field).join(', ')}`
        );
      }

      const errors =
        state.step === 5
          ? validateCompleteDraft(state.draft)
          : validateDraftStep(state.step, state.draft);
      if (errors.length > 0) {
        state = { ...state, errors };
        notify();
        throw new Error(`validation_failed: ${errors.map((e) => e.field).join(', ')}`);
      }

      state = { ...state, isSaving: true, errors: [] };
      notify();

      try {
        const nextCompletedStep = Math.min(5, Math.max(state.lastCompletedStep, state.step));
        await host.saveOnboardingDraft(state.draft, nextCompletedStep);

        state = {
          ...state,
          isSaving: false,
          lastCompletedStep: nextCompletedStep,
          step: Math.min(state.step + 1, 6),
        };
        notify();
      } catch (err) {
        state = { ...state, isSaving: false };
        notify();
        throw err;
      }
    },

    async back() {
      if (state.step <= 1 || state.isSaving) return;
      await navigateTo(state.step - 1);
    },

    async goToStep(targetStep) {
      await navigateTo(targetStep);
    },
  };
}
