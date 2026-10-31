import { t, useTranslation } from '../i18n';
import { useEffect, useRef, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { getCurrentWebviewWindow } from '@tauri-apps/api/webviewWindow';
import { actions, OverlayIndicator, OverlayNotification } from './OverlayVisuals';
import { defaultOverlayLayout, overlayPosition } from './overlayLayout';
import { normalizeNotificationSettings, defaultNotificationSettings, playNotificationSound, type NotificationSettings } from './notificationSound';
import './NotificationOverlay.css';

interface Batch { replaySaving?: boolean; events: string[]; settings: NotificationSettings; replayActive?: boolean; recordingActive?: boolean; editorActive?: boolean; editorFeedback?: 'saved' | 'cancelled' | null }
export function NotificationOverlay() {
  useTranslation();
  const [events, setEvents] = useState<{ kind: string; id: number }[]>([]);
  const [replaySaving, setReplaySaving] = useState(false);
  const [closing, setClosing] = useState(false);
  const [overlayEnabled, setOverlayEnabled] = useState(defaultNotificationSettings.overlayEnabled);
  const [editorActive, setEditorActive] = useState(false);
  const [editorFeedback, setEditorFeedback] = useState<'saved' | 'cancelled'>();
  const [capture, setCapture] = useState({ replay: false, recording: false });
  const [layout, setLayout] = useState(defaultOverlayLayout);
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
        setReplaySaving(batch.replaySaving ?? false);
        setEditorActive(batch.editorActive ?? false);
        setEditorFeedback(batch.editorFeedback ?? undefined);
        setLayout(normalizeNotificationSettings(batch.settings).layout!);
        setCapture({ replay: batch.replayActive ?? false, recording: batch.recordingActive ?? false });
        setOverlayEnabled(batch.settings.overlayEnabled);
        if (!batch.settings.overlayEnabled && !batch.settings.soundEnabled) {
          setEvents(previous => previous.length ? [] : previous);
        } else if (batch.events.length) setEvents(previous => [...previous, ...batch.events.filter(kind => kind !== 'replay_saving' && actions[kind]).map(kind => ({ kind, id: ++counter.current }))].slice(-32));
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
    if (!editorFeedback) return;
    const timer = setTimeout(() => {
      setEditorFeedback(undefined);
      if ('__TAURI_INTERNALS__' in window) void invoke('restore_overlay_editor_host').catch(() => {});
    }, 500);
    return () => clearTimeout(timer);
  }, [editorFeedback]);
  useEffect(() => {
    if (!current || replaySaving) return;
    setClosing(false);
    if (settings.current.soundEnabled) void playNotificationSound(settings.current).catch(() => {});
    const duration = actions[current.kind].tone === 'error' ? 5000 : 3200;
    const closingTimer = setTimeout(() => setClosing(true), duration);
    const removeTimer = setTimeout(() => setEvents(previous => previous.slice(1)), duration + 350);
    return () => { clearTimeout(closingTimer); clearTimeout(removeTimer); };
  }, [current?.id, replaySaving]);
  const visible = !editorActive && (Boolean(editorFeedback) || (overlayEnabled && (replaySaving || capture.replay || capture.recording || !!(action && current))));
  useEffect(() => {
    if ('__TAURI_INTERNALS__' in window) void invoke('set_overlay_visible', { visible }).catch(() => {});
  }, [visible]);
  if (!overlayEnabled && !editorFeedback) return null;
  return <>
    {overlayEnabled && capture.recording && <div className="capture-indicator-position" style={overlayPosition(layout.recording)} aria-label={t('Идёт запись')}><OverlayIndicator recording/></div>}
    {overlayEnabled && capture.replay && <div className="capture-indicator-position" style={overlayPosition(layout.replay)} aria-label={t('Мгновенный повтор включён')}><OverlayIndicator/></div>}
    {overlayEnabled && (replaySaving || (action && current)) && <div className="capture-notification-position" style={overlayPosition(layout.messages)}>
      <OverlayNotification key={replaySaving ? "saving" : current?.id} kind={replaySaving ? "replay_saving" : current!.kind} closing={!replaySaving && closing}/>
    </div>}
    {editorFeedback && <div className="overlay-editor-feedback" data-status={editorFeedback} role="status">
      {t(editorFeedback === 'saved' ? 'Оверлей сохранён' : 'Изменения отменены')}
    </div>}
  </>;
}
