import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import { invoke } from '@tauri-apps/api/core';
import { OverlayEditorWindow } from './OverlayEditorWindow';

vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn() }));
afterEach(() => { cleanup(); vi.resetAllMocks(); });
it('can cancel the fullscreen window when settings fail to load', async () => {
  vi.mocked(invoke).mockRejectedValue(new Error('read failed'));
  render(<OverlayEditorWindow/>);
  expect(await screen.findByRole('alert')).toHaveTextContent('Esc');
  vi.mocked(invoke).mockResolvedValue(undefined);
  await act(async () => { fireEvent.keyDown(window, { key: 'Escape' }); });
  expect(invoke).toHaveBeenCalledWith('finish_overlay_editor', { status: 'cancelled' });
});
