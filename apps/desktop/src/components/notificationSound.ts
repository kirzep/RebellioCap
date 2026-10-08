export interface NotificationSettings {
  overlayEnabled: boolean;
  soundEnabled: boolean;
  sound: 'glass' | 'soft' | 'bell';
  volume: number;
}
export const defaultNotificationSettings: NotificationSettings = { overlayEnabled: true, soundEnabled: true, sound: 'glass', volume: 35 };
let context: AudioContext | undefined;
let activeSounds = 0;

/** Original short chimes: rounded attack and an exponential tail, without audio samples. */
export async function playNotificationSound(settings: NotificationSettings) {
  if (!settings.soundEnabled || settings.volume === 0) return;
  context ??= new AudioContext();
  const audioContext = context;
  activeSounds += 1;
  try { await audioContext.resume(); }
  catch (error) { activeSounds -= 1; throw error; }
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
