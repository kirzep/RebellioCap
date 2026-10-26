import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import { invoke } from '@tauri-apps/api/core';
import { defaultOverlayLayout } from './overlayLayout';
import { NotificationSettingsPanel } from './NotificationSettings';
vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn() }));
vi.mock('@tauri-apps/api/webviewWindow', () => ({ getCurrentWebviewWindow: () => ({ listen: async () => () => {} }) }));
afterEach(() => { cleanup(); vi.resetAllMocks(); Reflect.deleteProperty(window, '__TAURI_INTERNALS__'); });
it('persists sound, volume and mute independently from recording settings', async () => {
  Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
  vi.mocked(invoke).mockResolvedValue({ overlayEnabled: true, soundEnabled: true, sound: 'glass', volume: 35 });
  render(<NotificationSettingsPanel />);
  const mute = screen.getByRole('checkbox', { name: 'Звук уведомлений' });
  await waitFor(() => expect(mute).toBeEnabled());
  fireEvent.click(mute);
  await waitFor(() => expect(invoke).toHaveBeenCalledWith('set_notification_settings', { settings: { overlayEnabled: true, soundEnabled: false, sound: 'glass', volume: 35, layout: defaultOverlayLayout } }));
});
it('restores the confirmed volume when saving fails', async () => {
  Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
  vi.mocked(invoke).mockImplementation(async command => {
    if (command === 'get_overlay_display') return { width: 2560, height: 1440 };
    if (command === 'get_notification_settings') return { overlayEnabled: true, soundEnabled: true, sound: 'glass', volume: 35 };
    throw new Error('disk full');
  });
  render(<NotificationSettingsPanel />);
  const slider = screen.getByRole('slider', { name: 'Громкость уведомлений' });

  await waitFor(() => expect(slider).toBeEnabled());
  fireEvent.change(slider, { target: { value: '80' } });
  fireEvent.pointerUp(slider);
  await screen.findByRole('alert');
  expect(slider).toHaveValue('35');
});
