import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import React from 'react';
import { describe, expect, it, vi } from 'vitest';
import type { HostBridge } from '../bridge/contracts';
import type { EngineSnapshot, StoredState } from '../config/model';
import { HomeScreen } from './HomeScreen';

function createSnapshot(overrides: Partial<EngineSnapshot> = {}): EngineSnapshot {
  return {
    revision: 1,
    lifecycle: 'ready',
    replayActive: true,
    continuousRecordingActive: false,
    replaySeconds: 30,
    metrics: {
      videoTicks: 100,
      missedVideoDeadlines: 0,
      videoPackets: 1800,
      audioPackets: 600,
      saveRequests: 0,
      completedSaves: 0,
      failedSaves: 0,
      rejectedSaves: 0,
      pipelineErrors: 0,
      continuousPackets: 0,
      continuousFailures: 0,
      continuousRecordingActive: false,
      lastHotkeySaveLatencyTicks: 0,
      replayBytes: 15_000_000,
    },
    lastError: null,
    ...overrides,
  };
}

function createStoredState(): StoredState {
  return {
    schema_version: 1,
    onboarding: { completed: true, last_completed_step: 6 },
    draft: {},
    active: {
      onboarding_completed: true,
      monitor_id: 'mon-0',
      system_audio: { endpoint: 'sys-0' },
      microphone: { endpoint: 'mic-0' },
      width: 1920,
      height: 1080,
      fps: 60,
      bitrate: 12_000_000,
      replay_seconds: 30,
      replay_mode: 'ram',
      container: 'mp4',
      output_directory: 'C:/Clips',
      save_replay_hotkey: { key: 0x77, ctrl: false, alt: false, shift: false, win: false },
      toggle_recording_hotkey: { key: 0x52, ctrl: true, alt: false, shift: true, win: false },
      preferences: { overlay_enabled: true, start_with_windows: false },
      continuous_recording_enabled: true,
    },
    last_successful_test: null,
  };
}

interface TestHost extends HostBridge {
  publish(snapshot: EngineSnapshot): void;
  subscribe(listener: (snapshot: EngineSnapshot) => void): () => void;
}

