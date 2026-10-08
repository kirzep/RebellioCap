import type {
  ActiveConfig,
  AudioCatalog,
  AudioMeasurement,
  Bootstrap,
  EngineSnapshot,
  MonitorChoice,
  OnboardingDraft,
  RecordingTestSummary,
  StoredState,
  SystemCheckResult,
} from '../config/model';

export interface ClipPlayback {
  id: string;
  video: string;
  tracks: { path: string; label: string; enabled: boolean }[];
}

export type AppSubsystem =
  | 'config'
  | 'system'
  | 'capture'
  | 'audio'
  | 'engine'
  | 'filesystem';

export type AppAction =
  | 'retry'
  | 'diagnostics'
  | 'settings'
  | 'choose-folder';

export interface AppError {
  code: string;
  summary: string;
  subsystem: AppSubsystem;
  technicalCause: string;
  retryable: boolean;
  actions: AppAction[];
}

export interface HostBridge {
  beginClipCatalog?(owner: string): Promise<void>;
  listClipPage?(request: ClipCatalogRequest): Promise<ClipCatalogPage>;
  releaseClipCatalog?(owner: string): Promise<void>;
  /** Installation owns signature verification, recording shutdown and restart. */
  checkForUpdate?(): Promise<AvailableUpdate | null>;
  installUpdate?(version: string, onProgress?: (progress: UpdateProgress) => void): Promise<void>;
  listClips?(): Promise<SavedClip[]>;
  openClip?(name: string): Promise<void>;
  prepareClipPlayback?(name: string): Promise<ClipPlayback>;
  releaseClipPlayback?(id: string): Promise<void>;
  renameClip?(name: string, newName: string): Promise<void>;
  deleteClip?(name: string): Promise<void>;
  clipThumbnail?(name: string): Promise<number[]>;
  folderIcon?(folder: string): Promise<number[]>;
  readAudioPeak?(endpointId: string): Promise<number>;
  bootstrapApp(): Promise<Bootstrap>;
  getOnboardingState(): Promise<StoredState>;
  runSystemCheck(): Promise<SystemCheckResult>;
  listMonitors(): Promise<MonitorChoice[]>;
  listAudioEndpoints(): Promise<AudioCatalog>;
  measureAudioLevel(endpointId: string | null): Promise<AudioMeasurement>;
  chooseOutputDirectory(): Promise<string | null>;
  saveOnboardingDraft(draft: OnboardingDraft, lastCompletedStep: number): Promise<void>;
  runRecordingTest(): Promise<RecordingTestSummary>;
  completeOnboarding(explicitCompletion: boolean): Promise<Bootstrap>;
  getEngineSnapshot(): Promise<EngineSnapshot>;
  startEngine(): Promise<EngineSnapshot>;
  stopEngine(): Promise<EngineSnapshot>;
  startReplay?(): Promise<EngineSnapshot>;
  stopReplay?(): Promise<EngineSnapshot>;
  saveReplay(): Promise<EngineSnapshot>;
  toggleRecording(): Promise<EngineSnapshot>;
  applyRuntimeSettings(candidate: ActiveConfig): Promise<EngineSnapshot>;
  openDiagnostics(): Promise<void>;
  openTestClip(): Promise<void>;
}

export interface AvailableUpdate {
  version: string;
  releaseNotes?: string;
}

export interface SavedClip {
  name: string;
  /** Orphaned partial recording; its last fragment may be incomplete. */
  recovery?: boolean;
  /** Path relative to the output directory; use for file operations. */
  relativePath?: string;
  folder?: string;
  savedMs?: number;
  bytes: number;
  modifiedMs: number;
}
export interface UpdateProgress {
  phase: 'downloading' | 'preparing' | 'installing';
  downloadedBytes: number;
  totalBytes?: number | null;
}

export interface ClipCatalogRequest { owner: string; query: string; folder: string | null; folders: boolean; page: number; limit: number; refresh: boolean }
export interface ClipCatalogPage { items: SavedClip[]; folders: { name: string; count: number; latest: SavedClip }[]; totalClips: number; totalMatches: number; page: number; pageCount: number }
