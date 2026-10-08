import { afterEach, expect, it, vi } from 'vitest';

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
