import { fireEvent, render, screen } from '@testing-library/react';
import React from 'react';
import { describe, expect, it, vi } from 'vitest';
import type { EngineSnapshot } from '../config/model';
import { EngineState } from './EngineState';

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
      saveRequests: 1,
      completedSaves: 1,
      failedSaves: 0,
      rejectedSaves: 0,
      pipelineErrors: 0,
      continuousPackets: 0,
      continuousFailures: 0,
      continuousRecordingActive: false,
      lastHotkeySaveLatencyTicks: 12,
      replayBytes: 15_000_000,
    },
    lastError: null,
    ...overrides,
  };
}

describe('EngineState', () => {
  it('renders ready lifecycle and active replay state', () => {
    const snapshot = createSnapshot();
    render(<EngineState snapshot={snapshot} />);

    expect(screen.getByTestId('engine-lifecycle-badge')).toHaveTextContent('Replay включён');
    expect(screen.queryByText('Обычная запись идёт')).not.toBeInTheDocument();
  });

  it('renders continuous recording active state', () => {
    const snapshot = createSnapshot({ continuousRecordingActive: true });
    render(<EngineState snapshot={snapshot} />);

    expect(screen.getByText('Обычная запись идёт')).toBeInTheDocument();
  });

  it('renders last error banner and handles diagnostics trigger', () => {
    const onOpenDiagnostics = vi.fn();
    const snapshot = createSnapshot({
      lifecycle: 'failed',
      lastError: {
        code: 'DXGI_ERROR_DEVICE_REMOVED',
        message: 'Графический процессор был перезапущен',
        hresult: -2005270523,
      },
    });

    render(<EngineState snapshot={snapshot} onOpenDiagnostics={onOpenDiagnostics} />);

    expect(screen.getByTestId('engine-error-banner')).toHaveTextContent('DXGI_ERROR_DEVICE_REMOVED');
    const diagBtn = screen.getByTestId('engine-diagnostics-button');
    fireEvent.click(diagBtn);
    expect(onOpenDiagnostics).toHaveBeenCalled();
  });
});