function createTestHost(initialSnap: EngineSnapshot): TestHost {
  const listeners = new Set<(s: EngineSnapshot) => void>();
  let currentSnap = { ...initialSnap };

  const host: TestHost = {
    bootstrapApp: vi.fn(),
    getOnboardingState: vi.fn().mockResolvedValue(createStoredState()),
    runSystemCheck: vi.fn(),
    listMonitors: vi.fn().mockResolvedValue([{id:'mon-0',name:'Экран',width:1920,height:1080,primary:true}]),
    listAudioEndpoints: vi.fn().mockResolvedValue({system_audio:[{id:'sys-0',name:'Система',available:true}],microphones:[{id:'mic-0',name:'Микрофон',available:true}]}),
    measureAudioLevel: vi.fn().mockResolvedValue({peak:0}),
    listClips: vi.fn().mockResolvedValue([]),
    chooseOutputDirectory: vi.fn(),
    saveOnboardingDraft: vi.fn(),
    runRecordingTest: vi.fn(),
    completeOnboarding: vi.fn(),
    getEngineSnapshot: vi.fn().mockImplementation(() => Promise.resolve(currentSnap)),
    startEngine: vi.fn().mockImplementation(() => Promise.resolve(currentSnap)),
    stopEngine: vi.fn().mockImplementation(() => Promise.resolve(currentSnap)),
    saveReplay: vi.fn().mockImplementation(() => {
      currentSnap = {
        ...currentSnap,
        revision: currentSnap.revision + 1,
        metrics: { ...currentSnap.metrics, completedSaves: currentSnap.metrics.completedSaves + 1 },
      };
      return Promise.resolve(currentSnap);
    }),
    toggleRecording: vi.fn().mockImplementation(() => {
      // Toggle pending resolution
      return Promise.resolve(currentSnap);
    }),
    applyRuntimeSettings: vi.fn().mockImplementation((cand) => {
      currentSnap = { ...currentSnap, revision: currentSnap.revision + 1, replaySeconds: cand.replay_seconds };
      return Promise.resolve(currentSnap);
    }),
    openDiagnostics: vi.fn(),
    openTestClip: vi.fn(),
    publish(next) {
      currentSnap = next;
      for (const listener of listeners) {
        listener(next);
      }
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };

  return host;
}

describe('HomeScreen', () => {
  it('removes the snapshot timer while hidden and refreshes immediately on return', async () => {
    vi.useFakeTimers();
    let visibility: DocumentVisibilityState = 'visible';
    const visibilitySpy = vi.spyOn(document, 'visibilityState', 'get').mockImplementation(() => visibility);
    const intervals = vi.spyOn(window, 'setInterval');
    const clearInterval = vi.spyOn(window, 'clearInterval');
    const host = createTestHost(createSnapshot());
    const view = render(<HomeScreen host={host} initialState={createStoredState()} initialSnapshot={createSnapshot()} />);
    try {
      await act(async () => {});
      const pollIndex = intervals.mock.calls.findIndex(call => call[1] === 1000);
      expect(pollIndex).toBeGreaterThanOrEqual(0);
      const pollTimer = intervals.mock.results[pollIndex].value;
      visibility = 'hidden';
      act(() => document.dispatchEvent(new Event('visibilitychange')));
      expect(clearInterval).toHaveBeenCalledWith(pollTimer);
      const hiddenCalls = vi.mocked(host.getEngineSnapshot).mock.calls.length;
      await act(async () => {
        window.dispatchEvent(new Event('focus'));
        await vi.advanceTimersByTimeAsync(5000);
      });
      expect(host.getEngineSnapshot).toHaveBeenCalledTimes(hiddenCalls);
      visibility = 'visible';
      await act(async () => document.dispatchEvent(new Event('visibilitychange')));
      expect(host.getEngineSnapshot).toHaveBeenCalledTimes(hiddenCalls + 1);
      // Repeated visibility events must not install additional intervals.
      await act(async () => document.dispatchEvent(new Event('visibilitychange')));
      const resumedCalls = vi.mocked(host.getEngineSnapshot).mock.calls.length;
      await act(async () => vi.advanceTimersByTimeAsync(1000));
      expect(host.getEngineSnapshot).toHaveBeenCalledTimes(resumedCalls + 1);
      view.unmount();
      const finalCalls = vi.mocked(host.getEngineSnapshot).mock.calls.length;
      await act(async () => vi.advanceTimersByTimeAsync(2000));
      expect(host.getEngineSnapshot).toHaveBeenCalledTimes(finalCalls);
    } finally {
      view.unmount();
      intervals.mockRestore(); clearInterval.mockRestore(); visibilitySpy.mockRestore();
      vi.useRealTimers();
    }
  });

  it('defers initial snapshot polling when the window opens hidden', async () => {
    vi.useFakeTimers();
    let visibility: DocumentVisibilityState = 'hidden';
    const visibilitySpy = vi.spyOn(document, 'visibilityState', 'get').mockImplementation(() => visibility);
    const intervals = vi.spyOn(window, 'setInterval');
    const host = createTestHost(createSnapshot());
    const view = render(<HomeScreen host={host} initialState={createStoredState()} initialSnapshot={createSnapshot()} />);
    try {
      await act(async () => vi.advanceTimersByTimeAsync(3000));
      expect(host.getEngineSnapshot).not.toHaveBeenCalled();
      expect(intervals.mock.calls.filter(call => call[1] === 1000)).toHaveLength(0);
      visibility = 'visible';
      await act(async () => document.dispatchEvent(new Event('visibilitychange')));
      expect(host.getEngineSnapshot).toHaveBeenCalledTimes(1);
    } finally {
      view.unmount(); intervals.mockRestore(); visibilitySpy.mockRestore(); vi.useRealTimers();
    }
  });

  it('does not drain a queued refresh in hidden StrictMode and resumes without overlap', async () => {
    vi.useFakeTimers();
    let visibility: DocumentVisibilityState = 'visible';
    const visibilitySpy = vi.spyOn(document, 'visibilityState', 'get').mockImplementation(() => visibility);
    const host = createTestHost(createSnapshot());
    let resolveSnapshot!: (snapshot: EngineSnapshot) => void;
    vi.mocked(host.getEngineSnapshot).mockImplementationOnce(() => new Promise(resolve => { resolveSnapshot = resolve; }));
    const view = render(<React.StrictMode><HomeScreen host={host} initialState={createStoredState()} initialSnapshot={createSnapshot()} /></React.StrictMode>);
    try {
      await act(async () => {
        window.dispatchEvent(new Event('focus'));
        await vi.advanceTimersByTimeAsync(3000);
      });
      expect(host.getEngineSnapshot).toHaveBeenCalledTimes(1);
      visibility = 'hidden';
      await act(async () => {
        document.dispatchEvent(new Event('visibilitychange'));
        resolveSnapshot(createSnapshot());
        await vi.advanceTimersByTimeAsync(3000);
      });
      expect(host.getEngineSnapshot).toHaveBeenCalledTimes(1);
      visibility = 'visible';
      await act(async () => document.dispatchEvent(new Event('visibilitychange')));
      expect(host.getEngineSnapshot).toHaveBeenCalledTimes(2);
      await act(async () => vi.advanceTimersByTimeAsync(1000));
      expect(host.getEngineSnapshot).toHaveBeenCalledTimes(3);
    } finally {
      view.unmount(); visibilitySpy.mockRestore(); vi.useRealTimers();
    }
  });

  it('renders the overview and confirmed engine status', async () => {
    const host = createTestHost(createSnapshot({ replayActive: true }));

    render(<HomeScreen host={host} />);

    expect(screen.getByRole('heading', { name: 'Обзор записи' })).toBeInTheDocument();
    expect(await screen.findByText('Replay включён')).toBeInTheDocument();
  });

  it('does not show recording active until the engine confirms it', async () => {
    const host = createTestHost(createSnapshot({ continuousRecordingActive: false }));

    render(<HomeScreen host={host} />);

    const toggleButton = await screen.findByRole('button', { name: 'Начать запись' });
    fireEvent.click(toggleButton);

    // Still shows inactive until host publishes confirmed engine snapshot
    expect(screen.queryByText('Обычная запись идёт')).not.toBeInTheDocument();

    // Confirm new revision from engine
    act(() => host.publish(createSnapshot({ revision: 2, continuousRecordingActive: true })));
    act(() => window.dispatchEvent(new Event('focus')));

    expect(await screen.findByText('Обычная запись идёт')).toBeInTheDocument();
  });

  it('opens the editor from the sidebar and returns with Back', async () => {
    const host = createTestHost(createSnapshot());
    vi.stubGlobal('ResizeObserver',class {observe(){} disconnect(){}});
    render(<HomeScreen host={host} initialState={createStoredState()} initialSnapshot={createSnapshot()} />);
    fireEvent.click(await screen.findByRole('button', {name:'Редактор клипов'}));
    expect(await screen.findByRole('dialog', {name:'Редактор клипов'})).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', {name:'Назад'}));
    expect(screen.queryByRole('dialog', {name:'Редактор клипов'})).not.toBeInTheDocument();
  });

  it('keeps dirty editor mounted when settings navigation is cancelled', async () => {
    const host = createTestHost(createSnapshot());
    vi.stubGlobal('ResizeObserver',class {observe(){} disconnect(){}});
    render(<HomeScreen host={host} initialState={createStoredState()} initialSnapshot={createSnapshot()} />);
    fireEvent.click(await screen.findByRole('button', {name:'Редактор клипов'}));
    await screen.findByRole('dialog', {name:'Редактор клипов'});
    fireEvent.change(screen.getByLabelText('Название проекта'), {target:{value:'Unsaved edit'}});
    fireEvent.click(screen.getByRole('button', {name:'Настройки'}));
    expect(screen.getByText('Сохранить монтаж?')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button',{name:'Продолжить монтаж'}));
    expect(screen.getByLabelText('Название проекта')).toHaveValue('Unsaved edit');
    fireEvent.click(screen.getByRole('button', {name:'Настройки'}));
    fireEvent.click(screen.getByRole('button',{name:'Закрыть без сохранения'}));
    expect(screen.queryByRole('dialog',{name:'Редактор клипов'})).not.toBeInTheDocument();
  });

  it('saves replay when save button is clicked', async () => {
    const host = createTestHost(createSnapshot({ replayActive: true }));

    render(<HomeScreen host={host} />);

    const saveButton = await screen.findByTestId('save-replay-button');
    fireEvent.click(saveButton);

    await waitFor(() => {
      expect(host.saveReplay).toHaveBeenCalled();
    });
  });

  it('opens and applies runtime settings', async () => {
    const host = createTestHost(createSnapshot());

    render(<HomeScreen host={host} initialState={createStoredState()} initialSnapshot={createSnapshot()} />);

    const settingsButton = screen.getByRole('button', {name:'Настройки'});
    fireEvent.click(settingsButton);
    fireEvent.click(screen.getByRole('radio', {name:'Повторы и файлы'}));

    expect(screen.getByTestId('runtime-settings')).toBeInTheDocument();
    fireEvent.keyDown(screen.getByLabelText('Длительность Replay'), {key:'Enter'});
    fireEvent.click(await screen.findByRole('option', {name:'60 секунд'}));
    const applyButton = screen.getByTestId('apply-settings-button');
    fireEvent.click(applyButton);

    await waitFor(() => {
      expect(host.applyRuntimeSettings).toHaveBeenCalled();
    });
  });
});


it('rejects settings success when the persisted memory limit differs from the candidate', async () => {
  const host = createTestHost(createSnapshot());
  render(<HomeScreen host={host} initialState={createStoredState()} initialSnapshot={createSnapshot()} />);
  fireEvent.click(screen.getByRole('button', {name:'Настройки'}));
  fireEvent.click(screen.getByRole('radio', {name:'Повторы и файлы'}));
  fireEvent.keyDown(screen.getByLabelText('Лимит памяти Replay'), {key:'Enter'});
  fireEvent.click(await screen.findByRole('option', {name:'Задать вручную'}));
  fireEvent.click(screen.getByTestId('apply-settings-button'));
  await waitFor(() => expect(host.applyRuntimeSettings).toHaveBeenCalled());
  expect(await screen.findByText(/не удалось подтвердить сохранённую конфигурацию/, {selector:"p"})).toBeInTheDocument();
  expect(screen.queryByText('Настройки применены. Replay включён с новой конфигурацией.')).not.toBeInTheDocument();
});


it('confirms explicit Auto memory against a legacy persisted configuration without the field', async () => {
  const host = createTestHost(createSnapshot());
  const state = createStoredState();
  state.active!.replay_memory_limit_mb = 1024;
  let confirmedState = state;
  host.getOnboardingState = vi.fn().mockImplementation(() => Promise.resolve(confirmedState));
  host.applyRuntimeSettings = vi.fn().mockImplementation(() => { confirmedState = createStoredState(); return Promise.resolve(createSnapshot()); });
  render(<HomeScreen host={host} initialState={state} initialSnapshot={createSnapshot()} />);
  fireEvent.click(screen.getByRole('button', {name:'Настройки'}));
  fireEvent.click(screen.getByRole('radio', {name:'Повторы и файлы'}));
  fireEvent.keyDown(screen.getByLabelText('Лимит памяти Replay'), {key:'Enter'});
  fireEvent.click(await screen.findByRole('option', {name:/^Авто$/}));
  fireEvent.click(screen.getByTestId('apply-settings-button'));
  expect(await screen.findByText('Настройки применены. Replay включён с новой конфигурацией.')).toBeInTheDocument();
});
