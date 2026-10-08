import { describe, expect, it, vi } from 'vitest';
import type { HostBridge } from '../bridge/contracts';
import type { OnboardingDraft } from '../config/model';
import { createOnboardingMachine } from './machine';

function createMockHost(saveDraftMock?: (draft: OnboardingDraft, step: number) => Promise<void>): HostBridge {
  return {
    bootstrapApp: vi.fn(),
    getOnboardingState: vi.fn(),
    runSystemCheck: vi.fn(),
    listMonitors: vi.fn(),
    listAudioEndpoints: vi.fn(),
    measureAudioLevel: vi.fn(),
    chooseOutputDirectory: vi.fn(),
    saveOnboardingDraft: saveDraftMock || vi.fn().mockResolvedValue(undefined),
    runRecordingTest: vi.fn(),
    completeOnboarding: vi.fn(),
    getEngineSnapshot: vi.fn(),
    startEngine: vi.fn(),
    stopEngine: vi.fn(),
    saveReplay: vi.fn(),
    toggleRecording: vi.fn(),
    applyRuntimeSettings: vi.fn(),
    openDiagnostics: vi.fn(),
    openTestClip: vi.fn(),
  };
}

const validStep1Draft: OnboardingDraft = {
  monitor_id: 'mon-1',
  width: 1920,
  height: 1080,
  fps: 60,
  bitrate: 30000000,
};

describe('OnboardingMachine', () => {
  it('restores defaults for unset persisted fields when resuming setup', () => {
    const machine = createOnboardingMachine(createMockHost(), {
      ...validStep1Draft,
      replay_seconds: null, replay_mode: null, container: null,
      save_replay_hotkey: null, toggle_recording_hotkey: null,
      microphone: 'disabled', continuous_recording_enabled: false,
    }, 3, 2);
    const draft = machine.snapshot().draft;
    expect(draft.replay_seconds).toBe(30);
    expect(draft.replay_mode).toBe('ram');
    expect(draft.container).toBe('mp4');
    expect(draft.save_replay_hotkey?.key).toBe(0x79);
    expect(draft.microphone).toBe('disabled');
    expect(draft.continuous_recording_enabled).toBe(false);
  });

  it('initializes with default values', () => {
    const host = createMockHost();
    const machine = createOnboardingMachine(host);
    expect(machine.snapshot().step).toBe(1);
    expect(machine.snapshot().lastCompletedStep).toBe(0);
  });

  it('rejects next() if validation fails on current step', async () => {
    const host = createMockHost();
    const machine = createOnboardingMachine(host, {}, 1);
    await expect(machine.next()).rejects.toThrow('validation_failed');
    expect(machine.snapshot().step).toBe(1);
    expect(machine.snapshot().errors.length).toBeGreaterThan(0);
  });

  it('never advances before draft persistence succeeds', async () => {
    const host = createMockHost(async () => {
      const err = new Error('config.write_failed');
      (err as unknown as { code: string }).code = 'config.write_failed';
      throw err;
    });
    const machine = createOnboardingMachine(host, validStep1Draft, 1);
    await expect(machine.next()).rejects.toMatchObject({ code: 'config.write_failed' });
    expect(machine.snapshot().step).toBe(1);
  });

  it('advances step on successful validation and persistence', async () => {
    const saveMock = vi.fn().mockResolvedValue(undefined);
    const host = createMockHost(saveMock);
    const machine = createOnboardingMachine(host, validStep1Draft, 1);

    await machine.next();

    expect(saveMock).toHaveBeenCalledWith(expect.objectContaining(validStep1Draft), 1);
    expect(machine.snapshot().step).toBe(2);
    expect(machine.snapshot().lastCompletedStep).toBe(1);
  });

  it('supports back() navigation', () => {
    const host = createMockHost();
    const machine = createOnboardingMachine(host, {}, 3, 2);
    machine.back();
    expect(machine.snapshot().step).toBe(2);
    machine.back();
    expect(machine.snapshot().step).toBe(1);
    machine.back();
    expect(machine.snapshot().step).toBe(1);
  });

  it('blocks direct navigation to future incomplete steps', () => {
    const host = createMockHost();
    const machine = createOnboardingMachine(host, {}, 1, 0);
    machine.goToStep(4);
    expect(machine.snapshot().step).toBe(1);

    machine.goToStep(1);
    expect(machine.snapshot().step).toBe(1);
  });

  it('allows navigation to previously completed steps', () => {
    const host = createMockHost();
    const machine = createOnboardingMachine(host, {}, 3, 2);
    machine.goToStep(1);
    expect(machine.snapshot().step).toBe(1);
    machine.goToStep(2);
    expect(machine.snapshot().step).toBe(2);
  });

  it('updates draft and notifies listeners', () => {
    const host = createMockHost();
    const machine = createOnboardingMachine(host);
    const listener = vi.fn();
    machine.subscribe(listener);

    machine.updateDraft({ monitor_id: 'mon-new' });
    expect(machine.snapshot().draft.monitor_id).toBe('mon-new');
    expect(listener).toHaveBeenCalled();
  });
});
