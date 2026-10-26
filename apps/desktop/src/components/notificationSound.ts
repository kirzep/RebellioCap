import { invoke } from '@tauri-apps/api/core';
import { normalizeOverlayLayout, type OverlayLayout } from './overlayLayout';
export interface NotificationSettings {
  overlayEnabled: boolean;
  soundEnabled: boolean;
  sound: 'glass' | 'soft' | 'bell' | 'custom';
  layout?: OverlayLayout;
  customSound?: string | null;
  customSoundName?: string | null;
  volume: number;
}
export const defaultNotificationSettings: NotificationSettings = { overlayEnabled: true, soundEnabled: true, sound: 'glass', volume: 35 };
export function normalizeNotificationSettings(value: NotificationSettings): NotificationSettings {
 return { ...value, layout: normalizeOverlayLayout(value.layout) };
}
let context: AudioContext | undefined;
let cachedSound: { id: string; buffer: AudioBuffer } | undefined;
let activeSounds = 0;

/** Original short chimes: rounded attack and an exponential tail, without audio samples. */
export async function playNotificationSound(settings: NotificationSettings) {
  if (!settings.soundEnabled || settings.volume === 0) return;
  context ??= new AudioContext();
  const audioContext = context;
  activeSounds += 1;
  try { await audioContext.resume(); }
  catch (error) { activeSounds -= 1; throw error; }
  if (settings.sound === 'custom') {
    try {
      if (!settings.customSound) throw new Error('Missing sound');
      if (cachedSound?.id !== settings.customSound) {
        const encoded = await invoke<string>('read_notification_sound', { id: settings.customSound });
        const bytes = Uint8Array.from(atob(encoded), c => c.charCodeAt(0));
        const buffer = await audioContext.decodeAudioData(bytes.buffer);
        if (buffer.duration > 15) throw new Error('Sound too long');
        cachedSound = { id: settings.customSound, buffer };
      }
      const source = audioContext.createBufferSource();
      const gain = audioContext.createGain();
      source.buffer = cachedSound.buffer;
      gain.gain.value = Math.min(100, Math.max(0, settings.volume)) / 100;
      source.connect(gain); gain.connect(audioContext.destination);
      source.onended = () => { source.disconnect(); gain.disconnect(); if (--activeSounds === 0) void audioContext.suspend().catch(() => {}); };
      source.start();
    } catch (error) { if (--activeSounds === 0) void audioContext.suspend().catch(() => {}); throw error; }
    return;
  }
  const now = context.currentTime;
  const notes = settings.sound === 'soft' ? [660, 880] : settings.sound === 'bell' ? [1046.5, 1568] : [880, 1318.5];
  let remaining = notes.length;
  notes.forEach((frequency, index) => {
    const oscillator = context!.createOscillator();
    const gain = context!.createGain();
    const start = now + index * 0.055;
    oscillator.type = 'sine';
    oscillator.frequency.value = frequency;
    gain.gain.setValueAtTime(0, start);
    gain.gain.linearRampToValueAtTime(Math.min(100, Math.max(0, settings.volume)) / 100 * 0.16, start + 0.012);
    gain.gain.exponentialRampToValueAtTime(0.0001, start + 0.32);
    oscillator.connect(gain);
    gain.connect(context!.destination);
    oscillator.start(start);
    oscillator.stop(start + 0.34);
    oscillator.onended = () => {
      oscillator.disconnect(); gain.disconnect();
      if (--remaining === 0 && --activeSounds === 0) {
        void audioContext.suspend().catch(() => {});
      }
    };
  });
}
