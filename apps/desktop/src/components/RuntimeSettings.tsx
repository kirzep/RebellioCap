import { t, useTranslation } from '../i18n';
import { ToggleGroup, ToggleGroupItem } from './ui/toggle-group';
import React, { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import type { HostBridge } from '../bridge/contracts';
import type { AppError } from '../bridge/contracts';
import { normalizeError } from '../bridge/host';
import { formatVideoMode } from '../config/format';
import type { ActiveConfig, EngineSnapshot, OnboardingDraft } from '../config/model';
import {
  areHotkeysEqual,
  validateCompleteDraft,
  type ValidationError,
} from '../config/validation';
import { AudioStep } from '../onboarding/steps/AudioStep';
import { HotkeysStep } from '../onboarding/steps/HotkeysStep';
import { ReplayStep } from '../onboarding/steps/ReplayStep';
import { VideoStep } from '../onboarding/steps/VideoStep';
import { Button, ConfirmDialog, ErrorNotice, InlineStatus } from './Primitives';
import styles from './RuntimeSettings.module.css';
import { ApplicationSettings } from './ApplicationSettings';

export type RuntimeSettingsSection = 'video' | 'audio' | 'replay' | 'hotkeys' | 'application';
type HotkeyTarget = 'replay' | 'recording';

const sections: readonly { id: RuntimeSettingsSection; label: string }[] = [
  { id: 'video', label: 'Экран' },
  { id: 'audio', label: 'Звук' },
  { id: 'replay', label: 'Повторы и файлы' },
  { id: 'hotkeys', label: 'Горячие клавиши' },
  { id: 'application', label: 'Приложение' },
];

const validationFieldSelectors: Record<string, string> = {
  monitor_id: '#video-monitor',
  width: '#video-width',
  height: '#video-height',
  fps: '#video-fps, #video-custom-fps',
  bitrate: '#video-bitrate',
  system_audio: '#audio-system',
  microphone: '#audio-microphone',
  replay_seconds: '#replay-duration-preset, #replay-seconds',
  replay_memory_limit_mb: '#replay-memory-limit',
  replay_mode: '[data-testid="replay-mode-ram-button"]',
  container: '[data-testid="container-mp4-button"]',
  output_directory: '#output-directory',
  save_replay_hotkey: '[data-testid="record-replay-hotkey-button"]',
  toggle_recording_hotkey: '[data-testid="record-recording-hotkey-button"]',
  duplicate_hotkeys: '[data-testid="record-replay-hotkey-button"]',
  continuous_recording_enabled: '[data-testid="continuous-recording-checkbox"]',
};

export interface RuntimeSettingsProps {
  hostBridge: HostBridge;
  activeConfig: ActiveConfig;
  snapshot: EngineSnapshot | null;
  onApply: (candidate: ActiveConfig) => Promise<void>;
  onStopEngine?: () => Promise<EngineSnapshot>;
  onStartEngine?: () => Promise<EngineSnapshot>;
  onSnapshotChange?: (snapshot: EngineSnapshot) => void;
  onDirtyChange?: (dirty: boolean) => void;
  onBusyChange?: (busy: boolean) => void;
  onClose?: () => void;
  closeRequestToken?: number;
  externalCloseRequested?: boolean;
  initialSection?: RuntimeSettingsSection;
  sidebarNavigation?: boolean;
  onSectionChange?: (section: RuntimeSettingsSection) => void;
}

interface HotkeyPrompt {
  target: HotkeyTarget;
  replayWasActive: boolean | null;
  resolve: (allowed: boolean) => void;
}

function activeAsDraft(active: ActiveConfig): OnboardingDraft {
  return {
    monitor_id: active.monitor_id,
    system_audio:
      typeof active.system_audio === 'object' ? { ...active.system_audio } : active.system_audio,
    microphone:
      typeof active.microphone === 'object' ? { ...active.microphone } : active.microphone,
    width: active.width,
    height: active.height,
    fps: active.fps,
    bitrate: active.bitrate,
    replay_seconds: active.replay_seconds,
    replay_memory_limit_mb: active.replay_memory_limit_mb ?? 0,
    replay_mode: active.replay_mode,
    container: active.container,
    output_directory: active.output_directory,
    save_without_game_folders: active.save_without_game_folders ?? false,
    save_replay_hotkey: { ...active.save_replay_hotkey },
    toggle_recording_hotkey: { ...active.toggle_recording_hotkey },
    preferences: { ...active.preferences },
    continuous_recording_enabled: active.continuous_recording_enabled,
  };
}

function required<T>(value: T | null | undefined, field: string): T {
  if (value == null) throw new Error(`validation.required:${field}`);
  return value;
}

function draftAsActive(draft: OnboardingDraft): ActiveConfig {
  return {
    onboarding_completed: true,
    monitor_id: required(draft.monitor_id, 'monitor_id'),
    system_audio: required(draft.system_audio, 'system_audio'),
    microphone: required(draft.microphone, 'microphone'),
    width: required(draft.width, 'width'),
    height: required(draft.height, 'height'),
    fps: required(draft.fps, 'fps'),
    bitrate: required(draft.bitrate, 'bitrate'),
    replay_seconds: required(draft.replay_seconds, 'replay_seconds'),
    replay_memory_limit_mb: draft.replay_memory_limit_mb ?? 0,
    replay_mode: required(draft.replay_mode, 'replay_mode'),
    container: required(draft.container, 'container'),
    output_directory: required(draft.output_directory, 'output_directory'),
    save_without_game_folders: draft.save_without_game_folders ?? false,
    save_replay_hotkey: required(draft.save_replay_hotkey, 'save_replay_hotkey'),
    toggle_recording_hotkey: required(
      draft.toggle_recording_hotkey,
      'toggle_recording_hotkey'
    ),
    preferences: required(draft.preferences, 'preferences'),
    continuous_recording_enabled: required(
      draft.continuous_recording_enabled,
      'continuous_recording_enabled'
    ),
  };
}

function firstValidationMessage(errors: ValidationError[]): string {
  if (errors.length === 0) return '';
  if (errors.length === 1) return t(errors[0].message);
  return t("{0}. Ещё полей с ошибками: {1}.", t(errors[0].message), errors.length - 1);
}

function sectionForValidationField(field: string): RuntimeSettingsSection {
  if (['system_audio', 'microphone'].includes(field)) return 'audio';
  if (['replay_seconds', 'replay_memory_limit_mb', 'replay_mode', 'container', 'output_directory'].includes(field)) {
    return 'replay';
  }
  if (
    [
      'save_replay_hotkey',
      'toggle_recording_hotkey',
      'duplicate_hotkeys',
      'continuous_recording_enabled',
    ].includes(field)
  ) {
    return 'hotkeys';
  }
  return 'video';
}

function describeConfirmedEngine(snapshot: EngineSnapshot | null): string {
  if (!snapshot) return t('состояние службы не подтверждено');
  if (snapshot.continuousRecordingActive) return t('обычная запись идёт');
  if (snapshot.replayActive) return t('Replay включён');

  switch (snapshot.lifecycle) {
    case 'starting':
      return t('Replay включается');
    case 'recovering':
      return t('служба восстанавливает запись');
    case 'degraded':
      return t('служба работает с ошибками');
    case 'blocked':
      return t('запуск заблокирован');
    case 'failed':
      return t('служба остановлена из-за ошибки');
    case 'ready':
    case 'stopped':
    default:
      return t('Replay выключен');
  }
}

export function RuntimeSettings({
  hostBridge,
  activeConfig,
  snapshot,
  onApply,
  onStopEngine,
  onStartEngine,
  onSnapshotChange,
  onDirtyChange,
  onBusyChange,
  onClose,
  closeRequestToken = 0,
  externalCloseRequested = false,
  initialSection = 'video',
  sidebarNavigation = false,
  onSectionChange,
}: RuntimeSettingsProps) {
  useTranslation();
  const [section, updateSection] = useState<RuntimeSettingsSection>(initialSection);
  function setSection(next: RuntimeSettingsSection) { updateSection(next); onSectionChange?.(next); }
  useLayoutEffect(() => { updateSection(initialSection); }, [initialSection]);
  const [candidate, setCandidate] = useState<OnboardingDraft>(() => activeAsDraft(activeConfig));
  const [isApplying, setIsApplying] = useState(false);
  const [isCheckingDevices, setIsCheckingDevices] = useState(false);
  const [applyError, setApplyError] = useState<AppError | null>(null);
  const [operationErrorTitle, setOperationErrorTitle] = useState(
    t('Не удалось применить настройки')
  );
  const [validationErrors, setValidationErrors] = useState<ValidationError[]>([]);
  const [appliedMessage, setAppliedMessage] = useState<string | null>(null);
  const [showApplyConfirmation, setShowApplyConfirmation] = useState(false);
  const [showLeaveConfirmation, setShowLeaveConfirmation] = useState(false);
  const [hotkeyPrompt, setHotkeyPrompt] = useState<HotkeyPrompt | null>(null);
  const [hotkeyDialogReplayActive, setHotkeyDialogReplayActive] = useState<boolean | null>(null);
  const [isStoppingForHotkey, setIsStoppingForHotkey] = useState(false);
  const [stoppedForHotkeys, setStoppedForHotkeys] = useState(false);
  const [replayWasActiveBeforeHotkeyStop, setReplayWasActiveBeforeHotkeyStop] = useState<
    boolean | null
  >(null);
  const [isResuming, setIsResuming] = useState(false);
  const [closeAfterApply, setCloseAfterApply] = useState(false);
  const lastCloseRequestTokenRef = useRef(closeRequestToken);
  const applyInFlightRef = useRef(false);
  const deviceCheckInFlightRef = useRef(false);
  const hotkeyStopInFlightRef = useRef(false);
  const resumeInFlightRef = useRef(false);
  const editorRef = useRef<HTMLDivElement>(null);

  const activeConfigSignature = useMemo(() => JSON.stringify(activeConfig), [activeConfig]);
  const lastActiveConfigSignatureRef = useRef(activeConfigSignature);
  const baseline = useMemo(() => activeAsDraft(activeConfig), [activeConfig]);
  const isDirty = JSON.stringify(candidate) !== JSON.stringify(baseline);
  const hotkeysDifferFromActive = Boolean(
    !candidate.save_replay_hotkey ||
      !candidate.toggle_recording_hotkey ||
      !areHotkeysEqual(candidate.save_replay_hotkey, activeConfig.save_replay_hotkey) ||
      !areHotkeysEqual(
        candidate.toggle_recording_hotkey,
        activeConfig.toggle_recording_hotkey
      )
  );

  useEffect(() => {
    if (activeConfigSignature === lastActiveConfigSignatureRef.current) return;
    lastActiveConfigSignatureRef.current = activeConfigSignature;
    setCandidate(activeAsDraft(activeConfig));
    setValidationErrors([]);
    setApplyError(null);
  }, [activeConfig, activeConfigSignature]);

  useEffect(() => {
    onDirtyChange?.(isDirty);
  }, [isDirty, onDirtyChange]);

  useEffect(() => {
    onBusyChange?.(
      Boolean(hotkeyPrompt) ||
        isApplying ||
        isCheckingDevices ||
        isStoppingForHotkey ||
        isResuming
    );
  }, [
    hotkeyPrompt,
    isApplying,
    isCheckingDevices,
    isResuming,
    isStoppingForHotkey,
    onBusyChange,
  ]);

  useEffect(() => {
    if (closeRequestToken === lastCloseRequestTokenRef.current) return;
    lastCloseRequestTokenRef.current = closeRequestToken;
    requestClose();
  }, [closeRequestToken]);

  useEffect(() => {
    if (!externalCloseRequested) return;
    setShowApplyConfirmation(false);
    setShowLeaveConfirmation(false);
    setCloseAfterApply(false);
    if (hotkeyPrompt) {
      hotkeyPrompt.resolve(false);
      setHotkeyPrompt(null);
    }
  }, [externalCloseRequested, hotkeyPrompt]);

  useEffect(() => {
    const firstError = validationErrors[0];
    if (!firstError || sectionForValidationField(firstError.field) !== section) return;
    const frame = window.requestAnimationFrame(() => {
      const selector = validationFieldSelectors[firstError.field];
      editorRef.current?.querySelector<HTMLElement>(selector)?.focus();
    });
    return () => window.cancelAnimationFrame(frame);
  }, [section, validationErrors]);

  function updateCandidate(patch: Partial<OnboardingDraft>) {
    setCandidate((current) => ({ ...current, ...patch }));
    const changedFields = new Set(Object.keys(patch));
    setValidationErrors((current) =>
      current.filter((error) => {
        if (changedFields.has(error.field)) return false;
        if (
          error.field === 'duplicate_hotkeys' &&
          (changedFields.has('save_replay_hotkey') ||
            changedFields.has('toggle_recording_hotkey'))
        ) {
          return false;
        }
        return true;
      })
    );
    setApplyError(null);
    setAppliedMessage(null);
  }

  async function performApply(shouldClose = false) {
    if (applyInFlightRef.current) return;
    const errors = validateCompleteDraft(candidate);
    if (errors.length > 0) {
      setValidationErrors(errors);
      setSection(sectionForValidationField(errors[0].field));
      setShowApplyConfirmation(false);
      return;
    }

    applyInFlightRef.current = true;
    setIsApplying(true);
    setApplyError(null);
    setOperationErrorTitle('Не удалось применить настройки');
    setAppliedMessage(null);
    try {
      await onApply(draftAsActive(candidate));
      setAppliedMessage('Настройки применены. Replay включён с новой конфигурацией.');
      setStoppedForHotkeys(false);
      setReplayWasActiveBeforeHotkeyStop(null);
      setShowApplyConfirmation(false);
      setShowLeaveConfirmation(false);
      setCloseAfterApply(false);
      if (shouldClose) onClose?.();
    } catch (cause) {
      setApplyError(normalizeError(cause));
      setShowApplyConfirmation(false);
      setShowLeaveConfirmation(false);
      setCloseAfterApply(false);
    } finally {
      applyInFlightRef.current = false;
      setIsApplying(false);
    }
  }

  async function validateCandidateDevices(): Promise<boolean> {
    if (deviceCheckInFlightRef.current) return false;
    deviceCheckInFlightRef.current = true;
    setIsCheckingDevices(true);
    setApplyError(null);
    setOperationErrorTitle('Не удалось проверить устройства');

    try {
      const [monitorResult, audioResult] = await Promise.allSettled([
        hostBridge.listMonitors(),
        hostBridge.listAudioEndpoints(),
      ]);

      if (monitorResult.status === 'rejected') {
        setSection('video');
        setOperationErrorTitle('Не удалось проверить доступные экраны');
        setApplyError(normalizeError(monitorResult.reason));
        return false;
      }

      const deviceErrors: ValidationError[] = [];
      if (
        !candidate.monitor_id ||
        !monitorResult.value.some((monitor) => monitor.id === candidate.monitor_id)
      ) {
        deviceErrors.push({
          field: 'monitor_id',
          code: 'unavailable',
          message: 'Выбранный экран сейчас недоступен. Выберите другой экран.',
        });
      }

      if (deviceErrors.length > 0) {
        setValidationErrors(deviceErrors);
        setSection('video');
        return false;
      }

      const systemEndpoint =
        candidate.system_audio && typeof candidate.system_audio === 'object'
          ? candidate.system_audio.endpoint
          : null;
      const microphoneEndpoint =
        candidate.microphone && typeof candidate.microphone === 'object'
          ? candidate.microphone.endpoint
          : null;
      const needsAudioCatalog = Boolean(systemEndpoint || microphoneEndpoint);

      if (audioResult.status === 'rejected' && needsAudioCatalog) {
        setSection('audio');
        setOperationErrorTitle('Не удалось проверить аудиоустройства');
        setApplyError(normalizeError(audioResult.reason));
        return false;
      }

      if (audioResult.status === 'fulfilled') {
        if (
          systemEndpoint &&
          !audioResult.value.system_audio.some(
            (choice) => choice.id === systemEndpoint && choice.available
          )
        ) {
          deviceErrors.push({
            field: 'system_audio',
            code: 'unavailable',
            message:
              'Выбранное устройство звука компьютера недоступно. Выберите другое или отключите звук.',
          });
        }
        if (
          microphoneEndpoint &&
          !audioResult.value.microphones.some(
            (choice) => choice.id === microphoneEndpoint && choice.available
          )
        ) {
          deviceErrors.push({
            field: 'microphone',
            code: 'unavailable',
            message:
              'Выбранный микрофон недоступен. Выберите другой или отключите микрофон.',
          });
        }
      }

      if (deviceErrors.length > 0) {
        setValidationErrors(deviceErrors);
        setSection(sectionForValidationField(deviceErrors[0].field));
        return false;
      }

      return true;
    } finally {
      deviceCheckInFlightRef.current = false;
      setIsCheckingDevices(false);
    }
  }

  async function requestApply(shouldClose = false) {
    if (isCheckingDevices || deviceCheckInFlightRef.current) return;
    const errors = validateCompleteDraft(candidate);
    if (errors.length > 0) {
      setValidationErrors(errors);
      setSection(sectionForValidationField(errors[0].field));
      return;
    }
    setValidationErrors([]);
    if (!(await validateCandidateDevices())) return;
    if (!snapshot || snapshot.continuousRecordingActive) {
      setCloseAfterApply(shouldClose);
      setShowApplyConfirmation(true);
      return;
    }
    await performApply(shouldClose);
  }

  function resetCandidate() {
    setCandidate(activeAsDraft(activeConfig));
    setValidationErrors([]);
    setApplyError(null);
    setAppliedMessage(null);
  }

  function requestClose() {
    if (isApplying || isCheckingDevices || isStoppingForHotkey || isResuming) return;
    if (isDirty) {
      setShowLeaveConfirmation(true);
    } else {
      void discardAndClose();
    }
  }

  function beforeHotkeyCapture(target: HotkeyTarget): Promise<boolean> {
    const engineIsRunning =
      !snapshot ||
      snapshot.replayActive ||
      snapshot.continuousRecordingActive ||
      !['stopped', 'blocked', 'failed'].includes(snapshot.lifecycle);
    if (!engineIsRunning) return Promise.resolve(true);

    setHotkeyDialogReplayActive(snapshot ? snapshot.replayActive : null);
    return new Promise<boolean>((resolve) => {
      setHotkeyPrompt({
        target,
        replayWasActive: snapshot ? snapshot.replayActive : null,
        resolve,
      });
    });
  }

  async function confirmHotkeyStop() {
    if (!hotkeyPrompt || hotkeyStopInFlightRef.current) return;
    hotkeyStopInFlightRef.current = true;
    setIsStoppingForHotkey(true);
    setApplyError(null);
    setOperationErrorTitle(
      hotkeyPrompt.replayWasActive === true
        ? 'Не удалось временно выключить Replay'
        : 'Не удалось остановить службу записи'
    );
    try {
      const nextSnapshot = await (onStopEngine?.() ?? hostBridge.stopEngine());
      onSnapshotChange?.(nextSnapshot);
      setStoppedForHotkeys(true);
      setReplayWasActiveBeforeHotkeyStop(hotkeyPrompt.replayWasActive);
      hotkeyPrompt.resolve(true);
      setHotkeyPrompt(null);
    } catch (cause) {
      setApplyError(normalizeError(cause));
      hotkeyPrompt.resolve(false);
      setHotkeyPrompt(null);
    } finally {
      hotkeyStopInFlightRef.current = false;
      setIsStoppingForHotkey(false);
    }
  }

  function cancelHotkeyStop() {
    hotkeyPrompt?.resolve(false);
    setHotkeyPrompt(null);
  }

  async function resumeReplay(): Promise<boolean> {
    if (resumeInFlightRef.current) return false;
    resumeInFlightRef.current = true;
    setIsResuming(true);
    setApplyError(null);
    setOperationErrorTitle('Не удалось включить Replay');
    try {
      const nextSnapshot = await (onStartEngine?.() ?? hostBridge.startEngine());
      onSnapshotChange?.(nextSnapshot);
      setStoppedForHotkeys(false);
      setReplayWasActiveBeforeHotkeyStop(null);
      return true;
    } catch (cause) {
      setApplyError(normalizeError(cause));
      return false;
    } finally {
      resumeInFlightRef.current = false;
      setIsResuming(false);
    }
  }

  async function discardAndClose() {
    resetCandidate();
    setShowLeaveConfirmation(false);
    if (
      stoppedForHotkeys &&
      replayWasActiveBeforeHotkeyStop === true &&
      !(await resumeReplay())
    ) {
      return;
    }
    onClose?.();
  }

  const hotkeyStopNoticeTitle =
    replayWasActiveBeforeHotkeyStop === true
      ? hotkeysDifferFromActive
        ? t('Replay выключен до применения настроек')
        : t('Replay временно выключен')
      : hotkeysDifferFromActive
        ? t('Служба записи остановлена до применения настроек')
        : t('Служба записи остановлена');

  const hotkeyStopNoticeMessage =
    replayWasActiveBeforeHotkeyStop === true
      ? hotkeysDifferFromActive
        ? t('Примените изменения или верните действующие значения.')
        : t('Глобальные клавиши не сработают, пока вы редактируете сочетание.')
      : replayWasActiveBeforeHotkeyStop === false
        ? hotkeysDifferFromActive
          ? t('Изменения ещё не применены. Replay останется выключен, как и до редактирования.')
          : t('Replay был выключен до редактирования и автоматически не включится.')
        : t('Прежнее состояние Replay не подтверждено, поэтому он не будет включён автоматически.');

  return (
    <section
      className={styles.settings}
      data-testid="runtime-settings"
      aria-busy={
        isApplying || isCheckingDevices || isStoppingForHotkey || isResuming || undefined
      }
      inert={
        isApplying || isCheckingDevices || isStoppingForHotkey || isResuming
          ? true
          : undefined
      }
    >
      <h1 className="sr-only">{t("Настройки записи")}</h1>

      <div className={styles.settingsGrid}>
        {!sidebarNavigation && <nav aria-label={t("Разделы настроек")}>
          <ToggleGroup type="single" value={section} variant="outline" spacing={2}
            className="flex w-full flex-wrap justify-start" aria-label={t("Разделы настроек")}
            onValueChange={(value) => {
              if (sections.some((item) => item.id === value)) setSection(value as RuntimeSettingsSection);
            }}>
            {sections.map((item) => (
              <ToggleGroupItem key={item.id} value={item.id} className="min-h-11 h-auto px-4 py-2 whitespace-normal">
                {t(item.label)}
              </ToggleGroupItem>
            ))}
          </ToggleGroup>
        </nav>}

        <div ref={editorRef} className={styles.editor}>
          {section === 'application' && <ApplicationSettings />}
          {section === 'video' && (
            <VideoStep
              draft={candidate}
              hostBridge={hostBridge}
              onChange={updateCandidate}
              errors={validationErrors}
            />
          )}
          {section === 'audio' && (
            <AudioStep
              liveLevels
              draft={candidate}
              hostBridge={hostBridge}
              onChange={updateCandidate}
              errors={validationErrors}
            />
          )}
          {section === 'replay' && (
            <ReplayStep
              memoryBudgetBytes={snapshot?.metrics.budgetBytes}
              draft={candidate}
              hostBridge={hostBridge}
              onChange={updateCandidate}
              errors={validationErrors}
            />
          )}
          {section === 'hotkeys' && (
            <HotkeysStep
              draft={candidate}
              onChange={updateCandidate}
              errors={validationErrors}
              onBeforeCapture={beforeHotkeyCapture}
            />
          )}
        </div>
      </div>

      <div className={styles.messages}>
        {validationErrors.length > 0 && (
          <ErrorNotice title={t("Проверьте настройки")} tone="warning">
            {firstValidationMessage(validationErrors)}
          </ErrorNotice>
        )}
        {applyError && (
          <ErrorNotice
            title={t(operationErrorTitle)}
            technicalDetails={t("Код: {0}\n{1}", applyError.code, applyError.technicalCause)}
            action={
              isDirty && operationErrorTitle === 'Не удалось применить настройки' ? (
                <Button variant="tertiary" size="compact" onClick={resetCandidate}>
                  {t("Вернуть действующие значения")}</Button>
              ) : undefined
            }
          >
            <p>{t(applyError.summary)} {' '}{t("Введённые значения сохранены в форме.")}</p>
            <p>
              {t("Последний подтверждённый профиль:")}{' '}
              {formatVideoMode(
                activeConfig.width,
                activeConfig.height,
                activeConfig.fps,
                activeConfig.bitrate
              )}; {describeConfirmedEngine(snapshot)}.
            </p>
          </ErrorNotice>
        )}
        {appliedMessage && <InlineStatus tone="success">{appliedMessage}</InlineStatus>}
        {stoppedForHotkeys && (
          <ErrorNotice
            tone="neutral"
            title={hotkeyStopNoticeTitle}
            action={
              replayWasActiveBeforeHotkeyStop === true && !hotkeysDifferFromActive ? (
                <Button variant="secondary" busy={isResuming} onClick={() => void resumeReplay()}>
                  {t("Включить Replay снова")}</Button>
              ) : undefined
            }
          >
            {hotkeyStopNoticeMessage}
          </ErrorNotice>
        )}
      </div>

      {isDirty && <div className={styles.restartNotice}>
        {t("Replay перезапустится; несохранённый повтор будет потерян.")}</div>}

      {(section !== 'application' || isDirty) && <div className={styles.actions}>
        <Button
          variant="secondary"
          onClick={resetCandidate}
          disabled={!isDirty || isApplying || isCheckingDevices}
          data-testid="cancel-settings-button"
        >
          {t("Сбросить изменения")}</Button>
        <Button
          variant="primary"
          busy={isApplying || isCheckingDevices}
          busyLabel={isCheckingDevices ? t('Проверяем устройства…') : t('Применяем…')}
          onClick={() => void requestApply(false)}
          disabled={!isDirty}
          data-testid="apply-settings-button"
        >
          {t("Применить")}</Button>
      </div>}

      <ConfirmDialog
        open={showApplyConfirmation}
        title={
          snapshot?.continuousRecordingActive
            ? t('Остановить запись и применить настройки?')
            : t('Применить настройки при неподтверждённом состоянии?')
        }
        description={
          snapshot
            ? t('Текущая обычная запись завершится. Replay запустится с новой конфигурацией, но обычная запись автоматически не возобновится.')
            : t('Состояние службы не подтверждено. Если обычная запись идёт, она завершится. Replay запустится с новой конфигурацией после подтверждения службы записи.')
        }
        confirmLabel={
          snapshot?.continuousRecordingActive
            ? t('Остановить и применить')
            : t('Применить и включить Replay')
        }
        cancelLabel={
          snapshot?.continuousRecordingActive
            ? t('Продолжить запись')
            : t('Продолжить редактирование')
        }
        destructive
        busy={isApplying}
        busyLabel={t("Применяем…")}
        onConfirm={() => void performApply(closeAfterApply)}
        onCancel={() => {
          setShowApplyConfirmation(false);
          setCloseAfterApply(false);
        }}
      />

      <ConfirmDialog
        open={Boolean(hotkeyPrompt)}
        title={
          hotkeyDialogReplayActive === true
            ? t('Временно выключить Replay?')
            : t('Остановить службу записи для изменения сочетания?')
        }
        description={
          !snapshot
            ? t('Состояние службы не подтверждено. Перед перехватом сочетания безопасно остановим Replay и возможную текущую запись.')
            : snapshot.continuousRecordingActive
              ? hotkeyDialogReplayActive === true
                ? t('Чтобы безопасно перехватить новое сочетание, нужно остановить Replay и завершить текущую запись.')
                : t('Чтобы безопасно перехватить новое сочетание, нужно завершить текущую запись и остановить службу записи.')
              : hotkeyDialogReplayActive === true
                ? t('Чтобы глобальная горячая клавиша не сработала во время редактирования, нужно временно выключить Replay.')
                : t('Чтобы безопасно перехватить новое сочетание, нужно временно остановить службу записи.')
        }
        confirmLabel={
          hotkeyDialogReplayActive === true
            ? t('Выключить и изменить')
            : t('Остановить и изменить')
        }
        cancelLabel={t("Отмена")}
        destructive={Boolean(snapshot?.continuousRecordingActive)}
        busy={isStoppingForHotkey}
        busyLabel={t("Выключаем…")}
        onConfirm={() => void confirmHotkeyStop()}
        onCancel={cancelHotkeyStop}
      />

      <ConfirmDialog
        open={showLeaveConfirmation}
        title={t("Сохранить изменения?")}
        description={t("Настройки в форме ещё не применены.")}
        confirmLabel={t("Применить")}
        cancelLabel={t("Продолжить редактирование")}
        busy={isApplying}
        busyLabel={t("Применяем…")}
        onConfirm={() => {
          setShowLeaveConfirmation(false);
          void requestApply(true);
        }}
        onCancel={() => setShowLeaveConfirmation(false)}
      >
        <Button
          variant="tertiary"
          busy={isResuming}
          busyLabel={t("Возобновляем Replay…")}
          disabled={isApplying}
          onClick={() => void discardAndClose()}
        >
          {t("Не сохранять")}</Button>
      </ConfirmDialog>
    </section>
  );
}
