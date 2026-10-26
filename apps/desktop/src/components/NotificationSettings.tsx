import { getCurrentWebviewWindow } from '@tauri-apps/api/webviewWindow';
import { t, useTranslation } from '../i18n';
import {NativeSelect,NativeSelectOption} from './ui/native-select';
import { useEffect, useRef, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { Volume2 } from 'lucide-react';
import { defaultOverlayDisplay, type OverlayDisplay } from './OverlayEditor';
import { OverlayConstructor } from './OverlayConstructor';
import { defaultOverlayLayout } from './overlayLayout';
import { normalizeNotificationSettings, defaultNotificationSettings, playNotificationSound, type NotificationSettings } from './notificationSound';

export function NotificationSettingsPanel() {
  useTranslation();
  const [editorOpen, setEditorOpen] = useState(false);
  const [display, setDisplay] = useState(defaultOverlayDisplay);
  const soundInput = useRef<HTMLInputElement>(null);
  const native = '__TAURI_INTERNALS__' in window;
  const [settings, setSettings] = useState(defaultNotificationSettings);
  const [volumeDraft, setVolumeDraft] = useState(defaultNotificationSettings.volume);
  const [busy, setBusy] = useState(native);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    if (!native) return;
    let disposed = false;
    void invoke<OverlayDisplay>('get_overlay_display').then(value => {
      if (!disposed && Number.isFinite(value?.width) && value.width > 0 && Number.isFinite(value.height) && value.height > 0) setDisplay(value);
    }).catch(() => {});
    invoke<NotificationSettings>('get_notification_settings').then(value => {
      if (!disposed) { setSettings(normalizeNotificationSettings(value)); setVolumeDraft(value.volume); }
    }).catch(() => { if (!disposed) setError('Не удалось загрузить настройки уведомлений.'); })
      .finally(() => { if (!disposed) setBusy(false); });
    return () => { disposed = true; };
  }, [native]);
  useEffect(() => {
    if (!native) return;
    let disposed = false;
    let release: (() => void) | undefined;
    void getCurrentWebviewWindow().listen('overlay-editor-closed', () => setEditorOpen(false))
      .then(unlisten => { if (disposed) unlisten(); else release = unlisten; }).catch(() => {});
    return () => { disposed = true; release?.(); };
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
  async function openEditor() {
    if (busy) return;
    setBusy(true); setError(null);
    try {
      if (native) await invoke('set_overlay_editor_fullscreen', { open: true });
      setEditorOpen(true);
    } catch { setError('Не удалось открыть конструктор оверлея.'); }
    finally { setBusy(false); }
  }
  async function closeEditor() {
    if (native) await invoke('set_overlay_editor_fullscreen', { open: false });
    setEditorOpen(false);
  }
  async function commitLayout(layout: typeof defaultOverlayLayout) {
    const next = { ...settings, layout };
    if (native) await invoke('set_notification_settings', { settings: next });
    setSettings(next);
  }
  async function importSound(file: File | undefined) {
    if (!file || busy) return;
    setBusy(true); setError(null);
    let decoder: AudioContext | undefined;
    try {
      if (file.size > 2 * 1024 * 1024 || !/\.(wav|mp3|ogg)$/i.test(file.name)) throw new Error('invalid');
      const bytes = await file.arrayBuffer();
      decoder = new AudioContext();
      const buffer = await decoder.decodeAudioData(bytes.slice(0));
      if (buffer.duration > 15 || buffer.duration <= 0) throw new Error('invalid');
      if (!native) throw new Error('native');
      const id = await invoke<string>('import_notification_sound', { bytes: Array.from(new Uint8Array(bytes)) });
      const next: NotificationSettings = { ...settings, sound: 'custom', customSound: id, customSoundName: file.name };
      await invoke('set_notification_settings', { settings: next });
      setSettings(next);
    } catch { setError('Не удалось добавить звук. Выберите WAV, MP3 или OGG до 2 МБ и 15 секунд.'); }
    finally { if (decoder) void decoder.close(); setBusy(false); if (soundInput.current) soundInput.current.value = ''; }
  }
  return <section className="space-y-5 rounded-2xl border p-5" aria-labelledby="notification-settings-title">
    <div><h3 id="notification-settings-title" className="font-semibold">{t("Уведомления")}</h3>
      <p className="mt-1 text-sm text-muted-foreground">{t("Короткий сигнал при записи и сохранении повтора. Настройки сохраняются сразу.")}</p></div>
    <label className="flex items-center gap-3">
      <input type="checkbox" aria-label={t("Оверлей уведомлений")} checked={settings.overlayEnabled} disabled={busy}
        onChange={event => void save({ ...settings, overlayEnabled: event.target.checked })} />
      <span className="font-medium">{t("Оверлей уведомлений")}</span>
    </label>
    <button type="button" className="rounded-lg border px-4 py-2 text-sm" disabled={busy} onClick={() => void openEditor()}>{t('Настроить оверлей')}</button>
    <p className="text-sm text-muted-foreground">{t('Расположите статус записи, статус повтора и сообщения в полноэкранном конструкторе.')}</p>
    {editorOpen && <OverlayConstructor display={display} layout={settings.layout ?? defaultOverlayLayout} onCommit={commitLayout} onClose={closeEditor}/>}
    <label className="flex items-center gap-3">
      <input type="checkbox" aria-label={t("Звук уведомлений")} checked={settings.soundEnabled} disabled={busy}
        onChange={event => void save({ ...settings, soundEnabled: event.target.checked })} />
      <span className="font-medium">{t("Звук уведомлений")}</span>
    </label>
    <div className="flex flex-wrap items-end gap-4">
      <label className="flex min-w-40 flex-col gap-2 text-sm">{t("Сигнал")}<NativeSelect aria-label={t("Сигнал уведомления")} className="rounded-lg border bg-background p-2" value={settings.sound} disabled={busy}
          onChange={event => void save({ ...settings, sound: event.target.value as NotificationSettings['sound'] })}>
          <NativeSelectOption value="glass">{t("Стекло")}</NativeSelectOption><NativeSelectOption value="soft">{t("Мягкий")}</NativeSelectOption><NativeSelectOption value="bell">{t("Колокольчик")}</NativeSelectOption>
          {settings.customSound && <NativeSelectOption value="custom">{settings.customSoundName ?? t("Свой звук")}</NativeSelectOption>}
        </NativeSelect>
      </label>
      <button type="button" className="flex items-center gap-2 rounded-lg border px-3 py-2 text-sm" disabled={busy}
        onClick={() => void playNotificationSound({ ...settings, soundEnabled: true }).catch(() => setError('Не удалось воспроизвести сигнал.'))}>
        <Volume2 size={16} />{t("Прослушать")}</button>
    </div>
    <div className="flex flex-wrap items-center gap-3">
      <input ref={soundInput} type="file" accept=".wav,.mp3,.ogg" hidden onChange={event => void importSound(event.target.files?.[0])} />
      <button type="button" className="rounded-lg border px-3 py-2 text-sm" disabled={busy || !native} onClick={() => soundInput.current?.click()}>{t('Добавить свой звук')}</button>
      <span className="text-xs text-muted-foreground">{t('WAV, MP3 или OGG · до 2 МБ · до 15 секунд')}</span>
    </div>
    <label className="flex flex-col gap-2 text-sm">{t("Громкость ·")}{' '}{volumeDraft}%
      <input type="range" aria-label={t("Громкость уведомлений")} min="0" max="100" value={volumeDraft} disabled={busy}
        onChange={event => setVolumeDraft(Number(event.target.value))}
        onPointerUp={() => { if (volumeDraft !== settings.volume) void save({ ...settings, volume: volumeDraft }); }}
        onKeyUp={() => { if (volumeDraft !== settings.volume) void save({ ...settings, volume: volumeDraft }); }}
        onBlur={() => { if (volumeDraft !== settings.volume) void save({ ...settings, volume: volumeDraft }); }} />
    </label>
    {!native && <p className="text-sm text-muted-foreground">{t("Предпросмотр звука. Сохранение доступно в приложении Windows.")}</p>}
    {error && <p role="alert" className="text-sm text-destructive">{t(error)}</p>}
  </section>;
}
