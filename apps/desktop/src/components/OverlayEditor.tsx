import { useLayoutEffect, useRef, useState } from 'react';
import { RotateCcw } from 'lucide-react';
import { t } from '../i18n';
import { defaultOverlayLayout, moveOverlayItem, overlayLabels, overlayPosition, type OverlayItem, type OverlayLayout } from './overlayLayout';
import { OverlayIndicator, OverlayNotification } from './OverlayVisuals';
import './OverlayEditor.css';
export interface OverlayDisplay { width: number; height: number }
export const defaultOverlayDisplay = { width: 1920, height: 1080 };
export function OverlayEditor({ layout, disabled, onSave, display = defaultOverlayDisplay, fullscreen = false }: { layout: OverlayLayout; disabled: boolean; onSave: (layout: OverlayLayout) => void; display?: OverlayDisplay; fullscreen?: boolean }) {
 const [selected, setSelected] = useState<OverlayItem>('messages');
 const [draft, setDraft] = useState<OverlayLayout | null>(null);
 const dragging = useRef<{ id: OverlayItem; pointer: number; x: number; y: number; initial: OverlayLayout } | null>(null);
 const liveDraft = useRef<OverlayLayout | null>(null);
 useLayoutEffect(() => {
  if (fullscreen && layout !== liveDraft.current) dragging.current = null;
 }, [layout, fullscreen]);
 const canvas = useRef<HTMLDivElement>(null);
 const [scale, setScale] = useState(1);
 useLayoutEffect(() => {
  const element = canvas.current;
  if (!element || fullscreen) return;
  const resize = () => { if (element.clientWidth > 0) setScale(element.clientWidth / display.width); };
  resize();
  if (typeof ResizeObserver === 'undefined') return;
  const observer = new ResizeObserver(resize);
  observer.observe(element);
  return () => observer.disconnect();
 }, [display.width, fullscreen]);
 const current = fullscreen ? layout : draft ?? layout;
 const items: OverlayItem[] = ['messages', 'recording', 'replay'];
 return <div className={`overlay-editor ${fullscreen ? 'overlay-editor--fullscreen' : ''}`}>
  {!fullscreen && <div className="overlay-editor__toolbar"><label>{t('Элемент оверлея')}<select value={selected} disabled={disabled} onChange={e => setSelected(e.target.value as OverlayItem)}>
   {Object.entries(overlayLabels).map(([key, label]) => <option key={key} value={key}>{t(label)}</option>)}
  </select></label><button type="button" disabled={disabled} onClick={() => { setDraft(null); onSave(structuredClone(defaultOverlayLayout)); }}><RotateCcw size={14}/>{t('Сбросить расположение')}</button></div>}
  <p>{t('Перетащите элементы на экране. Для точной настройки используйте стрелки на клавиатуре.')}</p>
  <div ref={canvas} className="overlay-editor__canvas" style={fullscreen ? undefined : { aspectRatio: `${display.width} / ${display.height}` }} aria-label={t('Редактор оверлея')}>
   {!fullscreen && <span className="overlay-editor__hint">{t(fullscreen ? 'Перетащите элементы на экране. Для точной настройки используйте стрелки на клавиатуре.' : 'Предпросмотр экрана')}</span>}
   <div data-overlay-stage className="overlay-editor__stage" style={fullscreen ? { width: '100%', height: '100%' } : { width: display.width, height: display.height, transform: `scale(${scale})` }}>
   {items.map(id => <button type="button" key={id} disabled={disabled} className="overlay-editor__item" style={overlayPosition(current[id])}
    aria-label={t(overlayLabels[id])} title={t(overlayLabels[id])} data-selected={selected === id}
    onPointerDown={e => { if (disabled) return; e.preventDefault(); e.currentTarget.focus(); setSelected(id); e.currentTarget.setPointerCapture(e.pointerId); dragging.current = { id, pointer: e.pointerId, x: e.clientX, y: e.clientY, initial: layout }; }}
    onPointerMove={e => { const drag = dragging.current; const element = canvas.current; if (disabled || !drag || drag.id !== id || !element) return;
     const itemRect = e.currentTarget.getBoundingClientRect();
     const width = Math.max(1, element.clientWidth - itemRect.width); const height = Math.max(1, element.clientHeight - itemRect.height);
     const next = moveOverlayItem(drag.initial, id, drag.initial[id].x + (e.clientX - drag.x) / width, drag.initial[id].y + (e.clientY - drag.y) / height);
     if (fullscreen) { liveDraft.current = next; onSave(next); } else setDraft(next); }}
    onPointerUp={e => { if (!dragging.current) return; dragging.current = null; e.currentTarget.releasePointerCapture(e.pointerId); if (!fullscreen && draft) onSave(draft); setDraft(null); }}
    onPointerCancel={() => { dragging.current = null; setDraft(null); }}
    onKeyDown={e => { const delta = e.shiftKey ? .05 : .005; const offsets: Record<string, [number, number]> = { ArrowLeft: [-delta, 0], ArrowRight: [delta, 0], ArrowUp: [0, -delta], ArrowDown: [0, delta] }; const offset = offsets[e.key]; if (!offset) return; e.preventDefault(); onSave(moveOverlayItem(layout, id, layout[id].x + offset[0], layout[id].y + offset[1])); }}>
    {id === 'recording' || id === 'replay' ? <OverlayIndicator recording={id === 'recording'}/> : <OverlayNotification kind="replay_saved" preview/>}
   </button>)}
   </div>
  </div>
  <p>{t('Значки видны только во время записи или работы повтора. Здесь показаны образцы.')}</p>
 </div>;
}
