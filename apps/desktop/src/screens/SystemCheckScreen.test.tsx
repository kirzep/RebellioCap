import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import React from 'react';
import { describe, expect, it, vi } from 'vitest';
import type { HostBridge } from '../bridge/contracts';
import type { SystemCheckResult } from '../config/model';
import { SystemCheckScreen } from './SystemCheckScreen';

function createMockHost(checkResult: SystemCheckResult): HostBridge {
  return {
    bootstrapApp: vi.fn(),
    getOnboardingState: vi.fn(),
    runSystemCheck: vi.fn().mockResolvedValue(checkResult),
    listMonitors: vi.fn(),
    listAudioEndpoints: vi.fn(),
    measureAudioLevel: vi.fn(),
    chooseOutputDirectory: vi.fn(),
    saveOnboardingDraft: vi.fn(),
    runRecordingTest: vi.fn(),
    completeOnboarding: vi.fn(),
    getEngineSnapshot: vi.fn(),
    startEngine: vi.fn(),
    stopEngine: vi.fn(),
    saveReplay: vi.fn(),
    toggleRecording: vi.fn(),
    applyRuntimeSettings: vi.fn(),
    openDiagnostics: vi.fn().mockResolvedValue(undefined),
    openTestClip: vi.fn(),
  };
}

describe('SystemCheckScreen', () => {
  it('renders success state and enables continue button when all stages pass', async () => {
    const mockHost = createMockHost({
      passed: true,
      stages: [
        { id: 'windows', passed: true, message: 'Windows 10/11 OK' },
        { id: 'gpu', passed: true, message: 'NVIDIA RTX OK' },
      ],
      diagnostics_path: null,
    });
    const onContinue = vi.fn();

    render(<SystemCheckScreen hostBridge={mockHost} onContinue={onContinue} />);

    await waitFor(() => {
      expect(screen.getByTestId('check-continue-button')).toBeInTheDocument();
    });

    fireEvent.click(screen.getByTestId('check-continue-button'));
    expect(onContinue).toHaveBeenCalledTimes(1);
  });

  it('renders failure state and provides retry and diagnostics buttons when a stage fails', async () => {
    const mockHost = createMockHost({
      passed: false,
      stages: [
        { id: 'windows', passed: true, message: 'Windows 10/11 OK' },
        { id: 'nvenc', passed: false, message: 'NVENC encoder not supported' },
      ],
      diagnostics_path: 'C:/diag.json',
    });

    render(<SystemCheckScreen hostBridge={mockHost} onContinue={vi.fn()} />);

    await waitFor(() => {
      expect(screen.getByTestId('check-retry-button')).toBeInTheDocument();
      expect(screen.getByTestId('check-diagnostics-button')).toBeInTheDocument();
    });

    fireEvent.click(screen.getByTestId('check-diagnostics-button'));
    expect(mockHost.openDiagnostics).toHaveBeenCalledTimes(1);
  });
});
