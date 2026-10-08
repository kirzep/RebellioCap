import React, { useEffect, useMemo, useRef, useState } from 'react';
import { ArrowLeft, ArrowRight } from 'lucide-react';
import { useWindowCloseGuard } from '../app/useWindowCloseGuard';
import type { AppError, HostBridge } from '../bridge/contracts';
import { normalizeError } from '../bridge/host';
import { Button, ConfirmDialog, ErrorNotice, InlineStatus } from '../components/Primitives';
import { SetupShell } from './SetupShell';
import type { Bootstrap, OnboardingDraft } from '../config/model';
import type { ValidationError } from '../config/validation';
import { createOnboardingMachine, type OnboardingMachine, type OnboardingState } from './machine';
import styles from './OnboardingScreen.module.css';
import { AudioStep } from './steps/AudioStep';
import { HotkeysStep } from './steps/HotkeysStep';
import {
  PreferencesStep,
  type SummaryBlockingIssue,
  type SummaryEditStep,
} from './steps/PreferencesStep';
import { ReplayStep } from './steps/ReplayStep';
import { TestStep } from './steps/TestStep';
import { VideoStep } from './steps/VideoStep';

export interface OnboardingScreenProps {
  hostBridge: HostBridge;
  initialDraft?: OnboardingDraft;
  initialStep?: number;
  initialLastCompletedStep?: number;
  onComplete: (result: Bootstrap) => void | Promise<void>;
  onBackToStart?: () => void;
}

type StepDirection = 'forward' | 'backward' | 'steady';

const nextLabels: Record<number, string> = {
  1: 'Продолжить',
  2: 'Продолжить',
  3: 'Продолжить',
  4: 'Проверить настройки',
  5: 'Проверить запись',
};

const validationFieldSelectors: Record<string, string> = {
  monitor_id: '#video-monitor',
  width: '#video-width',
  height: '#video-height',
  fps: '#video-fps, #video-custom-fps',
  bitrate: '#video-bitrate',
  system_audio: '#audio-system',
  microphone: '#audio-microphone',
  replay_seconds: '#replay-duration-preset, #replay-seconds',
  replay_mode: '[data-testid="replay-mode-ram-button"]',
  container: '[data-testid="container-mp4-button"]',
  output_directory: '#output-directory',
  save_replay_hotkey: '[data-testid="record-replay-hotkey-button"]',
  toggle_recording_hotkey: '[data-testid="record-recording-hotkey-button"]',
  duplicate_hotkeys: '[data-testid="record-replay-hotkey-button"]',
  continuous_recording_enabled: '[data-testid="continuous-recording-checkbox"]',
};

function validationStep(field: string): SummaryEditStep | 5 {
  if (['monitor_id', 'width', 'height', 'fps', 'bitrate'].includes(field)) return 1;
  if (['system_audio', 'microphone'].includes(field)) return 2;
  if (['replay_seconds', 'replay_mode', 'container', 'output_directory'].includes(field)) return 3;
  if (
    [
      'save_replay_hotkey',
      'toggle_recording_hotkey',
      'duplicate_hotkeys',
      'continuous_recording_enabled',
    ].includes(field)
  ) {
    return 4;
  }
  return 5;
}

function draftFingerprint(draft: OnboardingDraft): string {
  return JSON.stringify(draft);
}

function engineNeedsPause(lifecycle: string, replayActive: boolean, recordingActive: boolean) {
  return (
    replayActive ||
    recordingActive ||
    !['stopped', 'blocked', 'failed'].includes(lifecycle)
  );
}

function completionResultError(result: Bootstrap): AppError | null {
  const completedState =
    result.route === 'home' &&
    result.state.onboarding.completed &&
    result.state.onboarding.last_completed_step === 6 &&
    Boolean(result.state.active?.onboarding_completed);
  const replayStarted =
    result.snapshot.lifecycle === 'ready' && result.snapshot.replayActive;

  if (completedState && replayStarted) return null;

  return {
    code: 'onboarding.completion_not_confirmed',
    summary: 'Служба записи не подтвердила завершение настройки и запуск Replay.',
    subsystem: 'engine',
    technicalCause: [
      `route=${result.route}`,
      `completed=${result.state.onboarding.completed}`,
      `last_completed_step=${result.state.onboarding.last_completed_step}`,
      `active=${Boolean(result.state.active?.onboarding_completed)}`,
      `lifecycle=${result.snapshot.lifecycle}`,
      `replay_active=${result.snapshot.replayActive}`,
    ].join('; '),
    retryable: true,
    actions: ['retry', 'diagnostics'],
  };
}

