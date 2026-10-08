import { requestNavigation } from '../editor/navigationGuard';
import { isWindowHidden, subscribeWindowVisibility } from '../app/windowVisibility';
import React, { useCallback, useEffect, useRef, useState } from 'react';
import { ChevronDown, SlidersHorizontal } from 'lucide-react';
import { useWindowCloseGuard } from '../app/useWindowCloseGuard';
import { repairSectionForIssue } from '../app/route';
import type { AppError, HostBridge, AvailableUpdate } from '../bridge/contracts';
import { host as defaultHost, normalizeError } from '../bridge/host';
import { DiagnosticsPanel } from '../components/DiagnosticsPanel';
import { EngineState } from '../components/EngineState';
import { Button, ConfirmDialog, ErrorNotice, InlineStatus } from '../components/Primitives';
import { RecordingControls } from '../components/RecordingControls';
import {
  RuntimeSettings,
  type RuntimeSettingsSection,
} from '../components/RuntimeSettings';
import { WindowShell } from '../components/WindowShell';
import { RecordingStatus } from '../components/RecordingStatus';
import { formatBitrate, formatContainer, formatVideoMode, friendlyMonitorName } from '../config/format';
import type {
  ActiveConfig,
  AudioCatalog,
  AudioChoice,
  AudioSelection,
  EngineSnapshot,
  MonitorChoice,
  StoredState,
  SystemCheckResult,
} from '../config/model';
import styles from './HomeScreen.module.css';
import { LazyClipEditor as ClipEditor } from '../editor/LazyClipEditor';
import { ClipsScreen } from './ClipsScreen';

export interface HomeScreenProps {
  hostBridge?: HostBridge;
  /** Legacy test seam retained while callers migrate to hostBridge. */
  host?: HostBridge;
  initialState?: StoredState | null;
  initialSnapshot?: EngineSnapshot | null;
  initialView?: 'recording' | 'settings';
  initialSettingsSection?: RuntimeSettingsSection;
}

export type HomeView = 'recording' | 'settings' | 'clips' | 'editor';
type CommandName =
  | 'save'
  | 'toggle'
  | 'start'
  | 'stop'
  | 'diagnosticsCheck'
  | 'diagnosticsOpen';

interface CommandState {
  pending: boolean;
  error: AppError | null;
  success: string | null;
}

const HOME_NAVIGATION = [
  { id: 'recording', label: 'Обзор', group: 'Рабочая область' },
  { id: 'editor', label: 'Редактор клипов' },
  { id: 'clips', label: 'Клипы' },
  { id: 'video', label: 'Экран и качество', group: 'Настройки записи' },
  { id: 'audio', label: 'Звук' },
  { id: 'replay', label: 'Повторы и файлы' },
  { id: 'hotkeys', label: 'Горячие клавиши' },
  { id: 'application', label: 'Приложение', group: 'Общие настройки' },
] as const;

const IDLE_COMMAND: CommandState = {
  pending: false,
  error: null,
  success: null,
};

function normalizeHomeError(error: unknown, fallback: string): AppError {
  const normalized = normalizeError(error);
  const genericSummary =
    !normalized.summary.trim() ||
    normalized.summary === 'Неизвестная ошибка' ||
    normalized.summary === 'Служба записи не выполнила команду.';
  if (
    normalized.code === 'engine.unknown' &&
    genericSummary
  ) {
    return { ...normalized, summary: fallback };
  }
  return normalized;
}

function describeAudio(selection: AudioSelection, choices: AudioChoice[] | null): string {
  if (selection === 'disabled') return 'Не записывается';
  const choice = choices?.find((item) => item.id === selection.endpoint);
  if (!choice) {
    return choices
      ? `Устройство недоступно · ${selection.endpoint}`
      : `Сохранённое устройство · ${selection.endpoint}`;
  }
  return choice.available ? choice.name : `${choice.name} · недоступно`;
}

function settingsSectionForNativeError(
  error: EngineSnapshot['lastError']
): RuntimeSettingsSection {
  if (!error) return 'video';
  const normalized = normalizeError({ code: error.code, message: error.message });
  return repairSectionForIssue(normalized);
}

function activeConfigFingerprint(config: ActiveConfig): string {
  const audioValue = (selection: AudioSelection) =>
    selection === 'disabled' ? 'disabled' : selection.endpoint;
  const hotkeyValue = (hotkey: ActiveConfig['save_replay_hotkey']) => [
    hotkey.key,
    hotkey.ctrl,
    hotkey.alt,
    hotkey.shift,
    hotkey.win,
  ];

  return JSON.stringify([
    config.onboarding_completed,
    config.monitor_id,
    audioValue(config.system_audio),
    audioValue(config.microphone),
    config.width,
    config.height,
    config.fps,
    config.bitrate,
    config.replay_seconds,
    config.replay_memory_limit_mb ?? 0,
    config.replay_mode,
    config.container,
    config.output_directory,
    hotkeyValue(config.save_replay_hotkey),
    hotkeyValue(config.toggle_recording_hotkey),
    config.preferences.start_with_windows,
    config.continuous_recording_enabled,
  ]);
}

