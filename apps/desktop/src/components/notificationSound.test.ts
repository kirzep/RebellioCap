import { afterEach, expect, it, vi } from 'vitest';
import { invoke } from '@tauri-apps/api/core';
vi.mock('@tauri-apps/api/core', () => ({ invoke: vi.fn() }));

afterEach(() => { vi.unstubAllGlobals(); vi.resetModules(); });

it('suspends the audio device after the last note, including overlapping notifications', async () => {
  const oscillators: { onended: (() => void) | null; disconnect: ReturnType<typeof vi.fn> }[] = [];
  const suspend = vi.fn().mockResolvedValue(undefined);
  const resume = vi.fn().mockResolvedValue(undefined);
  vi.stubGlobal('AudioContext', class {
    currentTime = 0;
    destination = {};
    resume = resume;
    suspend = suspend;
    createOscillator() {
      const oscillator = { onended: null, disconnect: vi.fn(), frequency: { value: 0 },
        type: 'sine', connect: vi.fn(), start: vi.fn(), stop: vi.fn() };
      oscillators.push(oscillator);
      return oscillator;
    }
    createGain() {
      return { gain: { setValueAtTime: vi.fn(), linearRampToValueAtTime: vi.fn(), exponentialRampToValueAtTime: vi.fn() }, connect: vi.fn(), disconnect: vi.fn() };
    }
  });
  const { defaultNotificationSettings, playNotificationSound } = await import('./notificationSound');
  await playNotificationSound(defaultNotificationSettings);
  await playNotificationSound(defaultNotificationSettings);
  expect(oscillators).toHaveLength(4);
  for (const note of oscillators.slice(0, 3)) note.onended?.();
  expect(suspend).not.toHaveBeenCalled();
  oscillators[3].onended?.();
  expect(suspend).toHaveBeenCalledTimes(1);
  await playNotificationSound(defaultNotificationSettings);
  expect(resume).toHaveBeenCalledTimes(3);
});
it('plays imported audio at selected volume, caches decoding and releases the audio device', async () => {
 vi.mocked(invoke).mockResolvedValue(btoa('RIFF0000WAVEdata'));
 const source = { buffer: null, onended: null as (() => void) | null, connect: vi.fn(), disconnect: vi.fn(), start: vi.fn() };
 const gain = { gain: { value: 0 }, connect: vi.fn(), disconnect: vi.fn() };
 const suspend = vi.fn().mockResolvedValue(undefined);
 const decode = vi.fn().mockResolvedValue({ duration: 1 });
 vi.stubGlobal('AudioContext', class {
  destination = {};
  resume = vi.fn().mockResolvedValue(undefined);
  suspend = suspend;
  decodeAudioData = decode;
  createBufferSource = () => source;
  createGain = () => gain;
 });
 const { playNotificationSound } = await import('./notificationSound');
 const settings = { overlayEnabled: true, soundEnabled: true, sound: 'custom' as const, customSound: 'a'.repeat(64), volume: 60 };
 await playNotificationSound(settings);
 expect(gain.gain.value).toBe(.6);
 expect(source.start).toHaveBeenCalledTimes(1);
 source.onended?.();
 expect(suspend).toHaveBeenCalledTimes(1);
 await playNotificationSound(settings);
 expect(decode).toHaveBeenCalledTimes(1);
 source.onended?.();
 expect(suspend).toHaveBeenCalledTimes(2);
});
