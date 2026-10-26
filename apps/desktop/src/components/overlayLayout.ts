export interface OverlayPosition { x: number; y: number }
export const overlayLabels = { recording: 'Статус записи', replay: 'Статус повтора', messages: 'Сообщения' } as const;
export type OverlayItem = keyof typeof overlayLabels;
export type OverlayLayout = Record<OverlayItem, OverlayPosition>;
export const defaultOverlayLayout: OverlayLayout = {
 recording: { x: .93, y: .98 }, replay: { x: .99, y: .98 }, messages: { x: .02, y: .03 },
};
export function normalizeOverlayLayout(value?: Partial<Record<string, OverlayPosition>>): OverlayLayout {
 return { recording: value?.recording ?? defaultOverlayLayout.recording, replay: value?.replay ?? defaultOverlayLayout.replay,
  messages: value?.messages ?? value?.replay_saved ?? value?.recording_started ?? defaultOverlayLayout.messages };
}
export function moveOverlayItem(layout: OverlayLayout, item: OverlayItem, x: number, y: number): OverlayLayout {
 return { ...layout, [item]: { x: Math.max(0, Math.min(1, x)), y: Math.max(0, Math.min(1, y)) } };
}
export function overlayPosition(position: OverlayPosition) {
 return { left: `${position.x * 100}%`, top: `${position.y * 100}%`, transform: `translate(${-position.x * 100}%, ${-position.y * 100}%)` };
}