function describeWindowClose(
  savePending: boolean,
  recordingActive: boolean,
  stateUnknown: boolean,
  settingsBusy: boolean,
  settingsDirty: boolean
): string {
  if (savePending) {
    return 'Сначала дождёмся завершения сохранения, затем штатно остановим запись и закроем приложение.';
  }
  if (recordingActive && settingsDirty) {
    return 'Текущая запись будет корректно завершена. Изменения в настройках не применены и будут потеряны.';
  }
  if (recordingActive) {
    return 'Текущая запись будет корректно завершена перед закрытием.';
  }
  if (stateUnknown && settingsDirty) {
    return 'Текущее состояние записи не подтверждено. Служба записи будет штатно завершена, а неприменённые изменения настроек потеряны.';
  }
  if (stateUnknown) {
    return 'Текущее состояние записи не подтверждено. При закрытии приложение штатно завершит работу службы записи.';
  }
  if (settingsBusy) {
    return 'Сначала дождёмся завершения текущей операции, затем закроем приложение.';
  }
  if (settingsDirty) {
    return 'Изменения в настройках не применены и будут потеряны после подтверждения.';
  }
  return 'Текущая операция будет завершена перед закрытием приложения.';
}

export function HomeScreen({
  hostBridge,
  host,
  initialState = null,
  initialSnapshot = null,
  initialView = 'recording',
  initialSettingsSection = 'video',
}: HomeScreenProps) {
  const bridge: HostBridge = host ?? hostBridge ?? defaultHost;
  const [activeView, setActiveView] = useState<HomeView>(initialView);
  const destinationRef = useRef<HomeView>('recording');
  const [settingsSection, setSettingsSection] =
    useState<RuntimeSettingsSection>(initialSettingsSection);
  const [state, setState] = useState<StoredState | null>(initialState);
  const [snapshot, setSnapshot] = useState<EngineSnapshot | null>(initialSnapshot);
  const [isLoadingState, setIsLoadingState] = useState(initialState === null);
  const [stateLoadError, setStateLoadError] = useState<AppError | null>(null);
  const [snapshotRefreshError, setSnapshotRefreshError] = useState<AppError | null>(null);
  const [saveCommand, setSaveCommand] = useState<CommandState>({ ...IDLE_COMMAND });
  const [toggleCommand, setToggleCommand] = useState<CommandState>({ ...IDLE_COMMAND });
  const [startCommand, setStartCommand] = useState<CommandState>({ ...IDLE_COMMAND });
  const [stopCommand, setStopCommand] = useState<CommandState>({ ...IDLE_COMMAND });
  const [diagnosticsCheckCommand, setDiagnosticsCheckCommand] = useState<CommandState>({
    ...IDLE_COMMAND,
  });
  const [diagnosticsOpenCommand, setDiagnosticsOpenCommand] = useState<CommandState>({
    ...IDLE_COMMAND,
  });
  const [diagnosticsExpanded, setDiagnosticsExpanded] = useState(false);
  const [diagnosticsResult, setDiagnosticsResult] = useState<SystemCheckResult | null>(null);
  const [monitorCatalog, setMonitorCatalog] = useState<MonitorChoice[] | null>(null);
  const [audioCatalog, setAudioCatalog] = useState<AudioCatalog | null>(null);
  const [catalogLoadError, setCatalogLoadError] = useState<AppError | null>(null);
  const [catalogReloadToken, setCatalogReloadToken] = useState(0);
  const [isCatalogLoading, setIsCatalogLoading] = useState(true);
  const [settingsDirty, setSettingsDirty] = useState(false);
  const [settingsBusy, setSettingsBusy] = useState(false);
  const [availableUpdate, setAvailableUpdate] = useState<AvailableUpdate | null>(null);
  useEffect(() => {
    let disposed = false;
    if (bridge.checkForUpdate && bridge.installUpdate) {
      void bridge.checkForUpdate().then(update => {
        if (!disposed) setAvailableUpdate(update);
      }).catch(() => { /* A failed update check must not interrupt recording. */ });
    }
    return () => { disposed = true; };
  }, [bridge]);
  const [settingsCloseRequestToken, setSettingsCloseRequestToken] = useState(0);

  const mountedRef = useRef(false);
  const snapshotRequestInFlightRef = useRef(false);
  const snapshotRefreshQueuedRef = useRef(false);
  const engineGenerationRef = useRef(0);
  const acceptedRevisionRef = useRef(initialSnapshot?.revision ?? -1);
  const latestSnapshotRef = useRef<EngineSnapshot | null>(initialSnapshot);
  const stateRequestRef = useRef(0);
  const pendingHostOperationsRef = useRef(new Set<Promise<unknown>>());
  const pollingBlockingOperationsRef = useRef(new Set<Promise<unknown>>());
  const commandInFlightRef = useRef<Record<CommandName, boolean>>({
    save: false,
    toggle: false,
    start: false,
    stop: false,
    diagnosticsCheck: false,
    diagnosticsOpen: false,
  });

  function hasCommandInFlight(): boolean {
    return Object.values(commandInFlightRef.current).some(Boolean);
  }

  const trackHostOperation = useCallback(
    async function trackHostOperation<T>(
      operation: Promise<T>,
      blocksPolling = false
    ): Promise<T> {
      const tracked = operation as Promise<unknown>;
      pendingHostOperationsRef.current.add(tracked);
      if (blocksPolling) pollingBlockingOperationsRef.current.add(tracked);
      try {
        return await operation;
      } finally {
        pendingHostOperationsRef.current.delete(tracked);
        pollingBlockingOperationsRef.current.delete(tracked);
      }
    },
    []
  );

  const waitForPendingHostOperations = useCallback(async () => {
    const pending = Array.from(pendingHostOperationsRef.current);
    if (pending.length > 0) await Promise.allSettled(pending);
  }, []);

  useEffect(() => {
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
      snapshotRefreshQueuedRef.current = false;
    };
  }, []);

  const acceptSnapshot = useCallback((next: EngineSnapshot, generation: number) => {
    if (!mountedRef.current || generation !== engineGenerationRef.current) return false;

    // Polling never overlaps. A lower revision in the same generation therefore
    // indicates that the supervised engine created a new session on its own.
    if (next.revision < acceptedRevisionRef.current) {
      engineGenerationRef.current += 1;
    }

    acceptedRevisionRef.current = next.revision;
    latestSnapshotRef.current = next;
    setSnapshot(next);
    setSnapshotRefreshError(null);
    return true;
  }, []);

  const acceptRestartSnapshot = useCallback((next: EngineSnapshot) => {
    if (!mountedRef.current) return;

    engineGenerationRef.current += 1;
    acceptedRevisionRef.current = next.revision;
    latestSnapshotRef.current = next;
    setSnapshot(next);
    setSnapshotRefreshError(null);
  }, []);

  const refreshSnapshot = useCallback(
    async (queueIfBusy = false): Promise<void> => {
      if (!mountedRef.current) return;

      if (pollingBlockingOperationsRef.current.size > 0) {
        if (queueIfBusy) snapshotRefreshQueuedRef.current = true;
        return;
      }

      if (snapshotRequestInFlightRef.current) {
        if (queueIfBusy) snapshotRefreshQueuedRef.current = true;
        return;
      }

      snapshotRequestInFlightRef.current = true;
      const requestGeneration = engineGenerationRef.current;

      try {
        const next = await bridge.getEngineSnapshot();
        acceptSnapshot(next, requestGeneration);
      } catch (error) {
        if (mountedRef.current && requestGeneration === engineGenerationRef.current) {
          setSnapshotRefreshError(
            normalizeHomeError(error, 'Служба записи не ответила на запрос состояния.')
          );
        }
      } finally {
        snapshotRequestInFlightRef.current = false;
        const shouldRefreshAgain = mountedRef.current && snapshotRefreshQueuedRef.current
          && !isWindowHidden();
        snapshotRefreshQueuedRef.current = false;
        if (shouldRefreshAgain) {
          window.setTimeout(() => {
            if (!isWindowHidden()) void refreshSnapshot(false);
          }, 0);
        }
      }
    },
    [acceptSnapshot, bridge]
  );

  const loadState = useCallback(
    async (showLoading = false): Promise<StoredState | null> => {
      const requestId = stateRequestRef.current + 1;
      stateRequestRef.current = requestId;
      if (showLoading && mountedRef.current) setIsLoadingState(true);

      try {
        const next = await trackHostOperation(bridge.getOnboardingState());
        if (!mountedRef.current || requestId !== stateRequestRef.current) return null;
        setState(next);
        setStateLoadError(null);
        return next;
      } catch (error) {
        if (mountedRef.current && requestId === stateRequestRef.current) {
          setStateLoadError(
            normalizeHomeError(error, 'Не удалось получить действующие настройки записи.')
          );
        }
        return null;
      } finally {
        if (mountedRef.current && requestId === stateRequestRef.current) {
          setIsLoadingState(false);
        }
      }
    },
    [bridge, trackHostOperation]
  );

  useEffect(() => {
    void loadState(initialState === null);
  }, [initialState, loadState]);

  useEffect(() => {
    let cancelled = false;
    setIsCatalogLoading(true);

    void Promise.allSettled([
      trackHostOperation(bridge.listMonitors()),
      trackHostOperation(bridge.listAudioEndpoints()),
    ] as const).then(([monitorResult, audioResult]) => {
      if (cancelled) return;
      if (monitorResult.status === 'fulfilled') setMonitorCatalog(monitorResult.value);
      if (audioResult.status === 'fulfilled') setAudioCatalog(audioResult.value);
      const rejected =
        monitorResult.status === 'rejected'
          ? monitorResult.reason
          : audioResult.status === 'rejected'
            ? audioResult.reason
            : null;
      setCatalogLoadError(
        rejected
          ? normalizeHomeError(rejected, 'Не удалось обновить список устройств.')
          : null
      );
      setIsCatalogLoading(false);
    });

    return () => {
      cancelled = true;
    };
  }, [bridge, catalogReloadToken, trackHostOperation]);

  useEffect(() => {
    let interval: ReturnType<typeof window.setInterval> | undefined;
    const stopPolling = () => {
      if (interval !== undefined) window.clearInterval(interval);
      interval = undefined;
    };
    const refreshNow = () => {
      if (!isWindowHidden()) void refreshSnapshot(true);
    };
    const handleVisibility = () => {
      if (isWindowHidden()) {
        stopPolling();
        return;
      }
      refreshNow();
      if (interval === undefined) {
        interval = window.setInterval(() => {
          if (!isWindowHidden()) void refreshSnapshot(false);
        }, 1000);
      }
    };
    handleVisibility();
    window.addEventListener('focus', refreshNow);
    const unsubscribe = subscribeWindowVisibility(handleVisibility);

    return () => {
      stopPolling();
      window.removeEventListener('focus', refreshNow);
      unsubscribe();
    };
  }, [refreshSnapshot]);

  useEffect(() => {
    if (!saveCommand.success) return undefined;
    const timeout = window.setTimeout(() => {
      setSaveCommand((current) => ({ ...current, success: null }));
    }, 5000);
    return () => window.clearTimeout(timeout);
  }, [saveCommand.success]);

  async function handleSaveReplay() {
    if (hasCommandInFlight()) return;
    commandInFlightRef.current.save = true;
    setSaveCommand({ pending: true, error: null, success: null });

    try {
      const next = await trackHostOperation(bridge.saveReplay(), true);
      acceptRestartSnapshot(next);
      if (mountedRef.current) {
        setSaveCommand({ pending: false, error: null, success: 'Повтор сохранён.' });
      }
    } catch (error) {
      if (mountedRef.current) {
        setSaveCommand({
          pending: false,
          error: normalizeHomeError(error, 'Не удалось сохранить повтор.'),
          success: null,
        });
      }
    } finally {
      commandInFlightRef.current.save = false;
      void refreshSnapshot(true);
    }
  }

  async function handleToggleContinuous() {
    if (hasCommandInFlight()) return;
    commandInFlightRef.current.toggle = true;
    setToggleCommand({ pending: true, error: null, success: null });

    try {
      const next = await trackHostOperation(bridge.toggleRecording(), true);
      acceptRestartSnapshot(next);
      if (mountedRef.current) {
        setToggleCommand({
          pending: false,
          error: null,
          success: next.continuousRecordingActive
            ? 'Обычная запись начата.'
            : 'Обычная запись остановлена.',
        });
      }
    } catch (error) {
      if (mountedRef.current) {
        setToggleCommand({
          pending: false,
          error: normalizeHomeError(
            error,
            'Не удалось изменить состояние обычной записи.'
          ),
          success: null,
        });
      }
    } finally {
      commandInFlightRef.current.toggle = false;
      void refreshSnapshot(true);
    }
  }

  async function handleStartReplay() {
    if (hasCommandInFlight()) return;
    commandInFlightRef.current.start = true;
    setStartCommand({ pending: true, error: null, success: null });
    setStopCommand((current) => ({ ...current, error: null, success: null }));

    try {
      const next = await trackHostOperation(bridge.startReplay ? bridge.startReplay() : bridge.startEngine(), true);
      acceptRestartSnapshot(next);
      if (mountedRef.current) {
        setStartCommand({ pending: false, error: null, success: 'Replay включён.' });
      }
    } catch (error) {
      if (mountedRef.current) {
        setStartCommand({
          pending: false,
          error: normalizeHomeError(error, 'Не удалось включить Replay.'),
          success: null,
        });
      }
    } finally {
      commandInFlightRef.current.start = false;
      void refreshSnapshot(true);
    }
  }

  async function handleStopReplay() {
    if (hasCommandInFlight()) return;
    commandInFlightRef.current.stop = true;
    setStopCommand({ pending: true, error: null, success: null });
    setStartCommand((current) => ({ ...current, error: null, success: null }));

    try {
      const next = await trackHostOperation(bridge.stopReplay ? bridge.stopReplay() : bridge.stopEngine(), true);
      acceptRestartSnapshot(next);
      if (mountedRef.current) {
        setStopCommand({ pending: false, error: null, success: 'Replay выключен.' });
      }
    } catch (error) {
      if (mountedRef.current) {
        setStopCommand({
          pending: false,
          error: normalizeHomeError(error, 'Не удалось выключить Replay.'),
          success: null,
        });
      }
    } finally {
      commandInFlightRef.current.stop = false;
      void refreshSnapshot(true);
    }
  }

  async function handleApplySettings(candidate: ActiveConfig) {
    try {
      await waitForPendingHostOperations();
      const next = await trackHostOperation(bridge.applyRuntimeSettings(candidate), true);
      acceptRestartSnapshot(next);

      const confirmedState = await loadState(false);
      if (
        !confirmedState?.active ||
        activeConfigFingerprint(confirmedState.active) !== activeConfigFingerprint(candidate)
      ) {
        throw new Error(
          'Служба применила настройки, но не удалось подтвердить сохранённую конфигурацию.'
        );
      }
      setSaveCommand({ ...IDLE_COMMAND });
      setToggleCommand({ ...IDLE_COMMAND });
      setStartCommand({ ...IDLE_COMMAND });
      setStopCommand({ ...IDLE_COMMAND });
      setDiagnosticsResult(null);
      setDiagnosticsCheckCommand({ ...IDLE_COMMAND });
      setDiagnosticsOpenCommand({ ...IDLE_COMMAND });
      await refreshSnapshot(true);
    } catch (cause) {
      // applyRuntimeSettings may have stopped the old engine and started either the
      // candidate or a rollback session before rejecting. Accept the next host
      // snapshot as a new revision generation without claiming rollback succeeded.
      engineGenerationRef.current += 1;
      acceptedRevisionRef.current = -1;
      await Promise.all([loadState(false), refreshSnapshot(true)]);
      throw cause;
    }
  }

  function handleSettingsSnapshotChange(next: EngineSnapshot) {
    acceptRestartSnapshot(next);
  }

  async function handleRunDiagnostics() {
    if (hasCommandInFlight()) return;
    commandInFlightRef.current.diagnosticsCheck = true;
    setDiagnosticsCheckCommand({ pending: true, error: null, success: null });
    setDiagnosticsOpenCommand((current) => ({ ...current, error: null }));
    try {
      await waitForPendingHostOperations();
      const result = await trackHostOperation(bridge.runSystemCheck(), true);
      if (mountedRef.current) {
        setDiagnosticsResult(result);
        setDiagnosticsCheckCommand({ pending: false, error: null, success: null });
      }
    } catch (cause) {
      if (mountedRef.current) {
        setDiagnosticsCheckCommand({
          pending: false,
          error: normalizeHomeError(
            cause,
            'Не удалось проверить компьютер.'
          ),
          success: null,
        });
      }
    } finally {
      commandInFlightRef.current.diagnosticsCheck = false;
      void refreshSnapshot(true);
    }
  }

  async function handleOpenDiagnostics() {
    if (!diagnosticsResult?.diagnostics_path || hasCommandInFlight()) return;
    commandInFlightRef.current.diagnosticsOpen = true;
    setDiagnosticsOpenCommand({ pending: true, error: null, success: null });
    try {
      await waitForPendingHostOperations();
      await trackHostOperation(bridge.openDiagnostics(), true);
      if (mountedRef.current) {
        setDiagnosticsOpenCommand({ pending: false, error: null, success: null });
      }
    } catch (cause) {
      if (mountedRef.current) {
        setDiagnosticsOpenCommand({
          pending: false,
          error: normalizeHomeError(cause, 'Не удалось открыть диагностический отчёт.'),
          success: null,
        });
      }
    } finally {
      commandInFlightRef.current.diagnosticsOpen = false;
    }
  }

  function requestStopReplay() {
    void handleStopReplay();
  }

  const closeSettings = useCallback(() => {
    setSettingsDirty(false);
    setSettingsBusy(false);
    setActiveView(destinationRef.current);
  }, []);

  const handleSettingsDirtyChange = useCallback((dirty: boolean) => {
    setSettingsDirty(dirty);
  }, []);

  const handleSettingsBusyChange = useCallback((busy: boolean) => {
    setSettingsBusy(busy);
  }, []);

  const stopEngineFromSettings = useCallback(
    async () => {
      await waitForPendingHostOperations();
      return trackHostOperation(bridge.stopEngine(), true);
    },
    [bridge, trackHostOperation, waitForPendingHostOperations]
  );

  const startEngineFromSettings = useCallback(
    async () => {
      await waitForPendingHostOperations();
      return trackHostOperation(bridge.startEngine(), true);
    },
    [bridge, trackHostOperation, waitForPendingHostOperations]
  );

  const prepareWindowClose = useCallback(async () => {
    await waitForPendingHostOperations();

    const current = latestSnapshotRef.current;
    const engineNeedsStop =
      current !== null &&
      (current.replayActive ||
        current.continuousRecordingActive ||
        current.lifecycle === 'starting' ||
        current.lifecycle === 'ready' ||
        current.lifecycle === 'recovering' ||
        current.lifecycle === 'degraded');

    if (engineNeedsStop) {
      const stopped = await trackHostOperation(bridge.stopEngine(), true);
      latestSnapshotRef.current = stopped;
      acceptRestartSnapshot(stopped);
    }
  }, [acceptRestartSnapshot, bridge, trackHostOperation, waitForPendingHostOperations]);

  const activeConfig = state?.active ?? null;
  const activeMonitor = activeConfig
    ? monitorCatalog?.find((monitor) => monitor.id === activeConfig.monitor_id)
    : undefined;
  const activeMonitorName = activeConfig
    ? (activeMonitor ? friendlyMonitorName(activeMonitor.name) : undefined) ??
      (monitorCatalog
        ? 'Сохранённый экран · недоступен'
        : 'Сохранённый экран')
    : '';
  const snapshotIsStale = snapshotRefreshError !== null;
  const closeStopsEngine =
    !snapshot ||
    snapshotIsStale ||
    snapshot.replayActive ||
    snapshot.continuousRecordingActive ||
    ['starting', 'ready', 'recovering', 'degraded'].includes(snapshot.lifecycle);
  const recordingCommandPending =
    saveCommand.pending ||
    toggleCommand.pending ||
    startCommand.pending ||
    stopCommand.pending;
  const diagnosticsPending =
    diagnosticsCheckCommand.pending || diagnosticsOpenCommand.pending;
  const commandPending = recordingCommandPending || diagnosticsPending;
  const closeGuard = useWindowCloseGuard({
    shouldConfirm:
      !snapshot ||
      snapshotIsStale ||
      Boolean(snapshot?.continuousRecordingActive) ||
      commandPending ||
      settingsBusy ||
      settingsDirty,
    onBeforeClose: prepareWindowClose,
  });
  const settingsEngineError = snapshot?.lastError
    ? normalizeError({
        code: snapshot.lastError.code,
        message: snapshot.lastError.message,
      })
    : null;
  const navigation = HOME_NAVIGATION.filter(item => item.id === 'recording' || item.id === 'clips' || item.id === 'editor').map((item) => ({
    ...item,
    disabled: settingsBusy && item.id !== activeView,
  }));

  function handleNavigation(id: string) { requestNavigation(() => navigate(id)); }

  function navigate(id: string) {
    if (settingsBusy) return;
    if (id === 'settings') {
      setActiveView('settings');
      return;
    }
    if (['video', 'audio', 'replay', 'hotkeys', 'application'].includes(id)) {
      setSettingsSection(id as RuntimeSettingsSection);
      setActiveView('settings');
      return;
    }
    if (id !== 'recording' && id !== 'clips' && id !== 'editor') return;
    destinationRef.current = id;
    if (activeView === 'settings') {
      setSettingsCloseRequestToken((current) => current + 1);
      return;
    }
    closeSettings();
  }

  function openSettings(section: RuntimeSettingsSection = 'video') {
    requestNavigation(() => { setSettingsSection(section); setActiveView('settings'); });
  }

  useEffect(() => {
    if (!('__TAURI_INTERNALS__' in window)) return;
    let disposed = false;
    const listener = import('@tauri-apps/api/event').then(({ listen }) =>
      listen('tray-settings', () => {
        requestNavigation(() => { setSettingsSection('application'); setActiveView('settings'); });
      })
    );
    void listener.then(remove => { if (disposed) remove(); });
    return () => { disposed = true; void listener.then(remove => remove()); };
  }, []);

  return (
    <WindowShell
      windowStatus={<RecordingStatus snapshot={snapshot} unavailable={Boolean(snapshotRefreshError)} />}
      navigation={navigation}
      settingsNavigation={{ id: 'settings', label: 'Настройки', disabled: settingsBusy }}
      availableUpdate={availableUpdate}
      onInstallUpdate={availableUpdate && bridge.installUpdate ? () => bridge.installUpdate!(availableUpdate.version) : undefined}
      activeNavigation={activeView}
      onNavigate={handleNavigation}
      contentWidth={activeView === 'settings' ? 'form' : 'wide'}
      contentLabel={activeView === 'recording' ? 'Обзор' : activeView === 'clips' ? 'Клипы' : 'Настройки'}
    >
      <header className={styles.pageHeader}>
        <div><p className={styles.breadcrumb}>Рабочая область <span>/</span> {activeView === 'recording' ? 'Обзор' : activeView === 'clips' ? 'Клипы' : 'Настройки'}</p>
        <h1>{activeView === 'recording' ? 'Обзор записи' : activeView === 'clips' ? 'Ваши клипы' : HOME_NAVIGATION.find(item => item.id === settingsSection)?.label}</h1>
        <p className={styles.pageDescription}>{activeView === 'recording' ? 'Записывайте экран и сохраняйте то, что хочется оставить.' : activeView === 'clips' ? 'Всё, что вы сохранили, в одном месте.' : 'Настройте запись под себя.'}</p></div>
      </header>
      {activeView === 'recording' ? (
        <div className={styles.screen} data-testid="home-panel">
          {stateLoadError && activeConfig && (
            <ErrorNotice
              title="Не удалось обновить настройки"
              tone="warning"
              technicalDetails={`Код: ${stateLoadError.code}\n${stateLoadError.technicalCause}`}
              action={
                <Button size="compact" onClick={() => void loadState(false)}>
                  Повторить
                </Button>
              }
            >
              {stateLoadError.summary} Ниже показаны последние подтверждённые значения.
            </ErrorNotice>
          )}

          {!snapshot ? (
            <section className={styles.loadingState} aria-labelledby="recording-loading-title">
              <div>
                <h1 id="recording-loading-title" className={styles.loadingTitle}>
                  Получаем состояние записи…
                </h1>
                <InlineStatus tone={snapshotRefreshError ? 'warning' : 'busy'}>
                  {snapshotRefreshError
                    ? 'Служба записи пока не ответила'
                    : 'Связываемся со службой записи'}
                </InlineStatus>
              </div>
              {snapshotRefreshError && (
                <ErrorNotice
                  title="Не удаётся получить состояние"
                  technicalDetails={`Код: ${snapshotRefreshError.code}\n${snapshotRefreshError.technicalCause}`}
                  action={
                    <Button size="compact" onClick={() => void refreshSnapshot(true)}>
                      Повторить
                    </Button>
                  }
                >
                  {snapshotRefreshError.summary}
                </ErrorNotice>
              )}
            </section>
          ) : (
            <>
              {snapshotRefreshError && (
                <ErrorNotice
                  title="Не удаётся обновить состояние"
                  tone="warning"
                  technicalDetails={`Код: ${snapshotRefreshError.code}\n${snapshotRefreshError.technicalCause}`}
                  action={
                    <Button size="compact" onClick={() => void refreshSnapshot(true)}>
                      Повторить
                    </Button>
                  }
                >
                  {snapshotRefreshError.summary} Команды записи недоступны, пока связь не восстановлена.
                </ErrorNotice>
              )}

              <EngineState
                snapshot={snapshot}
                isStale={snapshotIsStale}
                onOpenDiagnostics={() => setDiagnosticsExpanded((current) => !current)}
                onOpenSettings={
                  activeConfig
                    ? () => openSettings(settingsSectionForNativeError(snapshot.lastError))
                    : undefined
                }
                diagnosticsDisabled={recordingCommandPending}
                diagnosticsExpanded={diagnosticsExpanded}
              />

              {diagnosticsExpanded && (
                <DiagnosticsPanel
                  snapshot={snapshot}
                  result={diagnosticsResult}
                  checkError={diagnosticsCheckCommand.error}
                  openError={diagnosticsOpenCommand.error}
                  isChecking={diagnosticsCheckCommand.pending}
                  isOpeningReport={diagnosticsOpenCommand.pending}
                  commandsDisabled={recordingCommandPending}
                  onRunCheck={handleRunDiagnostics}
                  onOpenReport={handleOpenDiagnostics}
                />
              )}

              {activeConfig ? (
                <RecordingControls
                  snapshot={snapshot}
                  activeConfig={activeConfig}
                  disabled={snapshotIsStale || diagnosticsPending}
                  isSavingReplay={saveCommand.pending}
                  isTogglingContinuous={toggleCommand.pending}
                  isStartingReplay={startCommand.pending}
                  isStoppingReplay={stopCommand.pending}
                  saveReplayError={saveCommand.error}
                  saveReplaySuccess={saveCommand.success}
                  toggleContinuousError={toggleCommand.error}
                  toggleContinuousSuccess={toggleCommand.success}
                  startReplayError={startCommand.error}
                  startReplaySuccess={startCommand.success}
                  stopReplayError={stopCommand.error}
                  stopReplaySuccess={stopCommand.success}
                  onSaveReplay={handleSaveReplay}
                  onToggleContinuous={handleToggleContinuous}
                  onStartReplay={handleStartReplay}
                  onStopReplay={requestStopReplay}
                  onOpenSettings={openSettings}
                />
              ) : isLoadingState ? (
                <InlineStatus tone="busy">Получаем действующие настройки…</InlineStatus>
              ) : (
                <ErrorNotice
                  title="Действующая конфигурация не найдена"
                  action={
                    <Button size="compact" onClick={() => void loadState(true)}>
                      Повторить
                    </Button>
                  }
                >
                  Команды записи недоступны. Настройки нужно восстановить через сохранённый профиль.
                  {stateLoadError ? ` ${stateLoadError.summary}` : ''}
                </ErrorNotice>
              )}

              {activeConfig && (
                <details className={styles.configuration}>
                  <summary className={styles.configurationToggle}>
                    <SlidersHorizontal size={16} aria-hidden="true" />
                    <span>Параметры записи</span>
                    <ChevronDown size={16} className={styles.configurationChevron} aria-hidden="true" />
                  </summary>

                  {isCatalogLoading && !catalogLoadError && (
                    <InlineStatus tone="busy" className={styles.configurationStatus}>
                      Уточняем названия и доступность устройств…
                    </InlineStatus>
                  )}

                  {catalogLoadError && (
                    <ErrorNotice
                      title="Не удалось обновить список устройств"
                      tone="warning"
                      className={styles.configurationStatus}
                      technicalDetails={`Код: ${catalogLoadError.code}\n${catalogLoadError.technicalCause}`}
                      action={
                        <Button
                          variant="tertiary"
                          size="compact"
                          busy={isCatalogLoading}
                          busyLabel="Обновляем…"
                          onClick={() => setCatalogReloadToken((value) => value + 1)}
                        >
                          Повторить
                        </Button>
                      }
                    >
                      {catalogLoadError.summary} Действующие настройки не изменены; названия и
                      доступность устройств могут быть неактуальны.
                    </ErrorNotice>
                  )}

                  <dl className={styles.configurationGrid}>
                    <div className={styles.configurationItem}>
                      <dt>Экран</dt>
                      <dd>
                        <span>{activeMonitorName}</span>
                        <small>
                          {formatVideoMode(
                            activeConfig.width,
                            activeConfig.height,
                            activeConfig.fps,
                            activeConfig.bitrate
                          )}
                        </small>
                      </dd>
                    </div>
                    <div className={styles.configurationItem}>
                      <dt>Звук</dt>
                      <dd>
                        <span>
                          Система: {describeAudio(activeConfig.system_audio, audioCatalog?.system_audio ?? null)}
                        </span>
                        <small>
                          Микрофон: {describeAudio(activeConfig.microphone, audioCatalog?.microphones ?? null)}
                        </small>
                      </dd>
                    </div>
                    <div className={styles.configurationItem}>
                      <dt>Папка</dt>
                      <dd>
                        <code className={styles.path}>{activeConfig.output_directory}</code>
                      </dd>
                    </div>
                    <div className={styles.configurationItem}>
                      <dt>Файл</dt>
                      <dd>
                        <span>H.264 · {formatContainer(activeConfig.container)}</span>
                        <small>{formatBitrate(activeConfig.bitrate)}</small>
                      </dd>
                    </div>
                  </dl>
                </details>
              )}
            </>
          )}
          {activeConfig && <ClipsScreen bridge={bridge} directory={activeConfig.output_directory} refreshToken={`${snapshot?.metrics.completedSaves}:${snapshot?.continuousRecordingActive}`} compact onShowAll={() => handleNavigation('clips')} />}
        </div>
      ) : activeView === 'editor' ? <ClipEditor onClose={() => {destinationRef.current = 'clips';setActiveView('clips');}} /> : activeView === 'clips' ? <ClipsScreen bridge={bridge} directory={activeConfig?.output_directory} /> : (
        <div className={styles.settingsView}>
          {!activeConfig && (
            <header className={styles.settingsHeader}>
              <p className={styles.eyebrow}>Действующий профиль</p>
              <h1 className={styles.settingsTitle}>Настройки записи</h1>
              <p className={styles.settingsDescription}>
                Изменения применяются к службе записи после подтверждения.
              </p>
            </header>
          )}

          {stateLoadError && (
            <ErrorNotice
              title="Не удалось обновить настройки"
              tone={activeConfig ? 'warning' : 'danger'}
              technicalDetails={`Код: ${stateLoadError.code}\n${stateLoadError.technicalCause}`}
              action={
                <Button size="compact" onClick={() => void loadState(true)}>
                  Повторить
                </Button>
              }
            >
              {stateLoadError.summary}
            </ErrorNotice>
          )}

          {settingsEngineError && (
            <ErrorNotice
              title="Служба записи сообщила об ошибке"
              technicalDetails={`Код: ${settingsEngineError.code}\n${settingsEngineError.technicalCause}`}
              data-testid="settings-engine-error"
            >
              {settingsEngineError.summary} Проверьте соответствующие параметры и примените изменения.
            </ErrorNotice>
          )}

          {snapshotRefreshError && (
            <ErrorNotice
              title="Состояние службы записи не подтверждено"
              tone="warning"
              technicalDetails={`Код: ${snapshotRefreshError.code}\n${snapshotRefreshError.technicalCause}`}
              action={
                <Button size="compact" onClick={() => void refreshSnapshot(true)}>
                  Повторить
                </Button>
              }
            >
              {snapshotRefreshError.summary} Настройки можно редактировать, но перед опасными действиями
              приложение повторно остановит возможную запись.
            </ErrorNotice>
          )}

          {!activeConfig ? (
            isLoadingState ? (
              <InlineStatus tone="busy">Получаем действующие настройки…</InlineStatus>
            ) : (
              <ErrorNotice title="Действующая конфигурация не найдена">
                Настройки нельзя открыть без подтверждённого профиля записи.
              </ErrorNotice>
            )
          ) : (
            <>
              {!snapshot && !snapshotRefreshError && (
                <div className={styles.settingsLoading}>
                  <InlineStatus tone="busy">Получаем состояние службы записи…</InlineStatus>
                </div>
              )}
              <RuntimeSettings
                hostBridge={bridge}
                activeConfig={activeConfig}
                snapshot={snapshotIsStale ? null : snapshot}
                onApply={handleApplySettings}
                onStopEngine={stopEngineFromSettings}
                onStartEngine={startEngineFromSettings}
                onSnapshotChange={handleSettingsSnapshotChange}
                onDirtyChange={handleSettingsDirtyChange}
                onBusyChange={handleSettingsBusyChange}
                onClose={closeSettings}
                closeRequestToken={settingsCloseRequestToken}
                externalCloseRequested={closeGuard.closeRequested}
                initialSection={settingsSection}
                sidebarNavigation={false}
                onSectionChange={setSettingsSection}
              />
            </>
          )}
        </div>
      )}

      <ConfirmDialog
        open={closeGuard.closeRequested}
        title={
          snapshot?.continuousRecordingActive
            ? 'Остановить запись и закрыть RebellioCap?'
            : 'Закрыть RebellioCap?'
        }
        description={describeWindowClose(
          saveCommand.pending,
          Boolean(snapshot?.continuousRecordingActive),
          !snapshot || snapshotIsStale,
          settingsBusy,
          settingsDirty
        )}
        confirmLabel={closeStopsEngine ? 'Остановить и закрыть' : 'Закрыть'}
        cancelLabel={
          settingsDirty
            ? 'Продолжить редактирование'
            : snapshot?.continuousRecordingActive
              ? 'Продолжить запись'
              : 'Остаться в приложении'
        }
        destructive
        busy={closeGuard.isClosing}
        busyLabel="Завершаем…"
        onConfirm={() => void closeGuard.confirmClose()}
        onCancel={closeGuard.cancelClose}
      >
        {closeGuard.closeError && (
          <ErrorNotice
            title="Не удалось безопасно закрыть приложение"
            technicalDetails={`Код: ${closeGuard.closeError.code}\n${closeGuard.closeError.technicalCause}`}
          >
            {closeGuard.closeError.summary}
          </ErrorNotice>
        )}
      </ConfirmDialog>
    </WindowShell>
  );
}
