import {NativeSelect,NativeSelectOption} from './ui/native-select';
import { useEffect, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { Volume2 } from 'lucide-react';
import { defaultNotificationSettings, playNotificationSound, type NotificationSettings } from './notificationSound';

export function NotificationSettingsPanel() {
  const native = '__TAURI_INTERNALS__' in window;
  const [settings, setSettings] = useState(defaultNotificationSettings);
  const [volumeDraft, setVolumeDraft] = useState(defaultNotificationSettings.volume);
  const [busy, setBusy] = useState(native);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    if (!native) return;
    let disposed = false;
    invoke<NotificationSettings>('get_notification_settings').then(value => {
      if (!disposed) { setSettings(value); setVolumeDraft(value.volume); }
    }).catch(() => { if (!disposed) setError('Не удалось загрузить настройки уведомлений.'); })
      .finally(() => { if (!disposed) setBusy(false); });
    return () => { disposed = true; };
  }, [native]);
  async function save(next: NotificationSettings) {
    if (busy) return;
    const previous = settings;
    setSettings(next);
    if (!native) return;
    setBusy(true);
    setError(null);
    try { await invoke('set_notification_settings', { settings: next }); }
    catch { setSettings(previous); setVolumeDraft(previous.volume); setError('Не удалось сохранить настройки уведомлений. Попробуйте ещё раз.'); }
    finally { setBusy(false); }
  }
  return <section className="space-y-5 rounded-2xl border p-5" aria-labelledby="notification-settings-title">
    <div><h3 id="notification-settings-title" className="font-semibold">Уведомления</h3>
      <p className="mt-1 text-sm text-muted-foreground">Короткий сигнал при записи и сохранении повтора. Настройки сохраняются сразу.</p></div>
    <label className="flex items-center gap-3">
      <input type="checkbox" aria-label="Оверлей уведомлений" checked={settings.overlayEnabled} disabled={busy}
        onChange={event => void save({ ...settings, overlayEnabled: event.target.checked })} />
      <span className="font-medium">Оверлей уведомлений</span>
    </label>
    <label className="flex items-center gap-3">
      <input type="checkbox" aria-label="Звук уведомлений" checked={settings.soundEnabled} disabled={busy}
        onChange={event => void save({ ...settings, soundEnabled: event.target.checked })} />
      <span className="font-medium">Звук уведомлений</span>
    </label>
    <div className="flex flex-wrap items-end gap-4">
      <label className="flex min-w-40 flex-col gap-2 text-sm">Сигнал
        <NativeSelect aria-label="Сигнал уведомления" className="rounded-lg border bg-background p-2" value={settings.sound} disabled={busy}
          onChange={event => void save({ ...settings, sound: event.target.value as NotificationSettings['sound'] })}>
          <NativeSelectOption value="glass">Стекло</NativeSelectOption><NativeSelectOption value="soft">Мягкий</NativeSelectOption><NativeSelectOption value="bell">Колокольчик</NativeSelectOption>
        </NativeSelect>
      </label>
      <button type="button" className="flex items-center gap-2 rounded-lg border px-3 py-2 text-sm" disabled={busy}
        onClick={() => void playNotificationSound({ ...settings, soundEnabled: true }).catch(() => setError('Не удалось воспроизвести сигнал.'))}>
        <Volume2 size={16} />Прослушать
      </button>
    </div>
    <label className="flex flex-col gap-2 text-sm">Громкость · {volumeDraft}%
      <input type="range" aria-label="Громкость уведомлений" min="0" max="100" value={volumeDraft} disabled={busy}
        onChange={event => setVolumeDraft(Number(event.target.value))}
        onPointerUp={() => { if (volumeDraft !== settings.volume) void save({ ...settings, volume: volumeDraft }); }}
        onKeyUp={() => { if (volumeDraft !== settings.volume) void save({ ...settings, volume: volumeDraft }); }}
        onBlur={() => { if (volumeDraft !== settings.volume) void save({ ...settings, volume: volumeDraft }); }} />
    </label>
    {!native && <p className="text-sm text-muted-foreground">Предпросмотр звука. Сохранение доступно в приложении Windows.</p>}
    {error && <p role="alert" className="text-sm text-destructive">{error}</p>}
  </section>;
}
