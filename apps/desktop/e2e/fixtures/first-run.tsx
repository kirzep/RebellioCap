import React from 'react';
import { createRoot } from 'react-dom/client';
import { App } from '../../src/app/App';
import type { HostBridge } from '../../src/bridge/contracts';
import type { ActiveConfig, EngineSnapshot, StoredState } from '../../src/config/model';
import { validateCompleteDraft } from '../../src/config/validation';
import '../../src/styles/global.css';

// Browser-only host: this exercises the real App and wizard without claiming
// coverage of native IPC, devices, recording, or installation.
const state: StoredState = JSON.parse(sessionStorage.getItem('first-run-state') ?? 'null') ?? {
  schema_version: 1,
  onboarding: { completed: false, last_completed_step: 0 },
  draft: {}, active: null, last_successful_test: null,
};
const snapshot: EngineSnapshot = {
  revision: 1, lifecycle: 'stopped', replayActive: false,
  continuousRecordingActive: false, replaySeconds: 30, lastError: null,
  metrics: {
    videoTicks: 0, missedVideoDeadlines: 0, videoPackets: 0, audioPackets: 0,
    saveRequests: 0, completedSaves: 0, failedSaves: 0, rejectedSaves: 0,
    pipelineErrors: 0, continuousPackets: 0, continuousFailures: 0,
    continuousRecordingActive: false, lastHotkeySaveLatencyTicks: 0, replayBytes: 0,
  },
};
const persist = () => sessionStorage.setItem('first-run-state', JSON.stringify(state));
if (state.onboarding.completed) { snapshot.lifecycle = 'ready'; snapshot.replayActive = true; }
const result = () => structuredClone({ route: state.onboarding.completed ? 'home' : 'start', state, snapshot });
const fingerprint = async () => Array.from(new Uint8Array(await crypto.subtle.digest(
  'SHA-256', new TextEncoder().encode(JSON.stringify(state.draft))
)), byte => byte.toString(16).padStart(2, '0')).join('');
const unsupported = async (): Promise<never> => { throw new Error('Unexpected first-run host operation'); };
const host: HostBridge = {
  bootstrapApp: async () => result(),
  getOnboardingState: async () => structuredClone(state),
  runSystemCheck: async () => ({ passed: true, diagnostics_path: null,
    stages: [{ id: 'native_hardware_and_runtime', passed: true, message: '' }] }),
  listMonitors: async () => [{ id: 'mon-0', name: 'Test monitor', width: 1920, height: 1080, primary: true }],
  listAudioEndpoints: async () => ({ system_audio: [], microphones: [] }),
  measureAudioLevel: async () => ({ samples: [], cancelled: true }),
  chooseOutputDirectory: async () => 'C:/Clips',
  saveOnboardingDraft: async (draft, lastCompletedStep) => {
    state.draft = structuredClone(draft);
    state.onboarding.last_completed_step = lastCompletedStep;
    state.last_successful_test = null;
    persist();
  },
  runRecordingTest: async () => {
    if (validateCompleteDraft(state.draft).length) throw new Error('Incomplete test draft');
    state.last_successful_test = { fingerprint: await fingerprint(), succeeded: true,
      clip_path: 'C:/Clips/test.mp4', video_packets: 300, audio_packets: 0 };
    persist();
    return structuredClone(state.last_successful_test);
  },
  completeOnboarding: async explicit => {
    if (!explicit || state.last_successful_test?.fingerprint !== await fingerprint()) {
      throw new Error('Successful recording test and explicit completion required');
    }
    state.active = { ...state.draft, onboarding_completed: true } as ActiveConfig;
    state.onboarding = { completed: true, last_completed_step: 6 };
    snapshot.lifecycle = 'ready'; snapshot.replayActive = true; snapshot.revision++;
    persist();
    return result();
  },
  getEngineSnapshot: async () => structuredClone(snapshot),
  listClips: async () => [],
  startEngine: unsupported, stopEngine: unsupported,
  saveReplay: async () => { snapshot.metrics.completedSaves++; snapshot.revision++; return structuredClone(snapshot); },
  toggleRecording: async () => { snapshot.continuousRecordingActive = !snapshot.continuousRecordingActive; snapshot.revision++; return structuredClone(snapshot); },
  applyRuntimeSettings: async candidate => { state.active = structuredClone(candidate); persist(); snapshot.revision++; return structuredClone(snapshot); },
  openDiagnostics: unsupported, openTestClip: unsupported,
};
Object.assign(window, { firstRunState: state, firstRunSnapshot: snapshot });
createRoot(document.getElementById('root')!).render(<React.StrictMode><App hostBridge={host} /></React.StrictMode>);
