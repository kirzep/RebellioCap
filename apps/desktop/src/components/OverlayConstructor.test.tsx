import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import { OverlayConstructor } from './OverlayConstructor';
import { defaultOverlayLayout } from './overlayLayout';
afterEach(() => { cleanup(); vi.useRealTimers(); });
function setup(onCommit = vi.fn().mockResolvedValue(undefined)) {
 const onClose = vi.fn().mockResolvedValue(undefined);
 render(<OverlayConstructor layout={defaultOverlayLayout} display={{ width: 1920, height: 1080 }} onCommit={onCommit} onClose={onClose}/>);
 return { onCommit, onClose, dialog: screen.getByRole('dialog') };
}
it('has only movable elements without external buttons or selectors', () => {
 setup();
 expect(screen.getAllByRole('button')).toHaveLength(3);
 expect(screen.queryByRole('combobox')).toBeNull();
 expect(screen.getByText('Ctrl + Enter')).toBeVisible();
 expect(screen.getByText('Esc')).toBeVisible();
});
it('saves with Ctrl+Enter and shows a green frame for 500ms before closing', async () => {
 vi.useFakeTimers();
 const { onCommit, onClose, dialog } = setup();
 fireEvent.keyDown(screen.getByRole('button', { name: 'Статус записи' }), { key: 'ArrowLeft' });
 await act(async () => { fireEvent.keyDown(dialog, { key: 'Enter', ctrlKey: true }); });
 expect(onCommit.mock.calls[0][0].recording.x).toBeCloseTo(defaultOverlayLayout.recording.x - .005);
 expect(screen.getByRole('status')).toHaveTextContent('Оверлей сохранён');
 expect(dialog).toHaveAttribute('data-feedback', 'saved');
 await act(async () => { await vi.advanceTimersByTimeAsync(499); });
 expect(onClose).not.toHaveBeenCalled();
 await act(async () => { await vi.advanceTimersByTimeAsync(1); });
 expect(onClose).toHaveBeenCalledOnce();
});
it('discards with Escape and shows a cancellation frame without saving', async () => {
 vi.useFakeTimers();
 const { onCommit, onClose, dialog } = setup();
 await act(async () => { fireEvent.keyDown(dialog, { key: 'Escape' }); });
 expect(screen.getByRole('status')).toHaveTextContent('Изменения отменены');
 expect(dialog).toHaveAttribute('data-feedback', 'cancelled');
 expect(onCommit).not.toHaveBeenCalled();
 await act(async () => { await vi.advanceTimersByTimeAsync(500); });
 expect(onClose).toHaveBeenCalledOnce();
});
it('shows no success feedback if saving fails and allows retry', async () => {
 const { onClose, dialog } = setup(vi.fn().mockRejectedValue(new Error('disk full')));
 await act(async () => { fireEvent.keyDown(dialog, { key: 'Enter', ctrlKey: true }); });
 expect(screen.getByRole('alert')).toBeVisible();
 expect(dialog).not.toHaveAttribute('data-feedback');
 expect(onClose).not.toHaveBeenCalled();
 expect(screen.getByRole('button', { name: 'Сообщения' })).toBeEnabled();
});
it('does not close later if the constructor is unmounted during feedback', async () => {
 vi.useFakeTimers();
 const { dialog, onClose } = setup();
 await act(async () => { fireEvent.keyDown(dialog, { key: 'Escape' }); });
 cleanup();
 await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
 expect(onClose).not.toHaveBeenCalled();
});
function dragRecording() {
 const button = screen.getByRole('button', { name: 'Статус записи' });
 const canvas = screen.getByLabelText('Редактор оверлея');
 Object.defineProperty(canvas, 'clientWidth', { value: 1920 });
 Object.defineProperty(canvas, 'clientHeight', { value: 1080 });
 button.setPointerCapture = vi.fn(); button.releasePointerCapture = vi.fn();
 vi.spyOn(button, 'getBoundingClientRect').mockReturnValue(new DOMRect(0, 0, 30, 30));
 fireEvent(button, new MouseEvent('pointerdown', { bubbles: true, clientX: 100, clientY: 100 }));
 fireEvent(button, new MouseEvent('pointermove', { bubbles: true, clientX: 200, clientY: 100 }));
 return button;
}
it('saves the currently visible position even before releasing a drag', async () => {
 vi.useFakeTimers();
 const { dialog, onCommit } = setup();
 dragRecording();
 await act(async () => { fireEvent.keyDown(dialog, { key: 'Enter', ctrlKey: true }); });
 expect(onCommit.mock.calls[0][0].recording.x).toBeCloseTo(defaultOverlayLayout.recording.x + 100 / 1890);
});
it('reset during a drag is not undone by pointer release', async () => {
 vi.useFakeTimers();
 const { onCommit } = setup();
 const button = dragRecording();
 fireEvent.keyDown(button, { key: 'Backspace', ctrlKey: true });
 fireEvent(button, new MouseEvent('pointerup', { bubbles: true }));
 await act(async () => { fireEvent.keyDown(button, { key: 'Enter', ctrlKey: true }); });
 expect(onCommit.mock.calls[0][0]).toEqual(defaultOverlayLayout);
});
