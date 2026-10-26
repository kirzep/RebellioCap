import { useEffect, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { t, useTranslation } from '../i18n';
import { OverlayConstructor } from './OverlayConstructor';
import { defaultOverlayDisplay, type OverlayDisplay } from './OverlayEditor';
import { normalizeNotificationSettings, type NotificationSettings } from './notificationSound';
import { defaultOverlayLayout, type OverlayLayout } from './overlayLayout';

export function OverlayEditorWindow() {
  useTranslation();
  const [settings, setSettings] = useState<NotificationSettings | null>(null);
  const [display, setDisplay] = useState<OverlayDisplay>(defaultOverlayDisplay);
  const [error, setError] = useState(false);

  useEffect(() => {
    if (settings) return;
    const cancel = (event: KeyboardEvent) => {
      if (event.key === 'Escape') void invoke('finish_overlay_editor', { status: 'cancelled' }).catch(() => setError(true));
    };
    window.addEventListener('keydown', cancel);
    return () => window.removeEventListener('keydown', cancel);
  }, [settings]);

  useEffect(() => {
    let disposed = false;
    void Promise.all([
      invoke<NotificationSettings>('get_notification_settings'),
      invoke<OverlayDisplay>('get_overlay_display'),
    ]).then(([value, screen]) => {
      if (disposed) return;
      setSettings(normalizeNotificationSettings(value));
      if (Number.isFinite(screen?.width) && screen.width > 0 && Number.isFinite(screen?.height) && screen.height > 0) setDisplay(screen);
    }).catch(() => { if (!disposed) setError(true); });
    return () => { disposed = true; };
  }, []);

  if (error) return <p role="alert">{t('Не удалось загрузить настройки оверлея. Нажмите Esc, чтобы выйти.')}</p>;
  if (!settings) return <p role="status">{t('Загружаем конструктор оверлея…')}</p>;

  return <OverlayConstructor
    layout={settings.layout ?? defaultOverlayLayout}
    display={display}
    feedbackAfterClose
    onCommit={async (layout: OverlayLayout) => {
      const next = { ...settings, layout };
      await invoke('set_notification_settings', { settings: next });
      setSettings(next);
    }}
    onClose={async status => { await invoke('finish_overlay_editor', { status }); }}
  />;
}
