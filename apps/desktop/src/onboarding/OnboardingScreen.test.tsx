import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import React from 'react';
import { describe, expect, it, vi } from 'vitest';
import type { HostBridge } from '../bridge/contracts';
import { OnboardingScreen } from './OnboardingScreen';

function createMockHost(): HostBridge {
  return {
    bootstrapApp: vi.fn(),
    getOnboardingState: vi.fn().mockResolvedValue({
      completed: false,
      last_completed_step: 0,
      draft: {},
    }),
    runSystemCheck: vi.fn().mockResolvedValue({ passed: true, stages: [], diagnostics_path: 'C:/diag.json' }),
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
    chooseOutputDirectory: vi.fn().mockResolvedValue('C:/Clips'),
    saveOnboardingDraft: vi.fn().mockResolvedValue(undefined),
    runRecordingTest: vi.fn().mockResolvedValue({
      fingerprint: 'a'.repeat(64),
      succeeded: true,
      clip_path: 'C:/Clips/test.mp4',
      video_packets: 120,
      audio_packets: 80,
    }),
    completeOnboarding: vi.fn().mockResolvedValue({
      route: 'home',
      state: { onboarding: { completed: true, last_completed_step: 6 }, draft: {}, active: { onboarding_completed: true } },
      snapshot: { lifecycle: 'ready', replayActive: true, continuousRecordingActive: false },
    }),
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

describe('OnboardingScreen', () => {
  it('renders initial step (VideoStep) and step counter', () => {
    const host = createMockHost();
    const onComplete = vi.fn();
    const onBackToStart = vi.fn();

    render(
      <OnboardingScreen
        hostBridge={host}
        initialStep={1}
        initialLastCompletedStep={0}
        onComplete={onComplete}
        onBackToStart={onBackToStart}
      />
    );

    expect(screen.getByTestId('step-counter')).toHaveTextContent('Шаг 1 из 6');
    expect(screen.getByTestId('video-step')).toBeInTheDocument();
    expect(screen.getByTestId('onboarding-next-button')).toBeInTheDocument();
  });

  it('calls onBackToStart when back button clicked on step 1', () => {
    const host = createMockHost();
    const onBackToStart = vi.fn();

    render(
      <OnboardingScreen
        hostBridge={host}
        initialStep={1}
        onComplete={vi.fn()}
        onBackToStart={onBackToStart}
      />
    );

    fireEvent.click(screen.getByTestId('onboarding-back-button'));
    expect(onBackToStart).toHaveBeenCalled();
  });

  it('advances from step 1 to step 2 after clicking next with valid draft', async () => {
    const host = createMockHost();

    render(
      <OnboardingScreen
        hostBridge={host}
        initialStep={1}
        initialDraft={{
          monitor_id: 'mon-0',
          width: 1920,
          height: 1080,
          fps: 60,
          bitrate: 12_000_000,
        }}
        onComplete={vi.fn()}
      />
    );

    await waitFor(() => expect(screen.getByTestId('onboarding-next-button')).not.toBeDisabled());
    fireEvent.click(screen.getByTestId('onboarding-next-button'));

    await waitFor(() => {
      expect(screen.getByTestId('step-counter')).toHaveTextContent('Шаг 2 из 6');
      expect(screen.getByTestId('audio-step')).toBeInTheDocument();
    });
  });

  it('navigates back to step 1 from step 2 via back button', async () => {
    const host = createMockHost();

    render(
      <OnboardingScreen
        hostBridge={host}
        initialStep={2}
        initialLastCompletedStep={1}
        onComplete={vi.fn()}
      />
    );

    expect(screen.getByTestId('step-counter')).toHaveTextContent('Шаг 2 из 6');
    fireEvent.click(screen.getByTestId('onboarding-back-button'));

    expect(screen.getByTestId('step-counter')).toHaveTextContent('Шаг 1 из 6');
  });

  it('completes onboarding when running test and enabling Replay on step 6', async () => {
    const host = createMockHost();
    const onComplete = vi.fn();

    render(
      <OnboardingScreen
        hostBridge={host}
        initialStep={6}
        initialLastCompletedStep={5}
        initialDraft={{
          width: 1920,
          height: 1080,
          fps: 60,
          output_directory: 'C:/Clips',
        }}
        onComplete={onComplete}
      />
    );

    expect(screen.getByTestId('step-counter')).toHaveTextContent('Шаг 6 из 6');
    expect(screen.getByTestId('test-step')).toBeInTheDocument();

    expect(screen.queryByTestId('complete-onboarding-button')).not.toBeInTheDocument();

    fireEvent.click(screen.getByTestId('run-test-button'));

    await waitFor(() => {
      expect(screen.getByTestId('test-success-summary')).toBeInTheDocument();
      expect(screen.getByTestId('complete-onboarding-button')).not.toBeDisabled();
    });

    fireEvent.click(screen.getByTestId('complete-onboarding-button'));

    await waitFor(() => {
      expect(host.completeOnboarding).toHaveBeenCalledWith(true);
      expect(onComplete).toHaveBeenCalled();
    });
  });

  it('shows error when test fails and provides diagnostics', async () => {
    const host = createMockHost();
    host.runRecordingTest = vi.fn().mockRejectedValue(new Error('Тест не пройден: сбой кодировщика'));

    render(
      <OnboardingScreen
        hostBridge={host}
        initialStep={6}
        initialLastCompletedStep={5}
        onComplete={vi.fn()}
      />
    );

    fireEvent.click(screen.getByTestId('run-test-button'));

    await waitFor(() => {
      expect(screen.getByTestId('test-error-message')).toHaveTextContent('Тест не пройден: сбой кодировщика');
      expect(screen.queryByTestId('complete-onboarding-button')).not.toBeInTheDocument();
    });

    fireEvent.click(screen.getByTestId('test-diagnostics-button'));
    await waitFor(() => expect(host.openDiagnostics).toHaveBeenCalled());
  });
});
