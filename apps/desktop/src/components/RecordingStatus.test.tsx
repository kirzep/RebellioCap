import React from 'react';
import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import type { EngineSnapshot } from '../config/model';
import { RecordingStatus } from './RecordingStatus';

function snapshot(): EngineSnapshot {
  return {
    revision: 1, lifecycle: 'ready', replayActive: true,
    continuousRecordingActive: false, replaySeconds: 120, lastError: null,
    metrics: {
      videoTicks: 10, missedVideoDeadlines: 0, videoPackets: 10, audioPackets: 0,
      saveRequests: 0, completedSaves: 0, failedSaves: 0, rejectedSaves: 0,
      pipelineErrors: 0, continuousPackets: 0, continuousFailures: 0,
      continuousRecordingActive: false, lastHotkeySaveLatencyTicks: 0, replayBytes: 1000,
      budgetBytes: 8192, bufferedBytes: 5000, replayRetainedSeconds: 12.9,
      replayMemoryLimited: true, replayMemoryDrops: 8,
    },
  };
}

describe('Replay memory pressure', () => {
  it('names the unavailable endpoint while recording continues and clears after recovery', () => {
    const state = snapshot();
    Object.assign(state.metrics, { audioUnavailableSources: 2 });
    const { rerender } = render(<RecordingStatus snapshot={state} />);
    expect(screen.getByText(/микрофон недоступен/i)).toBeInTheDocument();
    expect(screen.getByText(/запись продолжается/i)).toBeInTheDocument();
    Object.assign(state.metrics, { audioUnavailableSources: 0 });
    rerender(<RecordingStatus snapshot={{ ...state, revision: 2 }} />);
    expect(screen.queryByText(/микрофон недоступен/i)).not.toBeInTheDocument();
  });
  it('shows available footage when Replay is shorter because of memory pressure', () => {
    render(<RecordingStatus snapshot={snapshot()} />);
    expect(screen.getByText(/доступно 12 с/i)).toBeInTheDocument();
    expect(screen.getByText(/лимит памяти/i)).toBeInTheDocument();
  });
  it('does not present an unavailable snapshot as current memory pressure', () => {
    render(<RecordingStatus snapshot={snapshot()} unavailable />);
    expect(screen.queryByText(/лимит памяти/i)).not.toBeInTheDocument();
  });
});
