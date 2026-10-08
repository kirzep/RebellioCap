import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import React from 'react';
import { describe, expect, it, vi } from 'vitest';
import type { HostBridge } from '../../bridge/contracts';
import { AudioStep } from './AudioStep';
import { HotkeysStep } from './HotkeysStep';
import { PreferencesStep } from './PreferencesStep';
import { ReplayStep } from './ReplayStep';
import { TestStep } from './TestStep';
import { VideoStep } from './VideoStep';

function createMockHost(): HostBridge {
  return {
    bootstrapApp: vi.fn(),
    getOnboardingState: vi.fn(),
    runSystemCheck: vi.fn(),
    listMonitors: vi.fn().mockResolvedValue([
      { id: 'mon-0', name: 'Primary Display', width: 1920, height: 1080, primary: true },
    ]),
    listAudioEndpoints: vi.fn().mockResolvedValue({
      system_audio: [{ id: 'sys-0', name: 'Speakers', is_default: true, available: true }],
      microphones: [{ id: 'mic-0', name: 'Mic', is_default: true, available: true }],
    }),
    measureAudioLevel: vi.fn().mockResolvedValue({
      samples: [{ timestamp_ms: 100, level: 0.5 }],
      cancelled: false,
    }),
    chooseOutputDirectory: vi.fn().mockResolvedValue('C:/MyClips'),
    saveOnboardingDraft: vi.fn().mockResolvedValue(undefined),
    runRecordingTest: vi.fn().mockResolvedValue({
      fingerprint: 'fp',
      succeeded: true,
      clip_path: 'C:/MyClips/test.mp4',
      video_packets: 120,
      audio_packets: 80,
    }),
    completeOnboarding: vi.fn(),
    getEngineSnapshot: vi.fn(),
    startEngine: vi.fn(),
    stopEngine: vi.fn(),
    saveReplay: vi.fn(),
    toggleRecording: vi.fn(),
    applyRuntimeSettings: vi.fn(),
    openDiagnostics: vi.fn(),
    openTestClip: vi.fn().mockResolvedValue(undefined),
  };
}

describe('Onboarding Steps', () => {
  it('ReplayStep explains automatic shortening before settings are applied', () => {
    render(<ReplayStep draft={{ replay_seconds: 3600, bitrate: 200_000_000, replay_mode: 'ram' }}
      hostBridge={createMockHost()} onChange={vi.fn()} />);
    expect(screen.getByText(/автоматически сокращается/i)).toBeInTheDocument();
    expect(screen.getByText(/сохранённый клип может быть короче/i)).toBeInTheDocument();
  });
  it('VideoStep updates resolution and presets', async () => {
    const host = createMockHost();
    const onChange = vi.fn();
    render(<VideoStep draft={{ monitor_id: 'mon-0', width: 1280, height: 720, fps: 30 }} hostBridge={host} onChange={onChange} />);

    expect(screen.getByTestId('video-step')).toBeInTheDocument();
    await waitFor(() => expect(screen.getByTestId('video-monitor-select')).toBeEnabled());
    onChange.mockClear();
    fireEvent.click(screen.getByRole('radio', { name: 'Сбалансированное' }));
    expect(onChange).toHaveBeenCalledWith(expect.objectContaining({ width: 1920, height: 1080, fps: 60 }));
  });

  it('AudioStep measures audio level on demand', async () => {
    const host = createMockHost();
    const onChange = vi.fn();
    render(
      <AudioStep
        draft={{ microphone: { endpoint: 'mic-0' } }}
        hostBridge={host}
        onChange={onChange}
      />
    );

    const checkBtn = screen.getByTestId('audio-check-button');
    await waitFor(() => expect(checkBtn).toBeEnabled());
    fireEvent.click(checkBtn);
    expect(host.measureAudioLevel).toHaveBeenCalledWith('mic-0');
  });

  it('ReplayStep triggers folder chooser', async () => {
    const host = createMockHost();
    const onChange = vi.fn();
    render(
      <ReplayStep
        draft={{ output_directory: 'C:/default' }}
        hostBridge={host}
        onChange={onChange}
      />
    );

    fireEvent.click(screen.getByTestId('choose-folder-button'));
    expect(host.chooseOutputDirectory).toHaveBeenCalled();
  });

  it('HotkeysStep allows binding hotkeys', () => {
    const onChange = vi.fn();
    render(
      <HotkeysStep
        draft={{
          save_replay_hotkey: { key: 0x77, ctrl: false, alt: false, shift: false, win: false },
          toggle_recording_hotkey: { key: 0x52, ctrl: true, alt: false, shift: true, win: false },
        }}
        onChange={onChange}
      />
    );

    expect(screen.getByTestId('hotkeys-step')).toBeInTheDocument();
    fireEvent.click(screen.getByTestId('hotkeys-reset-button'));
    expect(onChange).toHaveBeenCalled();
  });

  it('PreferencesStep lets the user return from the summary to video settings', () => {
    const onChange = vi.fn();
    const onEditStep = vi.fn();
    render(
      <PreferencesStep
        draft={{ preferences: { overlay_enabled: true, start_with_windows: false } }}
        onChange={onChange}
        onEditStep={onEditStep}
      />
    );

    fireEvent.click(screen.getByTestId('summary-edit-step-1'));
    expect(onEditStep).toHaveBeenCalledWith(1);
    expect(onChange).not.toHaveBeenCalled();
  });

  it('PreferencesStep initializes only setup preferences without a second overlay flag', () => {
    const onChange=vi.fn();
    render(<PreferencesStep draft={{}} onChange={onChange}/>);
    expect(onChange).toHaveBeenCalledWith({preferences:{start_with_windows:false}});
  });

  it('TestStep executes recording test and permits completion', async () => {
    const host = createMockHost();
    const onComplete = vi.fn();
    render(<TestStep draft={{ width: 1920, height: 1080 }} hostBridge={host} onComplete={onComplete} />);

    const runBtn = screen.getByTestId('run-test-button');
    fireEvent.click(runBtn);
    expect(host.runRecordingTest).toHaveBeenCalled();
  });
});
