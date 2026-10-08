import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import { invoke } from '@tauri-apps/api/core';
import { NotificationSettingsPanel } from './NotificationSettings';
vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn() }));
afterEach(() => { cleanup(); vi.resetAllMocks(); Reflect.deleteProperty(window, '__TAURI_INTERNALS__'); });
it('persists sound, volume and mute independently from recording settings', async () => {
  Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
  vi.mocked(invoke).mockResolvedValue({ overlayEnabled: true, soundEnabled: true, sound: 'glass', volume: 35 });
  render(<NotificationSettingsPanel />);
  const mute = screen.getByRole('checkbox', { name: 'Звук уведомлений' });
  await waitFor(() => expect(mute).toBeEnabled());
  fireEvent.click(mute);
  await waitFor(() => expect(invoke).toHaveBeenCalledWith('set_notification_settings', { settings: { overlayEnabled: true, soundEnabled: false, sound: 'glass', volume: 35 } }));
});
it('restores the confirmed volume when saving fails', async () => {
  Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
  vi.mocked(invoke).mockResolvedValueOnce({ overlayEnabled: true, soundEnabled: true, sound: 'glass', volume: 35 }).mockRejectedValueOnce(new Error('disk full'));
  render(<NotificationSettingsPanel />);
  const slider = screen.getByRole('slider', { name: 'Громкость уведомлений' });
  await waitFor(() => expect(slider).toBeEnabled());
  fireEvent.change(slider, { target: { value: '80' } });
  fireEvent.pointerUp(slider);
  await screen.findByRole('alert');
  expect(slider).toHaveValue('35');
});
