import { it, expect } from 'vitest';
import { defaultOverlayLayout, normalizeOverlayLayout, moveOverlayItem } from './overlayLayout';
it('has only three independent overlay elements', () => {
 expect(Object.keys(defaultOverlayLayout).sort()).toEqual(['messages', 'recording', 'replay']);
 const moved = moveOverlayItem(defaultOverlayLayout, 'recording', 2, -1);
 expect(moved.recording).toEqual({ x: 1, y: 0 });
 expect(moved.replay).toEqual(defaultOverlayLayout.replay);
});
it('migrates legacy per-event positions to a single messages position', () => {
 const layout = normalizeOverlayLayout({ replay_saved: { x: .6, y: .7 }, recording: { x: .8, y: .9 } });
 expect(layout.messages).toEqual({ x: .6, y: .7 });
 expect(layout.recording).toEqual({ x: .8, y: .9 });
 expect(Object.keys(layout)).toHaveLength(3);
});
