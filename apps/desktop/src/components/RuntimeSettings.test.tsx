import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import React from 'react';
import { describe, expect, it, vi } from 'vitest';
import type { ActiveConfig, EngineSnapshot } from '../config/model';
import { RuntimeSettings } from './RuntimeSettings';
import type { HostBridge } from '../bridge/contracts';

const hostBridge = {
  listMonitors: vi.fn().mockResolvedValue([{ id:'mon-0', name:'Экран', width:1920, height:1080, primary:true }]),
  listAudioEndpoints: vi.fn().mockResolvedValue({ system_audio:[{id:'sys-0',name:'Система',available:true}], microphones:[{id:'mic-0',name:'Микрофон',available:true}] }),
  measureAudioLevel: vi.fn().mockResolvedValue({ peak:0 }),
} as unknown as HostBridge;

function createActiveConfig(): ActiveConfig {
  return {
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
    save_replay_hotkey: { key: 0x77, ctrl: false, alt: false, shift: false, win: false }, // F8
    toggle_recording_hotkey: { key: 0x52, ctrl: true, alt: false, shift: true, win: false }, // Ctrl+Shift+R
    preferences: { overlay_enabled: true, start_with_windows: false },
    continuous_recording_enabled: false,
  };
}

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

describe('RuntimeSettings', () => {
  it('applies direct output and restores the saved checkbox state', async () => {
    const onApply = vi.fn().mockResolvedValue(undefined);
    const props = { hostBridge, activeConfig: createActiveConfig(), snapshot: createSnapshot({ lifecycle: 'stopped', replayActive: false }), onApply, initialSection: 'replay' as const };
    const { unmount } = render(<RuntimeSettings {...props} />);
    const checkbox = screen.getByRole('checkbox', { name: /Сохранять записи без подпапок для игр/ });
    expect(checkbox).not.toBeChecked();
    fireEvent.click(checkbox);
    fireEvent.click(screen.getByRole('button', { name: 'Применить' }));
    await waitFor(() => expect(onApply).toHaveBeenCalledWith(expect.objectContaining({ save_without_game_folders: true })));
    const saved = onApply.mock.calls[0][0];
    unmount();
    render(<RuntimeSettings {...props} activeConfig={saved} />);
    expect(screen.getByRole('checkbox', { name: /Сохранять записи без подпапок для игр/ })).toBeChecked();
  });
  it('cancels hotkey preparation without showing a failure', async () => {
    render(<RuntimeSettings hostBridge={hostBridge} activeConfig={createActiveConfig()} snapshot={createSnapshot()} onApply={vi.fn()} initialSection="hotkeys" sidebarNavigation />);
    fireEvent.click(screen.getByRole('button', { name:'Изменить сочетание для сохранения Replay' }));
    expect(await screen.findByRole('dialog')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name:'Отмена' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(screen.queryByText(/Перехват не начат/)).not.toBeInTheDocument();
  });
  it('keeps unsaved edits when the sidebar changes the settings section', async () => {
    const props = { hostBridge, activeConfig:createActiveConfig(), snapshot:createSnapshot(), onApply:vi.fn(), sidebarNavigation:true };
    const { rerender } = render(<RuntimeSettings {...props} initialSection="video" />);
    const width = await screen.findByTestId('video-width-input');
    fireEvent.change(width, {target:{value:'1600'}});
    rerender(<RuntimeSettings {...props} initialSection="audio" />);
    expect(screen.getByTestId('audio-step')).toBeInTheDocument();
    rerender(<RuntimeSettings {...props} initialSection="video" />);
    expect(screen.getByTestId('video-width-input')).toHaveValue('1600');
    expect(screen.queryByRole('navigation', {name:'Разделы настроек'})).not.toBeInTheDocument();
  });
  it('displays "Применить и перезапустить запись" when engine is active', () => {
    const activeConfig = createActiveConfig();
    const snapshot = createSnapshot({ lifecycle: 'ready', replayActive: true });

    render(
      <RuntimeSettings hostBridge={hostBridge}
        activeConfig={activeConfig}
        snapshot={snapshot}
        onApply={vi.fn()}
      />
    );

    expect(screen.getByTestId('apply-settings-button')).toHaveTextContent('Применить');
    expect(screen.getByTestId('apply-settings-button')).toBeDisabled();
  });

  it('displays "Применить" when engine is stopped', () => {
    const activeConfig = createActiveConfig();
    const snapshot = createSnapshot({ lifecycle: 'stopped', replayActive: false });

    render(
      <RuntimeSettings hostBridge={hostBridge}
        activeConfig={activeConfig}
        snapshot={snapshot}
        onApply={vi.fn()}
      />
    );

    expect(screen.getByTestId('apply-settings-button')).toHaveTextContent('Применить');
  });

  it('calls onApply with updated candidate when valid', async () => {
    const activeConfig = createActiveConfig();
    const snapshot = createSnapshot();
    const onApply = vi.fn().mockResolvedValue(undefined);

    render(
      <RuntimeSettings hostBridge={hostBridge} initialSection="replay"
        activeConfig={activeConfig}
        snapshot={snapshot}
        onApply={onApply}
      />
    );

    const input = screen.getByLabelText('Длительность Replay');
    fireEvent.keyDown(input, { key:'Enter' });
    fireEvent.click(await screen.findByRole('option', { name:'60 секунд' }));

    fireEvent.click(screen.getByTestId('apply-settings-button'));

    await waitFor(() => {
      expect(onApply).toHaveBeenCalledWith(
        expect.objectContaining({ replay_seconds: 60 })
      );
    });
  });

  it('preserves the edited draft and displays error when apply fails', async () => {
    const activeConfig = createActiveConfig();
    const snapshot = createSnapshot();
    const onApply = vi.fn().mockRejectedValue(new Error('Каталог недоступен'));

    render(
      <RuntimeSettings hostBridge={hostBridge} initialSection="replay"
        activeConfig={activeConfig}
        snapshot={snapshot}
        onApply={onApply}
      />
    );

    const input = screen.getByLabelText('Длительность Replay');
    fireEvent.keyDown(input, { key:'Enter' });
    fireEvent.click(await screen.findByRole('option', { name:'60 секунд' }));
    fireEvent.click(screen.getByTestId('apply-settings-button'));

    await waitFor(() => {
      expect(screen.getByText(/Каталог недоступен.*Введённые значения/)).toBeInTheDocument();
      expect(screen.getByLabelText('Длительность Replay')).toHaveTextContent('60 секунд');
    });
  });
});


it('applies a manual Replay budget and preserves it on unrelated edits', async () => {
  const onApply = vi.fn().mockResolvedValue(undefined);
  render(<RuntimeSettings hostBridge={hostBridge} activeConfig={{...createActiveConfig(), replay_memory_limit_mb: 2048}} snapshot={createSnapshot()} onApply={onApply} initialSection="replay" />);
  const input = screen.getByLabelText('Лимит, МиБ');
  expect(input).toHaveValue('2048');
  fireEvent.change(input, {target: {value: '4096'}});
  fireEvent.click(screen.getByTestId('apply-settings-button'));
  await waitFor(() => expect(onApply).toHaveBeenCalledWith(expect.objectContaining({replay_memory_limit_mb: 4096})));
});

it('rejects zero typed in manual mode and allows choosing Auto explicitly', async () => {
  const onApply = vi.fn().mockResolvedValue(undefined);
  render(<RuntimeSettings hostBridge={hostBridge} activeConfig={{...createActiveConfig(), replay_memory_limit_mb: 2048}} snapshot={createSnapshot()} onApply={onApply} initialSection="replay" />);
  const input = screen.getByLabelText('Лимит, МиБ');
  fireEvent.focus(input);
  fireEvent.change(input, {target: {value: '0'}});
  expect(input).toHaveValue('0');
  fireEvent.click(screen.getByTestId('apply-settings-button'));
  expect(onApply).not.toHaveBeenCalled();
  expect(await screen.findAllByText(/Лимит памяти должен/)).not.toHaveLength(0);
  fireEvent.keyDown(screen.getByLabelText('Лимит памяти Replay'), {key:'Enter'});
  fireEvent.click(await screen.findByRole('option', {name:'Авто'}));
  fireEvent.click(screen.getByTestId('apply-settings-button'));
  await waitFor(() => expect(onApply).toHaveBeenCalledWith(expect.objectContaining({replay_memory_limit_mb: 0})));
});

it('retains the manual budget and RAM guidance after admission fails', async () => {
  const onApply = vi.fn().mockRejectedValue({code:'config.replay_memory_unavailable', message:'Доступно 2048 МиБ. Уменьшите лимит или выберите «Авто».'});
  render(<RuntimeSettings hostBridge={hostBridge} activeConfig={createActiveConfig()} snapshot={createSnapshot()} onApply={onApply} initialSection="replay" />);
  fireEvent.keyDown(screen.getByLabelText('Лимит памяти Replay'), {key:'Enter'});
  fireEvent.click(await screen.findByRole('option', {name:'Задать вручную'}));
  fireEvent.change(screen.getByLabelText('Лимит, МиБ'), {target: {value:'8192'}});
  fireEvent.click(screen.getByTestId('apply-settings-button'));
  await screen.findAllByText(/Доступно 2048 МиБ/);
  expect(screen.getByLabelText('Лимит, МиБ')).toHaveValue('8192');
});
