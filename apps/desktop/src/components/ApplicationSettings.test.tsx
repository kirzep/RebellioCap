import { afterEach, describe, expect, it, vi } from 'vitest';
import { render, screen, fireEvent, waitFor, cleanup } from '@testing-library/react';
import { invoke } from '@tauri-apps/api/core';
import { ApplicationSettings } from './ApplicationSettings';

vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn() }));
afterEach(() => { cleanup(); vi.resetAllMocks(); Reflect.deleteProperty(window, '__TAURI_INTERNALS__'); });
describe('ApplicationSettings', () => {
  it('reads the Windows setting and saves a change immediately', async () => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
    vi.mocked(invoke).mockImplementation(async command => command === 'get_autostart' ? true : command === 'get_notification_settings' ? { overlayEnabled: true, soundEnabled: true, sound: 'glass', volume: 35 } : undefined);
    render(<ApplicationSettings />);
    const checkbox = screen.getByRole('checkbox', { name: 'Запускать с Windows' });
    await waitFor(() => expect(checkbox).toBeChecked());
    fireEvent.click(checkbox);
    await waitFor(() => expect(invoke).toHaveBeenCalledWith('set_autostart', { enabled: false }));
    await waitFor(() => expect(checkbox).not.toBeChecked());
  });
  it('keeps the confirmed setting if Windows rejects the change', async () => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
    vi.mocked(invoke).mockImplementation(async command => { if (command === 'set_autostart') throw new Error('denied'); return command === 'get_notification_settings' ? { overlayEnabled: true, soundEnabled: true, sound: 'glass', volume: 35 } : false; });
    render(<ApplicationSettings />);
    const checkbox = screen.getByRole('checkbox', { name: 'Запускать с Windows' });
    await waitFor(() => expect(checkbox).toBeEnabled());
    fireEvent.click(checkbox);
    await screen.findByText('Не удалось изменить автозапуск. Попробуйте ещё раз.');
    expect(checkbox).not.toBeChecked();
  });
  it('disables autostart in the browser preview', () => {
    render(<ApplicationSettings />);
    expect(screen.getByRole('checkbox', { name: 'Запускать с Windows' })).toBeDisabled();
    expect(invoke).not.toHaveBeenCalled();
  });
});
