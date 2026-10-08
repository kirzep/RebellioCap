import { useEffect, useRef, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { getCurrentWebviewWindow } from '@tauri-apps/api/webviewWindow';
import replayStartedIcon from '../assets/notifications/replay-started.svg';
import replaySavedIcon from '../assets/notifications/replay-saved.svg';
import recordingStartedIcon from '../assets/notifications/recording-started.svg';
import recordingStoppedIcon from '../assets/notifications/recording-stopped.svg';
import recordingErrorIcon from '../assets/notifications/recording-error.svg';
import replayStoppedIcon from '../assets/notifications/replay-stopped.svg';
import engineStoppedIcon from '../assets/notifications/engine-stopped.svg';
import replayErrorIcon from '../assets/notifications/replay-error.svg';
import { defaultNotificationSettings, playNotificationSound, type NotificationSettings } from './notificationSound';
import './NotificationOverlay.css';

const actions: Record<string, { text: string; icon: string; tone: string }> = {
  replay_started: { text: 'Мгновенный повтор включён', icon: replayStartedIcon, tone: 'neutral' },
  replay_saved: { text: 'Повтор сохранён', icon: replaySavedIcon, tone: 'success' },
  recording_started: { text: 'Запись началась', icon: recordingStartedIcon, tone: 'recording' },
  recording_stopped: { text: 'Запись завершена', icon: recordingStoppedIcon, tone: 'neutral' },
  recording_error: { text: 'Ошибка записи', icon: recordingErrorIcon, tone: 'error' },
  replay_stopped: { text: 'Мгновенный повтор выключен', icon: replayStoppedIcon, tone: 'neutral' },
  engine_stopped: { text: 'Движок остановлен из-за ошибки', icon: engineStoppedIcon, tone: 'error' },
  replay_error: { text: 'Не удалось сохранить повтор', icon: replayErrorIcon, tone: 'error' },
};
interface Batch { events: string[]; settings: NotificationSettings }
export function NotificationOverlay() {
  const [events, setEvents] = useState<{ kind: string; id: number }[]>([]);
  const [closing, setClosing] = useState(false);
  const [overlayEnabled, setOverlayEnabled] = useState(defaultNotificationSettings.overlayEnabled);
  const settings = useRef(defaultNotificationSettings);
  const counter = useRef(0);
  const current = events[0];
  const action = current ? actions[current.kind] : undefined;
  useEffect(() => {
    let disposed = false;
    let subscribed = false;
    let inFlight = false;
    let pending = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let subscriptionTimer: ReturnType<typeof setTimeout> | undefined;
    let unlisten: (() => void) | undefined;
    const schedule = (delay = 0) => {
      if (disposed) return;
      pending = true;
      if (!subscribed || inFlight) return;
      clearTimeout(timer);
      timer = setTimeout(() => void drain(), delay);
    };
    const drain = async () => {
      if (disposed || inFlight) return;
      inFlight = true;
      pending = false;
      let failed = false;
      try {
        const batch = await invoke<Batch>('poll_notifications');
        if (disposed) return;
        settings.current = batch.settings;
        setOverlayEnabled(batch.settings.overlayEnabled);
        if (!batch.settings.overlayEnabled && !batch.settings.soundEnabled) {
          setEvents(previous => previous.length ? [] : previous);
        } else if (batch.events.length) setEvents(previous => [...previous, ...batch.events.filter(kind => actions[kind]).map(kind => ({ kind, id: ++counter.current }))].slice(-32));
      } catch { failed = true; }
      finally { inFlight = false; }
      if (disposed) return;
      if (pending) schedule();
      else if (failed) schedule(1000);
    };
    const subscribe = async () => {
      try {
        const release = await getCurrentWebviewWindow().listen('notifications-ready', () => schedule());
        if (disposed) { release(); return; }
        unlisten = release;
        subscribed = true;
        // Subscribe before draining so an event during startup cannot be lost.
        schedule();
      } catch {
        if (!disposed) subscriptionTimer = setTimeout(() => void subscribe(), 1000);
      }
    };
    // StrictMode's discarded effect never subscribes or consumes the queue.
    subscriptionTimer = setTimeout(() => void subscribe(), 0);
    return () => { disposed = true; clearTimeout(timer); clearTimeout(subscriptionTimer); unlisten?.(); };
  }, []);
  useEffect(() => {
    if (!current) return;
    setClosing(false);
    if (settings.current.soundEnabled) void playNotificationSound(settings.current).catch(() => {});
    const duration = actions[current.kind].tone === 'error' ? 5000 : 3200;
    const closingTimer = setTimeout(() => setClosing(true), duration);
    const removeTimer = setTimeout(() => setEvents(previous => previous.slice(1)), duration + 350);
    return () => { clearTimeout(closingTimer); clearTimeout(removeTimer); };
  }, [current?.id]);
  if (!overlayEnabled || !action || !current) return null;
  return <div key={current.id} role="status" className={`capture-notification ${closing ? 'is-closing' : ''}`} data-tone={action.tone}>
    <div className="capture-notification__icon"><img src={action.icon} alt="" draggable={false} width={42} height={42} /></div>
    <strong className="capture-notification__text">{action.text}</strong>
  </div>;
}
