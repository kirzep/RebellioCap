import recordingContract from '../../../../contracts/recording-settings.v1.json';
import type { Hotkey, MonitorChoice, OnboardingDraft } from './model';

export const DEFAULT_SAVE_REPLAY_HOTKEY: Hotkey = { ...recordingContract.defaults.save_replay_hotkey };
export const DEFAULT_TOGGLE_RECORDING_HOTKEY: Hotkey = { ...recordingContract.defaults.toggle_recording_hotkey };

export function createDraftDefaults(): OnboardingDraft {
  return {
    microphone: 'disabled',
    replay_seconds: 30,
    replay_memory_limit_mb: 0,
    replay_mode: 'ram',
    container: 'mp4',
    save_replay_hotkey: { ...DEFAULT_SAVE_REPLAY_HOTKEY },
    toggle_recording_hotkey: { ...DEFAULT_TOGGLE_RECORDING_HOTKEY },
    preferences: {
      start_with_windows: false,
    },
    continuous_recording_enabled: true,
  };
}

function evenFloor(value: number): number {
  return Math.max(64, Math.floor(value / 2) * 2);
}

export function balancedVideoForMonitor(monitor: MonitorChoice): Pick<
  OnboardingDraft,
  'width' | 'height' | 'fps' | 'bitrate'
> {
  const scale = Math.min(1, 1920 / monitor.width, 1080 / monitor.height);
  return {
    width: evenFloor(monitor.width * scale),
    height: evenFloor(monitor.height * scale),
    fps: 60,
    bitrate: 12_000_000,
  };
}
