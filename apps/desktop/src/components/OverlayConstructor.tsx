import { ErrorNotice } from './Primitives';
import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { t } from '../i18n';
import { OverlayEditor, type OverlayDisplay } from './OverlayEditor';
import { defaultOverlayLayout, type OverlayLayout } from './overlayLayout';
import './OverlayEditor.css';
export function OverlayConstructor({ layout, display, onCommit, onClose, feedbackAfterClose = false }: {
 layout: OverlayLayout; display: OverlayDisplay; onCommit: (layout: OverlayLayout) => Promise<void>; onClose: (status: 'saved' | 'cancelled') => Promise<void>; feedbackAfterClose?: boolean;
}) {
 const [draft, setDraft] = useState(() => structuredClone(layout));
 const [busy, setBusy] = useState(false);
 const [error, setError] = useState(false);
 const [feedback, setFeedback] = useState<'saved' | 'cancelled'>();
 const mounted = useRef(false);
 const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
 const release = useRef<(() => void) | undefined>(undefined);
 const dialog = useRef<HTMLDivElement>(null);
 const pending = useRef(false);
 useEffect(() => {
  mounted.current = true;
  const previous = document.activeElement as HTMLElement | null;
  const root = document.getElementById('root');
  const inert = root?.inert ?? false;
  if (root) root.inert = true;
  dialog.current?.focus();
  return () => { mounted.current = false; clearTimeout(timer.current); release.current?.(); if (root) root.inert = inert; previous?.focus(); };
 }, []);
 async function finish(save: boolean) {
  if (pending.current) return;
  pending.current = true; setBusy(true); setError(false);
  try {
   if (save) await onCommit(draft);
   if (!mounted.current) return;
   const status = save ? 'saved' : 'cancelled';
   if (feedbackAfterClose) { await onClose(status); return; }
   setFeedback(save ? 'saved' : 'cancelled');
   await new Promise<void>(resolve => { release.current = resolve; timer.current = setTimeout(resolve, 500); });
   release.current = undefined; timer.current = undefined;
   if (mounted.current) await onClose(status);
  } catch { if (mounted.current) { setFeedback(undefined); setError(true); } }
  finally { pending.current = false; if (mounted.current) setBusy(false); }
 }
 return createPortal(<div ref={dialog} className="overlay-constructor" data-feedback={feedback} role="dialog" aria-modal="true" aria-label={t('Конструктор оверлея')} tabIndex={-1}
  onKeyDown={event => {
   if (event.ctrlKey && event.key === 'Enter' && !event.altKey) { event.preventDefault(); event.stopPropagation(); void finish(true); }
   if (event.ctrlKey && event.key === 'Backspace') { event.preventDefault(); event.stopPropagation(); if (!pending.current) setDraft(structuredClone(defaultOverlayLayout)); }
   if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); void finish(false); }
   if (event.key === 'Tab') {
    const controls = [...(dialog.current?.querySelectorAll<HTMLElement>('button:not(:disabled), select:not(:disabled)') ?? [])];
    const index = controls.indexOf(document.activeElement as HTMLElement);
    const next = event.shiftKey ? (index <= 0 ? controls.length - 1 : index - 1) : (index + 1) % controls.length;
    if (controls[next]) { event.preventDefault(); controls[next].focus(); }
   }
  }}>
  <OverlayEditor layout={draft} display={display} disabled={busy} onSave={setDraft} fullscreen/>
  <div className="overlay-constructor__guidance">
   {error ? <ErrorNotice title={t('Конструктор оверлея')}>{t('Не удалось завершить настройку оверлея. Попробуйте ещё раз.')}</ErrorNotice> : null}
   {feedback ? <p role="status" className="overlay-constructor__result">{t(feedback === 'saved' ? 'Оверлей сохранён' : 'Изменения отменены')}</p> : <>
    <p>{t('Перетащите элементы на экране. Для точной настройки используйте стрелки на клавиатуре.')}</p>
    <p><kbd>Esc</kbd> {t('— выйти без сохранения')} <span>·</span> <kbd>Ctrl + Enter</kbd> {t('— сохранить и выйти')}</p>
    <p className="overlay-constructor__secondary"><kbd>Ctrl + Backspace</kbd> {t('— сбросить расположение')} <span>·</span> <kbd>Tab</kbd> {t('— выбрать элемент')}</p>
    <div className="overlay-shortcut-cards" aria-hidden="true">
     {[{label:'Перемещение', keys:['←','↑','↓','→']}, {label:'Сохранить', keys:['Ctrl','Enter']}, {label:'Сбросить', keys:['Ctrl','⌫']}, {label:'Выбрать', keys:['Tab']}, {label:'Выйти', keys:['Esc']}].map(item => <div className="overlay-shortcut-card" key={item.label}>
      <span>{t(item.label)}</span><div>{item.keys.map(key => <kbd key={key}>{key}</kbd>)}</div>
     </div>)}
    </div>
   </>}
  </div>
 </div>, document.body);
}
