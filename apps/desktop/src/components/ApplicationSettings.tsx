import { useEffect, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { DeveloperLogsPanel } from './DeveloperMode';
import { NotificationSettingsPanel } from './NotificationSettings';
import { RecordingNameSettings } from '../naming/NamingSettings';
import { ErrorNotice } from './Primitives';

export function ApplicationSettings() {
  const [enabled, setEnabled] = useState(false);
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [technicalError, setTechnicalError] = useState<string | null>(null);
  const native = '__TAURI_INTERNALS__' in window;
  useEffect(() => {
    let disposed = false;
    if (!native) { setBusy(false); return; }
    invoke<boolean>('get_autostart').then(value => {
      if (!disposed) setEnabled(value);
    }).catch(() => {
      if (!disposed) setError('Не удалось прочитать настройку автозапуска. Откройте этот раздел ещё раз.');
    }).finally(() => { if (!disposed) setBusy(false); });
    return () => { disposed = true; };
  }, [native]);
  async function change(next: boolean) {
    setBusy(true);
    setError(null);
    try {
      await invoke('set_autostart', { enabled: next });
      setEnabled(next);
    } catch (cause) {
      setError('Не удалось изменить автозапуск. Попробуйте ещё раз.');
      setTechnicalError(String(cause));
    } finally { setBusy(false); }
  }
  return <div className="space-y-6">
    <div><h2 className="text-xl font-semibold">Приложение</h2>
      <p className="mt-2 text-muted-foreground">Крестик сворачивает RebellioCap в трей. Replay, запись и горячие клавиши продолжают работать.</p></div>
    <label className="flex items-center gap-3 rounded-2xl border p-5">
      <input type="checkbox" checked={enabled} disabled={busy || !native}
        onChange={event => void change(event.target.checked)} aria-label="Запускать с Windows" />
      <span><span className="block font-medium">Запускать с Windows</span>
        <span className="block text-sm text-muted-foreground">При входе в Windows приложение запускается в трее. Изменение сохраняется сразу.</span></span>
    </label>
    <p className="text-sm text-muted-foreground">Нажмите на значок в трее, чтобы вернуть окно. В меню по правой кнопке мыши доступны быстрые действия и «Выйти».</p>
    {!native && <p role="status">Автозапуск доступен в установленном приложении Windows.</p>}
    <RecordingNameSettings />
    <NotificationSettingsPanel />
    <DeveloperLogsPanel />
    {error && <ErrorNotice title="Автозапуск" technicalDetails={technicalError ?? undefined}>{error}</ErrorNotice>}
  </div>;
}
