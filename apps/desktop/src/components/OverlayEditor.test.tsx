import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, expect, it, vi } from 'vitest';
import { OverlayEditor } from './OverlayEditor';
import { defaultOverlayLayout } from './overlayLayout';
afterEach(cleanup);
it('moves only the focused element with keyboard and resets all positions', () => {
 const onSave = vi.fn();
 render(<OverlayEditor layout={defaultOverlayLayout} disabled={false} onSave={onSave}/>);
 fireEvent.keyDown(screen.getByRole('button', { name: 'Статус записи' }), { key: 'ArrowLeft' });
 expect(onSave.mock.calls[0][0].recording.x).toBeCloseTo(defaultOverlayLayout.recording.x - .005);
 expect(onSave.mock.calls[0][0].replay).toEqual(defaultOverlayLayout.replay);
 fireEvent.click(screen.getByRole('button', { name: 'Сбросить расположение' }));
 expect(onSave).toHaveBeenLastCalledWith(defaultOverlayLayout);
});
it('offers exactly the three conceptual overlay elements', () => {
 render(<OverlayEditor layout={defaultOverlayLayout} disabled={false} onSave={() => {}}/>);
 expect(screen.getAllByRole('option').map(option => option.textContent)).toEqual(['Статус записи', 'Статус повтора', 'Сообщения']);
 expect(screen.getByRole('button', { name: 'Сообщения' })).toBeVisible();
});
it('uses the real display aspect and full-size overlay elements inside one scaled stage', () => {
 render(<OverlayEditor layout={defaultOverlayLayout} disabled={false} onSave={() => {}} display={{ width: 2560, height: 1080 }} />);
 const canvas = screen.getByLabelText('Редактор оверлея');
 expect(canvas.style.aspectRatio).toBe('2560 / 1080');
 const stage = canvas.querySelector('[data-overlay-stage]') as HTMLElement;
 expect(stage.style.width).toBe('2560px');
 expect(stage.style.height).toBe('1080px');
 expect(screen.getByRole('button', { name: 'Статус записи' }).querySelector('.capture-indicator')).toBeTruthy();
 expect(screen.getByRole('button', { name: 'Сообщения' }).querySelector('.capture-notification')).toBeTruthy();
});
