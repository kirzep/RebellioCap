import React, {
  createContext, useContext, useEffect, useId, useLayoutEffect, useMemo, useRef, useState,
  type ReactNode,
} from 'react';
import { createPortal } from 'react-dom';
import { ChevronLeft, ChevronRight, X } from 'lucide-react';
import { t, useTranslation } from '../i18n';
import styles from './ErrorCenter.module.css';

type Entry = { id: string; fingerprint: string; content: ReactNode };
type Registry = { put: (entry: Entry) => void; remove: (id: string) => void };
const Context = createContext<Registry | null>(null);

/** Text identity groups simultaneous reports without comparing React elements or callbacks. */
export function errorText(node: ReactNode): string {
  if (typeof node === 'string' || typeof node === 'number') return String(node);
  if (Array.isArray(node)) return node.map(errorText).join(' ');
  if (React.isValidElement<{ children?: ReactNode }>(node)) return errorText(node.props.children);
  return '';
}

export function ErrorCenterProvider({ children }: { children: ReactNode }) {
  const [entries, setEntries] = useState<Entry[]>([]);
  const [dismissed, setDismissed] = useState<Record<string, string>>({});
  const [selected, setSelected] = useState<string | null>(null);
  // Stable context prevents a registration update from rerendering its sources.
  const registry = useMemo<Registry>(() => ({
    put(entry) {
      setEntries(previous => {
        const index = previous.findIndex(item => item.id === entry.id);
        if (index < 0) return [...previous, entry];
        const next = [...previous];
        next[index] = entry;
        return next;
      });
    },
    remove(id) {
      setEntries(previous => previous.filter(item => item.id !== id));
      setDismissed(previous => {
        if (!(id in previous)) return previous;
        const next = { ...previous };
        delete next[id];
        return next;
      });
    },
  }), []);

  const groups = new Map<string, Entry[]>();
  for (const entry of entries) {
    if (dismissed[entry.id] === entry.fingerprint) continue;
    const group = groups.get(entry.fingerprint) ?? [];
    group.push(entry);
    groups.set(entry.fingerprint, group);
  }
  const errors = [...groups.values()];
  const selectedIndex = errors.findIndex(group => group[0].fingerprint === selected);
  const index = selectedIndex < 0 ? errors.length - 1 : selectedIndex;
  const current = errors[index];

  function dismissCurrent() {
    setDismissed(previous => ({
      ...previous,
      ...Object.fromEntries(current.map(entry => [entry.id, entry.fingerprint])),
    }));
    setSelected(null);
  }

  return <Context.Provider value={registry}>
    {children}
    {current && <ErrorCenterCard count={errors.length} index={index}
      onSelect={next => setSelected(errors[next][0].fingerprint)} onDismiss={dismissCurrent}>
      {current[0].content}
    </ErrorCenterCard>}
  </Context.Provider>;
}

interface CardProps {
  children: ReactNode;
  count: number;
  index: number;
  onSelect: (index: number) => void;
  onDismiss: () => void;
}

function ErrorCenterCard({ children, count, index, onSelect, onDismiss }: CardProps) {
  useTranslation();
  const [target, setTarget] = useState<HTMLElement>(document.body);
  const ref = useRef<HTMLDivElement>(null);

  // Keep the card inside a modal's focus scope. A manual popover places it in the
  // top layer, so dialog transforms and clipping cannot move it from the corner.
  useLayoutEffect(() => {
    function updateTarget() {
      const modals = [...document.querySelectorAll<HTMLElement>(
        '[role="dialog"][aria-modal="true"], [data-slot="dialog-content"]',
      )].filter(node => !node.closest('[hidden]') && node.dataset.state !== 'closed' &&
        getComputedStyle(node).display !== 'none');
      setTarget(modals.at(-1) ?? document.body);
    }
    updateTarget();
    const observer = new MutationObserver(updateTarget);
    observer.observe(document.body, {
      childList: true, subtree: true, attributes: true,
      attributeFilter: ['hidden', 'aria-modal', 'data-state'],
    });
    return () => observer.disconnect();
  }, []);

  useLayoutEffect(() => {
    const node = ref.current;
    if (node?.showPopover) {
      try { node.showPopover(); }
      catch { /* Detached during a modal transition; the next target opens it. */ }
    }
  }, [target]);

  return createPortal(
    <div ref={ref} popover={typeof HTMLElement.prototype.showPopover === 'function' ? 'manual' : undefined}
      className={styles.center} role="region" aria-label={t('Ошибки приложения')} data-error-center="">
      <div className={styles.card}>
        <button type="button" className={styles.close} aria-label={t('Закрыть ошибку')} onClick={onDismiss}>
          <X size={16} aria-hidden="true"/>
        </button>
        <div className={styles.content}>{children}</div>
        {count > 1 && <nav className={styles.queue} aria-label={t('Список ошибок')}>
          <span>{t('Ошибки: {0}', count)}</span>
          <span className={styles.position}>{index + 1} / {count}</span>
          <button type="button" aria-label={t('Предыдущая ошибка')} onClick={() => onSelect((index + count - 1) % count)}>
            <ChevronLeft size={16} aria-hidden="true"/>
          </button>
          <button type="button" aria-label={t('Следующая ошибка')} onClick={() => onSelect((index + 1) % count)}>
            <ChevronRight size={16} aria-hidden="true"/>
          </button>
        </nav>}
      </div>
    </div>, target,
  );
}

/** Registers while the source exists; resolving or unmounting removes its card. */
export function ErrorNotification({ children, fingerprint }: { children: ReactNode; fingerprint: string }) {
  const registry = useContext(Context);
  const id = useId();
  useEffect(() => { registry?.put({ id, fingerprint, content: children }); }, [registry, id, fingerprint, children]);
  useEffect(() => () => registry?.remove(id), [registry, id]);
  return registry ? null : children;
}
