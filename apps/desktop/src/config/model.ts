export type AudioSelection =
  | 'disabled'
  | { endpoint: string };

export type ReplayMode = 'ram' | 'disk';

export type Container = 'mp4' | 'mkv';

export interface Hotkey {
  key: number;
  ctrl: boolean;
  alt: boolean;
  shift: boolean;
  win: boolean;
}

export interface Preferences {
  /** Legacy configuration input; active overlay settings live in NotificationSettings. */
  overlay_enabled?: boolean;
  start_with_windows: boolean;
}

export interface RecordingTestSummary {
  fingerprint: string;
  succeeded: boolean;
  clip_path: string;
  video_packets: number;
  audio_packets: number;
}

export interface OnboardingProgress {
  completed: boolean;
  last_completed_step: number;
}

export interface OnboardingDraft {
  monitor_id?: string | null;
  system_audio?: AudioSelection | null;
  microphone?: AudioSelection | null;
  width?: number | null;
  height?: number | null;
  fps?: number | null;
  bitrate?: number | null;
  replay_seconds?: number | null;
  replay_memory_limit_mb?: number | null;
  replay_mode?: ReplayMode | null;
  container?: Container | null;
  output_directory?: string | null;
  save_without_game_folders?: boolean | null;
  save_replay_hotkey?: Hotkey | null;
  toggle_recording_hotkey?: Hotkey | null;
  preferences?: Preferences | null;
  continuous_recording_enabled?: boolean | null;
}

export interface ActiveConfig {
  onboarding_completed: boolean;
  monitor_id: string;
  system_audio: AudioSelection;
  microphone: AudioSelection;
  width: number;
  height: number;
  fps: number;
  bitrate: number;
  replay_seconds: number;
  replay_memory_limit_mb?: number;
  replay_mode: ReplayMode;
  container: Container;
  output_directory: string;
  save_without_game_folders?: boolean;
  save_replay_hotkey: Hotkey;
  toggle_recording_hotkey: Hotkey;
  preferences: Preferences;
  continuous_recording_enabled: boolean;
}

export interface StoredState {
  schema_version: number;
  onboarding: OnboardingProgress;
  draft: OnboardingDraft;
  active: ActiveConfig | null;
  last_successful_test: RecordingTestSummary | null;
}

export type EngineLifecycle =
  | 'starting'
  | 'ready'
  | 'stopped'
  | 'recovering'
  | 'degraded'
  | 'blocked'
  | 'failed';

export interface EngineMetrics {
  videoTicks: number;
  missedVideoDeadlines: number;
  videoPackets: number;
  audioPackets: number;
  /** PCM frames discarded from mixing copies, summed across sources. */
  audioMixingDroppedFrames?: number;
  /** Bit 0: system audio unavailable; bit 1: microphone unavailable. */
  audioUnavailableSources?: number;
  saveRequests: number;
  completedSaves: number;
  failedSaves: number;
  rejectedSaves: number;
  pipelineErrors: number;
  continuousPackets: number;
  continuousFailures: number;
  continuousRecoveryPath?: string;
  continuousRecordingActive: boolean;
  lastHotkeySaveLatencyTicks: number;
  replayBytes: number;
  /** Budget for retained encoded data; excludes GPU/codec memory. */
  budgetBytes?: number;
  bufferedBytes?: number;
  replayRetainedSeconds?: number;
  replayMemoryLimited?: boolean;
  replayMemoryDrops?: number;
}

export interface NativeError {
  code: string;
  message: string;
  hresult: number | null;
}

export interface EngineSnapshot {
  revision: number;
  lifecycle: EngineLifecycle;
  replayActive: boolean;
  continuousRecordingActive: boolean;
  replaySeconds: number;
  metrics: EngineMetrics;
  lastError: NativeError | null;
}

export interface MonitorChoice {
  id: string;
  name: string;
  width: number;
  height: number;
  primary: boolean;
}

export interface AudioChoice {
  id: string;
  name: string;
  is_default: boolean;
  available: boolean;
}

export interface AudioCatalog {
  system_audio: AudioChoice[];
  microphones: AudioChoice[];
}

export interface SystemCheckStage {
  id: string;
  passed: boolean;
  message: string;
}

export interface SystemCheckResult {
  stages: SystemCheckStage[];
  passed: boolean;
  diagnostics_path: string | null;
}

export interface AudioLevel {
  timestamp_ms: number;
  level: number;
}

export interface AudioMeasurement {
  samples: AudioLevel[];
  cancelled: boolean;
}

export interface Bootstrap {
  route: string;
  state: StoredState;
  snapshot: EngineSnapshot;
}
