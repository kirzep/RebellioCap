import { fireEvent, render, screen } from '@testing-library/react';
import React from 'react';
import { describe, expect, it, vi } from 'vitest';
import type { EngineSnapshot } from '../config/model';
import { RecordingControls } from './RecordingControls';
import { createDraftDefaults } from '../config/defaults';
import type { ActiveConfig } from '../config/model';

const activeConfig: ActiveConfig = {
  ...createDraftDefaults(), onboarding_completed: true, monitor_id: 'mon-0',
  width: 1920, height: 1080, fps: 60, bitrate: 12_000_000,
  system_audio: 'disabled', microphone: 'disabled', output_directory: 'C:/Clips',
} as ActiveConfig;
const lifecycleActions = { activeConfig, onStartReplay: vi.fn(), onStopReplay: vi.fn() };

function createSnapshot(overrides: Partial<EngineSnapshot> = {}): EngineSnapshot {
  return {
    revision: 1,
    lifecycle: 'ready',
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
    lastError: null,
    ...overrides,
  };
}

describe('RecordingControls', () => {
  it('shows the retained recording path after an asynchronous writer failure', () => {
    const snapshot = createSnapshot();
    snapshot.metrics.continuousFailures = 1;
    snapshot.metrics.continuousRecoveryPath = 'C:/Clips/Запись.mp4.partial';
    render(<RecordingControls {...lifecycleActions} snapshot={snapshot}
      onSaveReplay={vi.fn()} onToggleContinuous={vi.fn()} />);
    expect(screen.getByText('C:/Clips/Запись.mp4.partial')).toBeVisible();
    expect(screen.getByText('Сохранена аварийная копия записи')).toBeVisible();
  });
  it('can start recording with Replay disabled and keeps Replay controls available during recording', () => {
    const toggle = vi.fn();
    const { rerender } = render(<RecordingControls {...lifecycleActions}
      snapshot={createSnapshot({ lifecycle: 'stopped', replayActive: false })}
      onSaveReplay={vi.fn()} onToggleContinuous={toggle} />);
    expect(screen.getByTestId('toggle-recording-button')).toBeEnabled();
    fireEvent.click(screen.getByTestId('toggle-recording-button'));
    expect(toggle).toHaveBeenCalledOnce();
    rerender(<RecordingControls {...lifecycleActions}
      snapshot={createSnapshot({ replayActive: false, continuousRecordingActive: true })}
      onSaveReplay={vi.fn()} onToggleContinuous={toggle} />);
    expect(screen.getByTestId('start-replay-button')).toBeEnabled();
    expect(screen.getByTestId('toggle-recording-button')).toBeEnabled();
  });
  it('triggers saveReplay on click when replay is active', () => {
    const onSaveReplay = vi.fn();
    const onToggleContinuous = vi.fn();
    const snapshot = createSnapshot({ replayActive: true });

    render(
      <RecordingControls {...lifecycleActions}
        snapshot={snapshot}
        onSaveReplay={onSaveReplay}
        onToggleContinuous={onToggleContinuous}
      />
    );

    const saveBtn = screen.getByTestId('save-replay-button');
    expect(saveBtn).toHaveTextContent('Сохранить последние 30 секунд');
    expect(saveBtn).not.toBeDisabled();

    fireEvent.click(saveBtn);
    expect(onSaveReplay).toHaveBeenCalledTimes(1);
  });

  it('disables saveReplay button when replay is inactive or engine is stopped', () => {
    const onSaveReplay = vi.fn();
    const snapshot = createSnapshot({ replayActive: false, lifecycle: 'stopped' });

    render(
      <RecordingControls {...lifecycleActions}
        snapshot={snapshot}
        onSaveReplay={onSaveReplay}
        onToggleContinuous={vi.fn()}
      />
    );

    const saveBtn = screen.getByTestId('save-replay-button');
    expect(saveBtn).toBeDisabled();
  });

  it('renders "Начать запись" when inactive and "Остановить запись" when active', () => {
    const onToggleContinuous = vi.fn();
    const { rerender } = render(
      <RecordingControls {...lifecycleActions}
        snapshot={createSnapshot({ continuousRecordingActive: false })}
        onSaveReplay={vi.fn()}
        onToggleContinuous={onToggleContinuous}
      />
    );

    const toggleBtn = screen.getByTestId('toggle-recording-button');
    expect(toggleBtn).toHaveTextContent('Начать запись');

    fireEvent.click(toggleBtn);
    expect(onToggleContinuous).toHaveBeenCalledTimes(1);

    rerender(
      <RecordingControls {...lifecycleActions}
        snapshot={createSnapshot({ continuousRecordingActive: true })}
        onSaveReplay={vi.fn()}
        onToggleContinuous={onToggleContinuous}
      />
    );

    expect(screen.getByTestId('toggle-recording-button')).toHaveTextContent('Остановить запись');
  });
});