export function OnboardingScreen({
  hostBridge,
  initialDraft = {},
  initialStep = 1,
  initialLastCompletedStep = 0,
  onComplete,
  onBackToStart,
}: OnboardingScreenProps) {
  const [machine] = useState<OnboardingMachine>(() =>
    createOnboardingMachine(
      hostBridge,
      initialDraft,
      initialStep,
      initialLastCompletedStep
    )
  );
  const [state, setState] = useState<OnboardingState>(() => machine.snapshot());
  const [direction, setDirection] = useState<StepDirection>('steady');
  const [operationError, setOperationError] = useState<AppError | null>(null);
  const [testBusy, setTestBusy] = useState(false);
  const [testFooter, setTestFooter] = useState<HTMLDivElement | null>(null);
  const [hotkeyCaptureActive, setHotkeyCaptureActive] = useState(false);
  const [hotkeyEngineTransition, setHotkeyEngineTransition] = useState(false);
  const [hotkeyEngineNotice, setHotkeyEngineNotice] = useState<string | null>(null);
  const [showExitConfirmation, setShowExitConfirmation] = useState(false);
  const [summaryBusy, setSummaryBusy] = useState(false);
  const [summaryBlockingIssue, setSummaryBlockingIssue] =
    useState<SummaryBlockingIssue | null>(null);
  const [videoCatalogBusy, setVideoCatalogBusy] = useState(false);
  const [videoAvailabilityIssue, setVideoAvailabilityIssue] = useState<string | null>(null);
  const [audioCatalogBusy, setAudioCatalogBusy] = useState(false);
  const [audioAvailabilityIssue, setAudioAvailabilityIssue] = useState<string | null>(null);
  const previousStepRef = useRef(state.step);
  const stageRef = useRef<HTMLDivElement>(null);
  const validationNoticeRef = useRef<HTMLDivElement>(null);
  const engineStoppedForCaptureRef = useRef(false);
  const resumeReplayAfterCaptureRef = useRef(false);
  const hotkeyEngineTransitionRef = useRef(false);
  const persistedDraftRef = useRef(draftFingerprint(state.draft));

  useEffect(() => {
    return machine.subscribe((nextState) => {
      if (nextState.step !== previousStepRef.current) {
        setDirection(nextState.step > previousStepRef.current ? 'forward' : 'backward');
        previousStepRef.current = nextState.step;
      }
      setState(nextState);
    });
  }, [machine]);

  useEffect(() => {
    const frame = window.requestAnimationFrame(() => {
      const heading = stageRef.current?.querySelector<HTMLElement>('h2');
      if (!heading) return;
      heading.tabIndex = -1;
      heading.focus();
    });
    return () => window.cancelAnimationFrame(frame);
  }, [state.step]);

  useEffect(() => {
    if (state.step !== 4) setHotkeyEngineNotice(null);
  }, [state.step]);

  const navigationLocked =
    state.isSaving || testBusy || summaryBusy || hotkeyCaptureActive || hotkeyEngineTransition;
  const deviceNavigationBlocked =
    (state.step === 1 && (videoCatalogBusy || Boolean(videoAvailabilityIssue))) ||
    (state.step === 2 && (audioCatalogBusy || Boolean(audioAvailabilityIssue)));
  const hasUnsavedChanges = draftFingerprint(state.draft) !== persistedDraftRef.current;
  const closeGuard = useWindowCloseGuard({
    shouldConfirm: hasUnsavedChanges || navigationLocked,
    onBeforeClose: async () => {
      // Cancels an in-flight audio sample when present. Native shutdown remains
      // responsible for finalizing any recording/test operation owned by the host.
      await hostBridge.measureAudioLevel(null).catch(() => undefined);
    },
  });
  useEffect(() => {
    if (closeGuard.closeRequested) setShowExitConfirmation(false);
  }, [closeGuard.closeRequested]);
  const animationClass =
    direction === 'forward'
      ? styles.stageForward
      : direction === 'backward'
        ? styles.stageBackward
        : styles.stageSteady;

  const validationGroups = useMemo(() => {
    const groups = new Map<number, ValidationError[]>();
    for (const error of state.errors) {
      const step = validationStep(error.field);
      groups.set(step, [...(groups.get(step) ?? []), error]);
    }
    return [...groups.entries()];
  }, [state.errors]);

  function focusValidationError(errors: ValidationError[]) {
    const selector = errors[0] ? validationFieldSelectors[errors[0].field] : undefined;
    window.requestAnimationFrame(() => {
      const field = selector
        ? stageRef.current?.querySelector<HTMLElement>(selector)
        : undefined;
      (field ?? validationNoticeRef.current)?.focus();
    });
  }

  function handleMachineError(error: unknown) {
    const nextState = machine.snapshot();
    if (nextState.errors.length > 0) {
      setOperationError(null);
      focusValidationError(nextState.errors);
      return;
    }
    setOperationError(normalizeError(error));
  }

  function rememberPersistedDraftIfSaved() {
    const snapshot = machine.snapshot();
    if (snapshot.errors.length === 0) {
      persistedDraftRef.current = draftFingerprint(snapshot.draft);
    }
  }

  async function navigateToStep(target: number) {
    if (navigationLocked || target === state.step) return;
    if (
      (state.step === 1 || state.step === 2) &&
      target > state.step &&
      deviceNavigationBlocked
    ) {
      return;
    }
    setOperationError(null);
    try {
      await machine.goToStep(target);
      rememberPersistedDraftIfSaved();
    } catch (error) {
      handleMachineError(error);
    }
  }

  async function handleNext() {
    if (navigationLocked || state.step >= 6) return;
    if (deviceNavigationBlocked) return;
    setOperationError(null);
    if (state.step === 5 && summaryBlockingIssue) return;
    try {
      await machine.next();
      rememberPersistedDraftIfSaved();
    } catch (error) {
      handleMachineError(error);
    }
  }

  async function handleBack() {
    if (navigationLocked) return;
    setOperationError(null);

    if (state.step === 1) {
      if (!onBackToStart) return;
      if (hasUnsavedChanges) {
        setShowExitConfirmation(true);
      } else {
        onBackToStart();
      }
      return;
    }

    try {
      await machine.back();
      rememberPersistedDraftIfSaved();
    } catch (error) {
      handleMachineError(error);
    }
  }

  async function prepareHotkeyCapture(): Promise<boolean> {
    if (
      state.isSaving ||
      testBusy ||
      hotkeyCaptureActive ||
      hotkeyEngineTransitionRef.current
    ) {
      return false;
    }

    hotkeyEngineTransitionRef.current = true;
    setHotkeyEngineTransition(true);
    setHotkeyEngineNotice('Проверяем состояние глобальных горячих клавиш…');
    setOperationError(null);

    try {
      const snapshot = await hostBridge.getEngineSnapshot();
      const shouldStop = engineNeedsPause(
        snapshot.lifecycle,
        snapshot.replayActive,
        snapshot.continuousRecordingActive
      );

      if (shouldStop) {
        await hostBridge.stopEngine();
        engineStoppedForCaptureRef.current = true;
        resumeReplayAfterCaptureRef.current = snapshot.replayActive;
        setHotkeyEngineNotice(
          snapshot.replayActive
            ? 'Replay временно остановлен, чтобы старое сочетание не сработало во время ввода.'
            : 'Служба записи остановлена, чтобы глобальное сочетание не сработало во время ввода.'
        );
      } else {
        engineStoppedForCaptureRef.current = false;
        resumeReplayAfterCaptureRef.current = false;
        setHotkeyEngineNotice('Введите новое сочетание клавиш.');
      }

      setHotkeyCaptureActive(true);
      return true;
    } catch (error) {
      engineStoppedForCaptureRef.current = false;
      resumeReplayAfterCaptureRef.current = false;
      setHotkeyEngineNotice(null);
      setOperationError(normalizeError(error));
      return false;
    } finally {
      hotkeyEngineTransitionRef.current = false;
      setHotkeyEngineTransition(false);
    }
  }

  async function finishHotkeyCapture() {
    setHotkeyCaptureActive(false);
    const engineWasStopped = engineStoppedForCaptureRef.current;
    const shouldRestart =
      engineWasStopped && resumeReplayAfterCaptureRef.current;
    engineStoppedForCaptureRef.current = false;
    resumeReplayAfterCaptureRef.current = false;

    if (!shouldRestart || hotkeyEngineTransitionRef.current) {
      setHotkeyEngineNotice(
        engineWasStopped ? 'Служба записи остаётся выключенной.' : null
      );
      return;
    }

    hotkeyEngineTransitionRef.current = true;
    setHotkeyEngineTransition(true);
    setHotkeyEngineNotice('Возобновляем Replay после изменения сочетания…');
    try {
      await hostBridge.startEngine();
      setHotkeyEngineNotice('Replay снова работает.');
    } catch (error) {
      setHotkeyEngineNotice(null);
      setOperationError(normalizeError(error));
    } finally {
      hotkeyEngineTransitionRef.current = false;
      setHotkeyEngineTransition(false);
    }
  }

  function updateDraft(patch: Partial<OnboardingDraft>) {
    setOperationError(null);
    machine.updateDraft(patch);
  }

  function handleStageClickCapture(event: React.MouseEvent<HTMLDivElement>) {
    if (state.step !== 6 || !(event.target instanceof Element)) return;
    if (
      event.target.closest(
        '[data-testid="run-test-button"], [data-testid="complete-onboarding-button"]'
      )
    ) {
      setTestBusy(true);
    }
  }

  async function completeOnboarding() {
    const result = await hostBridge.completeOnboarding(true);
    const completionError = completionResultError(result);
    if (completionError) throw completionError;
    await onComplete(result);
  }

  const footer =
    state.step < 6 ? (
      <div className={styles.footerActions}>
        <Button
          variant="tertiary"
          leadingIcon={<ArrowLeft size={18} />}
          onClick={() => void handleBack()}
          disabled={navigationLocked || (state.step === 1 && !onBackToStart)}
          data-testid="onboarding-back-button"
        >
          {state.step === 1 ? 'К началу' : 'Назад'}
        </Button>

        <div className={styles.footerRight}>
          {state.isSaving && (
            <InlineStatus tone="busy" className={styles.saveStatus}>
              Сохраняем настройки…
            </InlineStatus>
          )}
          <Button
            variant="primary"
            trailingIcon={<ArrowRight size={18} />}
            busy={state.isSaving}
            busyLabel="Сохраняем…"
            onClick={() => void handleNext()}
            disabled={
              navigationLocked ||
              deviceNavigationBlocked
            }
            data-testid="onboarding-next-button"
          >
            {nextLabels[state.step] ?? 'Продолжить'}
          </Button>
        </div>
      </div>
    ) : <div ref={setTestFooter} className={styles.testFooter} />;

  return (
    <SetupShell
      step={state.step}
      lastCompletedStep={state.lastCompletedStep}
      onSelectStep={navigationLocked || deviceNavigationBlocked ? undefined : (target) => void navigateToStep(target)}
      footer={footer}
    >
      <div className={styles.setupLayout} data-testid="onboarding-panel">
        <h1 className="sr-only">Настройка записи</h1>

        <div className={styles.formColumn}>
          <div
            key={state.step}
            ref={stageRef}
            className={`${styles.stage} ${animationClass}`}
            aria-busy={state.isSaving || testBusy || summaryBusy || undefined}
            inert={state.isSaving || hotkeyEngineTransition ? true : undefined}
            onClickCapture={handleStageClickCapture}
          >
            {state.step === 1 && (
              <VideoStep
                guided
                draft={state.draft}
                hostBridge={hostBridge}
                onChange={updateDraft}
                errors={state.errors}
                onAvailabilityIssueChange={setVideoAvailabilityIssue}
                onCatalogBusyChange={setVideoCatalogBusy}
              />
            )}
            {state.step === 2 && (
              <AudioStep
                liveLevels
                draft={state.draft}
                hostBridge={hostBridge}
                onChange={updateDraft}
                errors={state.errors}
                onAvailabilityIssueChange={setAudioAvailabilityIssue}
                onCatalogBusyChange={setAudioCatalogBusy}
              />
            )}
            {state.step === 3 && (
              <ReplayStep
                draft={state.draft}
                hostBridge={hostBridge}
                onChange={updateDraft}
                errors={state.errors}
              />
            )}
            {state.step === 4 && (
              <HotkeysStep
                draft={state.draft}
                onChange={updateDraft}
                errors={state.errors}
                onBeforeCapture={prepareHotkeyCapture}
                onCaptureEnd={() => void finishHotkeyCapture()}
              />
            )}
            {state.step === 5 && (
              <PreferencesStep
                draft={state.draft}
                hostBridge={hostBridge}
                onChange={updateDraft}
                onEditStep={summaryBusy ? undefined : (target) => void navigateToStep(target)}
                onBlockingIssueChange={setSummaryBlockingIssue}
                onBusyChange={setSummaryBusy}
              />
            )}
            {state.step === 6 && (
              <TestStep
                footerTarget={testFooter}
                draft={state.draft}
                hostBridge={hostBridge}
                onBack={() => void handleBack()}
                onBusyChange={setTestBusy}
                onComplete={completeOnboarding}
              />
            )}
          </div>

          {hotkeyEngineNotice && state.step === 4 && (
            <InlineStatus
              tone={hotkeyEngineTransition ? 'busy' : 'neutral'}
              className={styles.runtimeNotice}
            >
              {hotkeyEngineNotice}
            </InlineStatus>
          )}

          {state.errors.length > 0 && validationGroups.some(([step]) => step !== state.step) && (
            <div ref={validationNoticeRef} className={styles.noticeRegion} tabIndex={-1}>
              <ErrorNotice
                title={
                  validationGroups.some(([step]) => step !== state.step)
                    ? 'Есть несохранённые изменения на другом шаге'
                    : 'Проверьте настройки перед продолжением'
                }
              >
                <div className={styles.validationContent}>
                  <ul className={styles.validationList}>
                    {state.errors.map((error) => (
                      <li key={`${error.field}-${error.code}`}>{error.message}</li>
                    ))}
                  </ul>
                  {validationGroups.some(([step]) => step !== state.step) && (
                    <div className={styles.validationLinks}>
                      {validationGroups
                        .filter(([step]) => step !== state.step)
                        .map(([step]) => (
                          <Button
                            key={step}
                            variant="tertiary"
                            size="compact"
                            onClick={() => void navigateToStep(step)}
                          >
                            Исправить на шаге {step}
                          </Button>
                        ))}
                    </div>
                  )}
                </div>
              </ErrorNotice>
            </div>
          )}

          {state.step === 5 && summaryBlockingIssue && (
            <div className={styles.noticeRegion}>
              <ErrorNotice
                title="Перед тестом нужно исправить устройство"
                tone="warning"
                action={
                  <Button
                    variant="secondary"
                    size="compact"
                    onClick={() => void navigateToStep(summaryBlockingIssue.step)}
                  >
                    Исправить
                  </Button>
                }
              >
                {summaryBlockingIssue.message}
              </ErrorNotice>
            </div>
          )}

          {operationError && (
            <div className={styles.noticeRegion}>
              <ErrorNotice
                title={operationError.summary}
                technicalDetails={`Код: ${operationError.code}\n${operationError.technicalCause}`}
              >
                Настройки остались на экране. Исправьте причину и повторите действие.
              </ErrorNotice>
            </div>
          )}
        </div>
      </div>

      <ConfirmDialog
        open={showExitConfirmation}
        title="Вернуться к началу?"
        description="Изменения на текущем шаге ещё не сохранены и будут потеряны."
        confirmLabel="Вернуться к началу"
        cancelLabel="Остаться в настройке"
        destructive
        onCancel={() => setShowExitConfirmation(false)}
        onConfirm={() => {
          setShowExitConfirmation(false);
          onBackToStart?.();
        }}
      />

      <ConfirmDialog
        open={closeGuard.closeRequested}
        title={testBusy ? 'Закрыть во время тестовой записи?' : 'Закрыть RebellioCap?'}
        description={
          testBusy
            ? 'Тест ещё не завершён и не будет считаться успешным. Служба записи будет штатно остановлена перед закрытием.'
            : navigationLocked
              ? 'Текущая операция ещё не завершена. Служба записи будет штатно остановлена, а незавершённое действие не будет считаться успешным.'
              : 'Изменения после последнего подтверждения службы записи будут потеряны.'
        }
        confirmLabel="Закрыть RebellioCap"
        cancelLabel="Остаться в настройке"
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
    </SetupShell>
  );
}
