import { t, useTranslation } from '../i18n';
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { getCurrentWindow } from '@tauri-apps/api/window';
import { Copy, Minus, Square, X } from 'lucide-react';
import { Button } from './ui/button';
import styles from './WindowTitlebar.module.css';

/** The native close handler hides the window while recording continues. */
export function WindowTitlebar({ children }: { children?: ReactNode }) {
  useTranslation();
  const native = typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window;
  const [maximized, setMaximized] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(false);
  const pending = useRef(false);

  useEffect(() => {
    mounted.current = true;
    if (!native) return () => { mounted.current = false; };
    let disposed = false;
    let unlisten: (() => void) | undefined;
    let revision = 0;
    const appWindow = getCurrentWindow();
    async function refresh() {
      const request = ++revision;
      try {
        const next = await appWindow.isMaximized();
        if (!disposed && request === revision) setMaximized(next);
      } catch {
        if (!disposed) setError('Не удалось получить состояние окна.');
      }
    }
    void refresh();
    void appWindow.onResized(() => { void refresh(); }).then((remove) => {
      if (disposed) remove();
      else unlisten = remove;
    }).catch(() => {
      if (!disposed) setError('Не удалось подключить события окна.');
    });
    return () => { disposed = true; mounted.current = false; unlisten?.(); };
  }, [native]);

  async function run(action: 'minimize' | 'toggleMaximize' | 'close' | 'startDragging') {
    if (!native || pending.current) return;
    pending.current = true;
    setError(null);
    try {
      const appWindow = getCurrentWindow();
      await appWindow[action]();
      if (action === 'toggleMaximize') {
        const next = await appWindow.isMaximized();
        if (mounted.current) setMaximized(next);
      }
    } catch {
      if (mounted.current) setError('Действие с окном не выполнено. Повторите попытку.');
    } finally {
      pending.current = false;
    }
  }

  if (!native && !children) return null;
  return (
    <header className={styles.titlebar} aria-label={t("Панель окна")}>
      {children && <div className={styles.leading}>{children}</div>}
      <div className={styles.dragRegion} aria-hidden="true" onMouseDown={(event) => {
        if (event.button === 0) void run(event.detail === 2 ? 'toggleMaximize' : 'startDragging');
      }} />
      {error && <span className={styles.error} role="alert">{t(error)}</span>}
      {native && <div className={styles.controls}>
        <Button variant="ghost" size="icon" className={styles.control} aria-label={t("Свернуть")} title={t("Свернуть")}
          onClick={() => void run('minimize')}><Minus aria-hidden="true" /></Button>
        <Button variant="ghost" size="icon" className={styles.control}
          aria-label={maximized ? t('Восстановить размер') : t('Развернуть')} title={maximized ? t('Восстановить размер') : t('Развернуть')}
          onClick={() => void run('toggleMaximize')}>
          {maximized ? <Copy aria-hidden="true" /> : <Square aria-hidden="true" />}
        </Button>
        <Button variant="ghost" size="icon" className={styles.close} aria-label={t("Свернуть в трей")} title={t("Свернуть в трей")}
          onClick={() => void run('close')}><X aria-hidden="true" /></Button>
      </div>}
    </header>
  );
}
