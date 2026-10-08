import { act, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { HostBridge } from '../bridge/contracts';
import { LiveAudioMeter } from './LiveAudioMeter';

describe('LiveAudioMeter', () => {
  afterEach(() => { vi.useRealTimers(); });
  it('updates continuously, follows device changes and stops after unmount', async () => {
    vi.useFakeTimers();
    const readAudioPeak = vi.fn().mockResolvedValueOnce(.25).mockResolvedValue(.6);
    const bridge = { readAudioPeak } as unknown as HostBridge;
    const view = render(<LiveAudioMeter bridge={bridge} endpointId="speaker" label="Компьютер" available />);
    await act(async () => {});
    expect(screen.getByRole('meter')).toHaveAttribute('aria-valuenow', '25');
    await act(async () => { await vi.advanceTimersByTimeAsync(80); });
    expect(screen.getByRole('meter')).toHaveAttribute('aria-valuenow', '60');
    view.rerender(<LiveAudioMeter bridge={bridge} endpointId="microphone" label="Микрофон" available />);
    await act(async () => {});
    expect(readAudioPeak).toHaveBeenLastCalledWith('microphone');
    view.unmount();
    const calls = readAudioPeak.mock.calls.length;
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(readAudioPeak).toHaveBeenCalledTimes(calls);
  });
  it('never overlaps requests or applies a stale result after device changes', async () => {
    vi.useFakeTimers();
    let complete!: (value: number) => void;
    const readAudioPeak = vi.fn().mockImplementationOnce(() => new Promise<number>(resolve => { complete = resolve; })).mockResolvedValue(.1);
    const bridge = { readAudioPeak } as unknown as HostBridge;
    const view = render(<LiveAudioMeter bridge={bridge} endpointId="old" label="Звук" available />);
    await act(async () => { await vi.advanceTimersByTimeAsync(500); });
    expect(readAudioPeak).toHaveBeenCalledTimes(1);
    view.rerender(<LiveAudioMeter bridge={bridge} endpointId="new" label="Звук" available />);
    await act(async () => { complete(.9); });
    expect(screen.getByRole('meter')).toHaveAttribute('aria-valuenow', '10');
    view.unmount();
  });
});
