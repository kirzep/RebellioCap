import { afterEach, describe, expect, it, vi } from 'vitest';
import { renderHook, waitFor, act, cleanup } from '@testing-library/react';
import { useWindowCloseGuard } from './useWindowCloseGuard';

const native = vi.hoisted(() => ({ hide: vi.fn(), onCloseRequested: vi.fn() }));
vi.mock('@tauri-apps/api/window', () => ({ getCurrentWindow: () => native }));
afterEach(() => { cleanup(); vi.resetAllMocks(); Reflect.deleteProperty(window, '__TAURI_INTERNALS__'); });
describe('tray close', () => {
  it('hides without stopping recording or prompting, even with pending edits', async () => {
    Object.defineProperty(window, '__TAURI_INTERNALS__', { value: {}, configurable: true });
    native.onCloseRequested.mockResolvedValue(vi.fn());
    native.hide.mockResolvedValue(undefined);
    const stop = vi.fn();
    const hook = renderHook(() => useWindowCloseGuard({ shouldConfirm: true, onBeforeClose: stop }));
    await waitFor(() => expect(native.onCloseRequested).toHaveBeenCalled());
    const preventDefault = vi.fn();
    await act(async () => native.onCloseRequested.mock.calls[0][0]({ preventDefault }));
    expect(preventDefault).toHaveBeenCalled();
    expect(native.hide).toHaveBeenCalledOnce();
    expect(stop).not.toHaveBeenCalled();
    expect(hook.result.current.closeRequested).toBe(false);
  });
});
