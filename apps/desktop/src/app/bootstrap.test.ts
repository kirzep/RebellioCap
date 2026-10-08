import { describe, expect, it } from 'vitest';
import type { HostBridge } from '../bridge/contracts';
import type { Bootstrap, EngineSnapshot, StoredState } from '../config/model';
import { bootstrap } from './bootstrap';

function createFakeHost(options: {
  completed: boolean;
  lastCompletedStep: number;
  lifecycle?: EngineSnapshot['lifecycle'];
  hasActive?: boolean;
}): HostBridge {
  const defaultSnapshot: EngineSnapshot = {
    revision: 1,
    lifecycle: options.lifecycle ?? 'ready',
    replayActive: true,
    continuousRecordingActive: false,
    replaySeconds: 30,
    metrics: {
      videoTicks: 0,
      missedVideoDeadlines: 0,
      videoPackets: 0,
      audioPackets: 0,
      saveRequests: 0,
      completedSaves: 0,
      failedSaves: 0,
      rejectedSaves: 0,
      pipelineErrors: 0,
      continuousPackets: 0,
      continuousFailures: 0,
      continuousRecordingActive: false,
      lastHotkeySaveLatencyTicks: 0,
      replayBytes: 0,
    },
    lastError: options.lifecycle === 'blocked' ? { code: 'host.blocked', message: 'device lost', hresult: null } : null,
  };

  const defaultState: StoredState = {
    schema_version: 1,
    onboarding: {
      completed: options.completed,
      last_completed_step: options.lastCompletedStep,
    },
    draft: {},
    active: options.hasActive ?? options.completed
      ? {
          onboarding_completed: true,
          monitor_id: 'mon-1',
          system_audio: 'disabled',
          microphone: 'disabled',
          width: 1920,
          height: 1080,
          fps: 60,
          bitrate: 30000000,
          replay_seconds: 30,
          replay_mode: 'ram',
          container: 'mp4',
          output_directory: 'C:/clips',
          save_replay_hotkey: { key: 0x77, ctrl: false, alt: false, shift: false, win: false },
          toggle_recording_hotkey: { key: 0x52, ctrl: true, alt: false, shift: true, win: false },
          preferences: { overlay_enabled: true, start_with_windows: false },
          continuous_recording_enabled: false,
        }
      : null,
    last_successful_test: null,
  };

  const bootstrapData: Bootstrap = {
    route: options.completed ? 'home' : 'onboarding',
    state: defaultState,
    snapshot: defaultSnapshot,
  };

  return {
    bootstrapApp: async () => bootstrapData,
    getOnboardingState: async () => defaultState,
    runSystemCheck: async () => ({ stages: [], passed: true, diagnostics_path: null }),
    listMonitors: async () => [],
    listAudioEndpoints: async () => ({ system_audio: [], microphones: [] }),
    measureAudioLevel: async () => ({ samples: [], cancelled: false }),
    chooseOutputDirectory: async () => null,
    saveOnboardingDraft: async () => {},
    runRecordingTest: async () => ({
      fingerprint: 'test',
      succeeded: true,
      clip_path: 'clip.mp4',
      video_packets: 10,
      audio_packets: 10,
    }),
    completeOnboarding: async () => bootstrapData,
    getEngineSnapshot: async () => defaultSnapshot,
    startEngine: async () => defaultSnapshot,
    stopEngine: async () => defaultSnapshot,
    saveReplay: async () => defaultSnapshot,
    toggleRecording: async () => defaultSnapshot,
    applyRuntimeSettings: async () => defaultSnapshot,
    openDiagnostics: async () => {},
    openTestClip: async () => {},
  };
}

describe('bootstrap', () => {
  it('navigates clean profile to start route', async () => {
    const host = createFakeHost({ completed: false, lastCompletedStep: 0 });
    const result = await bootstrap(host);
    expect(result.route).toEqual({ kind: 'start' });
  });

  it('resumes the first incomplete onboarding step', async () => {
    const host = createFakeHost({ completed: false, lastCompletedStep: 2 });
    const saved = await host.getOnboardingState();
    saved.draft = { monitor_id: 'mon-1', width: 1920, height: 1080, fps: 60,
      bitrate: 30000000, system_audio: 'disabled', microphone: 'disabled' };
    host.listMonitors = async () => [{ id: 'mon-1', name: 'Display', width: 1920, height: 1080, primary: true }];
    const result = await bootstrap(host);
    expect(result.route).toEqual({ kind: 'onboarding', step: 3 });
  });

  it('sends completed profile to home route', async () => {
    const host = createFakeHost({ completed: true, lastCompletedStep: 6, hasActive: true });
    const result = await bootstrap(host);
    expect(result.route).toEqual({ kind: 'home' });
  });

  it('keeps a completed profile accessible with a repair action when the engine is blocked', async () => {
    const host = createFakeHost({
      completed: true,
      lastCompletedStep: 6,
      hasActive: true,
      lifecycle: 'blocked',
    });
    const result = await bootstrap(host);
    expect(result.route).toMatchObject({ kind: 'home', repair: { error: { code: 'host.blocked' } } });
  });

  it('returns to an invalid completed step instead of trusting the saved step number', async () => {
    const host = createFakeHost({ completed: false, lastCompletedStep: 2 });
    expect((await bootstrap(host)).route).toEqual({ kind: 'onboarding', step: 1 });
  });

  it('sends completed profile with missing active config to blocked route', async () => {
    const host = createFakeHost({
      completed: true,
      lastCompletedStep: 6,
      hasActive: false,
    });
    const result = await bootstrap(host);
    expect(result.route.kind).toBe('blocked');
  });
});
