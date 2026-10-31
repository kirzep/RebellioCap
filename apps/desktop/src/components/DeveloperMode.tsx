import { ErrorNotice } from './Primitives';
import { t, useTranslation } from '../i18n';
import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { invoke } from '@tauri-apps/api/core';
import logo from '../assets/rebelliocap-logo.svg';

let developerEnabled = false;
const subscribers = new Set<() => void>();
function publish(enabled: boolean) { developerEnabled = enabled; subscribers.forEach(callback => callback()); }
function subscribe(callback: () => void) { subscribers.add(callback); return () => { subscribers.delete(callback); }; }
export function DeveloperBrand({ className }: { className?: string }) {
  useTranslation();
  const clicks = useRef(0);
  const [notice, setNotice] = useState(false);
  const [error, setError] = useState(false);
  useEffect(() => {
    if (!notice) return;
    const timer = setTimeout(() => setNotice(false), 4000);
    return () => clearTimeout(timer);
  }, [notice]);
  async function click() {
    if (developerEnabled) return;
    try {
      const enabled = '__TAURI_INTERNALS__' in window ? await invoke<boolean>('developer_click') : ++clicks.current >= 10;
      if (enabled && !developerEnabled) { publish(true); setNotice(true); }
    } catch { setError(true); }
  }
  return <>
    <button type="button" aria-label="RebellioCap" onClick={() => void click()}
      style={{ padding: 0, border: 0, background: 'transparent', flexShrink: 0, display: 'grid', placeItems: 'center' }}>
      <img src={logo} className={className} alt="" draggable={false} />
    </button>
    {error && <ErrorNotice title={t('Не удалось включить режим разработчика.')}/>}
    {notice && !error && <div role="status" className="pointer-events-none fixed bottom-6 right-6 z-50 rounded-xl border bg-card px-5 py-3 text-sm font-semibold shadow-lg">
      {t('Режим разработчика включен')}
    </div>}
  </>;
}
export function DeveloperLogsPanel() {
  useTranslation();
  const enabled = useSyncExternalStore(subscribe, () => developerEnabled);
  const [busy, setBusy] = useState(false);
  const [savedPath, setSavedPath] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState(false);
  useEffect(() => {
    if (!('__TAURI_INTERNALS__' in window)) return;
    let disposed = false;
    void invoke<boolean>('get_developer_mode').then(value => { if (!disposed) publish(value); }).catch(() => {});
    return () => { disposed = true; };
  }, []);
  async function download() {
    setBusy(true); setMessage(null); setError(false);
    try {
      const path = await invoke<string | null>('export_application_logs');
      if (path) { setSavedPath(path); setMessage(`Логи сохранены: ${path}`); }
    } catch { setError(true); setMessage('Не удалось сохранить логи. Выберите другую папку и попробуйте ещё раз.'); }
    finally { setBusy(false); }
  }
  if (!enabled) return null;
  return <section className="space-y-4 rounded-2xl border p-5" aria-labelledby="developer-settings-title">
    <div><h3 id="developer-settings-title" className="font-semibold">{t("Для разработчика")}</h3>
      <p className="mt-1 text-sm text-muted-foreground">{t("Подробные логи приложения и движка. До 8 МБ, не старше 7 дней. В архив не входят записи и клипы.")}</p></div>
    <button type="button" className="rounded-lg border px-4 py-2 text-sm font-medium" disabled={busy || !('__TAURI_INTERNALS__' in window)} onClick={() => void download()}>
      {busy ? t('Сохраняем…') : t('Скачать логи приложения')}
    </button>
    {savedPath && <button type="button" className="rounded-lg border px-4 py-2 text-sm font-medium" onClick={() => {
      void invoke('open_saved_file_folder', { path: savedPath }).catch(() => { setError(true); setMessage('Не удалось открыть папку.'); });
    }}>{t('Открыть папку')}</button>}
    {message && (error ? <ErrorNotice title={t('Логи приложения')}>{t(message)}</ErrorNotice> : <p role="status" className="break-all text-sm">{t(message)}</p>)}
  </section>;
}
